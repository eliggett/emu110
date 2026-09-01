#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
fx_analyse.py -- measure the U-110's chorus and tremolo in a capture_env --set effects take.

    python3 tools/fx_analyse.py --hw listen/hardware/effects
    python3 tools/fx_analyse.py --hw listen/hardware/effects --emu listen/emulated/effects-emu

Everything here is a ratio, a frequency or a duty cycle, so it does not care what the
interface gain was set to -- which matters, because the hardware take and the render are
never at the same level.

The four measurements, and why each is done the way it is:

  RATE      the LFO period, from the autocorrelation of the envelope in dB.  Not from the
            modulation spectrum's largest peak: the LFO is a triangle and the organ tone has
            a ripple of its own near 18 Hz, so peak-picking lands on a harmonic about half
            the time.

  DEPTH     the tremolo is an auto-pan, so L/(L+R) is the LFO normalised by the sum of its
            two endpoints -- and the tone's own level cancels out of that ratio exactly.
            Measuring either channel on its own instead reads the organ's ripple as tremolo
            and puts depth 1 at 8 dB.

  DELAY     the chorus splits a partial into a symmetric triplet, one sideband per ramp.
            The delay slope follows from the sideband offset, and the swing in samples from
            the slope and the half period; dividing that into the level swing gives the tap
            shift.  Note the two sidebands are NOT equal in cents -- a slope s gives ratios
            1+s and 1-s, unequal by the curvature of the log -- and that inequality is what
            distinguishes a delay from a pair of detuned voices.

  ORDER     whether the delay runs before or after the pan.  With both effects on, track the
            carrier's energy and the sidebands' energy separately and band-pass both at the
            tremolo rate: if the pan multiplies the delayed copy as well as the dry one, the
            two move together and S/C is 1.  If only the dry were panned the sidebands would
            carry no tremolo at all.  Measured 0.977 with a correlation of +1.000, so the
            delay comes first -- which is also the only arrangement one 2K x 8 SRAM can
            support, since it can hold a mono signal and nothing else.

  STEREO    whether the wet signal is a polarity flip or a second tap.  Track which sideband
            is the stronger one over time in each channel: opposite (correlation -1) means
            two taps sweeping in opposite directions.

Onsets are found in the audio, not read from trials.csv, for the reason scratch_analyse.py
gives: the CSV carries session times and the segment files start at the segment.
"""

import argparse, os, sys, wave
import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(HERE, 'roms', 'roland_u110_pgm_(15179960).bin')

# The four segment-word tables, indexed by DEPTH; the RATE parameter is then added to the
# rising word and subtracted from the falling one.  See analysis/EFFECTS.md section 3.
TBL = dict(chorus=(0xAA46, 0xAA66), tremolo=(0xAA86, 0xAAA6))
ENGINE = 32000.0
# Measured: the LFO slots ramp at 2^(rate/8) * 4 per engine sample in BOTH directions --
# the falling constant either way, which is not what a voice does.  Section 4.
RAMP_SCALE = 4.0
TAP_SHIFT = 14
NOTE60 = 261.626


def words(rom, addr, n=16):
    return [rom[addr + 2 * i] | (rom[addr + 2 * i + 1] << 8) for i in range(n)]


def s8(b):
    return b - 256 if b > 127 else b


def level(log_level):
    return (2 ** 26) * 2.0 ** ((log_level - 255) / 16.0)


def segment(rom, which, depth, rate):
    """(rising word, falling word) for one depth/rate pair."""
    up, dn = TBL[which]
    return words(rom, up)[depth] + rate, words(rom, dn)[depth] - rate


def predict(rom, which, depth, rate):
    """(frequency Hz, half period s, level swing) for one depth/rate pair."""
    wu, wd = segment(rom, which, depth, rate)
    swing = level(wu >> 8) - level(wd >> 8)
    tu = swing / (2.0 ** (s8(wu & 0xFF) / 8.0) * RAMP_SCALE) / ENGINE
    td = swing / (2.0 ** (abs(s8(wd & 0xFF)) / 8.0) * RAMP_SCALE) / ENGINE
    return 1.0 / (tu + td), tu, swing


def load(path):
    w = wave.open(path)
    n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
    d = np.frombuffer(w.readframes(n), '<i2').astype(np.float64) / 32768.0
    return sr, d.reshape(-1, ch)


def envelope(x, sr, hop=24, win=96):
    n = (len(x) - win) // hop
    e = np.sqrt(np.array([np.mean(x[i * hop:i * hop + win] ** 2) for i in range(n)]))
    return e, sr / hop


def onsets(x, sr):
    e, fr = envelope(x, sr, hop=480, win=1920)
    pk = e.max()
    out, armed = [], True
    for i, v in enumerate(e):
        if armed and v > 0.06 * pk:
            out.append(i / fr)
            armed = False
        elif not armed and v < 0.015 * pk:
            armed = True
    return out


def lfo_period(x, sr, fmin=0.4, fmax=20.0):
    e, fr = envelope(x, sr)
    y = 20 * np.log10(np.maximum(e, 1e-7))
    y -= y.mean()
    ac = np.correlate(y, y, 'full')[len(y) - 1:]
    lo, hi = int(fr / fmax), min(int(fr / fmin), len(ac) - 2)
    if hi <= lo:
        return None, None
    lag = lo + int(np.argmax(ac[lo:hi]))
    a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]
    frac = 0.5 * (a - c) / (a - 2 * b + c + 1e-30)
    return fr / (lag + frac), (y, fr, lag)


def duty(y, fr, lag):
    """Fraction of the cycle spent rising.  0.5 is a triangle, 1/17 the sawtooth that the
    voices' rise/fall asymmetry would predict."""
    n = (len(y) - lag) // lag
    if n < 2:
        return float('nan')
    m = np.array([y[i * lag:(i + 1) * lag] for i in range(n)]).mean(0)
    m = np.roll(m, -int(np.argmin(m)))
    return int(np.argmax(m)) / lag


def sideband_offset(x, sr, harmonic):
    """Half the separation of the two shifted lines, in cents, or None."""
    fc = NOTE60 * harmonic
    w = x * np.hanning(len(x))
    S = np.abs(np.fft.rfft(w))
    ff = np.fft.rfftfreq(len(x), 1 / sr)
    m = (ff > fc * 2 ** (-0.14)) & (ff < fc * 2 ** 0.14)
    sub, fs_ = S[m], ff[m]
    pk = sub.max()
    up, dn = [], []
    for i in range(3, len(sub) - 3):
        if sub[i] == max(sub[i - 3:i + 4]) and sub[i] > pk * 0.05:
            c = 1200 * np.log2(fs_[i] / fc)
            if c > 18:
                up.append((sub[i], c))
            elif c < -18:
                dn.append((sub[i], c))
    if not up or not dn:
        return None
    # A slope s gives +1200*log2(1+s) and -1200*log2(1-s); solve each for s and average.
    cu = max(up)[1]
    cd = min(dn, key=lambda z: -z[0])[1]
    su = 2 ** (cu / 1200.0) - 1.0
    sd = 1.0 - 2 ** (cd / 1200.0)
    return (su + sd) / 2.0


SWEEPS = [('01_fx_chorus_rate.wav', 'chorus', 'rate', (0, 3, 7, 11, 15), 7, 10.0),
          ('02_fx_chorus_depth.wav', 'chorus', 'depth', (0, 1, 4, 7, 11, 15), 7, 10.0),
          ('03_fx_tremolo_rate.wav', 'tremolo', 'rate', (0, 3, 7, 11, 15), 7, 10.0),
          ('04_fx_tremolo_depth.wav', 'tremolo', 'depth', (0, 1, 4, 7, 11, 15), 7, 10.0)]


def analyse(root, label):
    rom = open(ROM, 'rb').read()
    print("\n" + "=" * 78)
    print("%s   %s" % (label, root))
    print("=" * 78)

    for fname, which, varying, values, other, hold in SWEEPS:
        path = os.path.join(root, fname)
        if not os.path.exists(path):
            continue
        sr, d = load(path)
        on = onsets(d[:, 0], sr)
        if len(on) != len(values):
            print("\n%s: found %d onsets for %d trials, skipping"
                  % (fname, len(on), len(values)))
            continue
        print("\n%s %s sweep" % (which.upper(), varying))
        print("  %5s %9s %9s %7s %9s"
              % (varying, 'f meas', 'f pred', 'duty', 'scale fit'))
        for t, v in zip(on, values):
            depth, rate = (v, other) if varying == 'depth' else (other, v)
            a, b = int((t + 2) * sr), int((t + hold - 1) * sr)
            f, aux = lfo_period(d[a:b, 0], sr)
            fp, halfT, swing = predict(rom, which, depth, rate)
            if depth == 0:
                print("  %5d %9s %9s %7s %9s   depth 0: the firmware switches it off"
                      % (v, '-', '-', '-', '-'))
                continue
            du = duty(*aux) if aux else float('nan')
            # what ramp scale would this period imply?
            wu, _ = segment(rom, which, depth, rate)
            fit = swing / (0.5 / f * ENGINE) / 2.0 ** (s8(wu & 0xFF) / 8.0)
            print("  %5d %8.3f %8.3f %7.3f %9.2f" % (v, f, fp, du, fit))

    # tremolo depth, from the pan ratio
    path = os.path.join(root, '04_fx_tremolo_depth.wav')
    if os.path.exists(path):
        sr, d = load(path)
        on = onsets(d[:, 0], sr)
        # channel imbalance, measured where the two channels carry the same signal
        print("\nTREMOLO pan ratio L/(L+R) -- the tone's own level cancels")
        print("  %5s %19s %19s" % ('depth', 'measured', 'predicted'))
        for t, dep in zip(on, (0, 1, 4, 7, 11, 15)):
            s = d[int((t + 1.5) * sr):int((t + 9.5) * sr)]
            eL, fr = envelope(s[:, 0], sr)
            eR, _ = envelope(s[:, 1], sr)
            n = min(len(eL), len(eR))
            r = eL[:n] / (eL[:n] + eR[:n] + 1e-12)
            r = r[int(fr * 0.3):]
            wu, wd = segment(rom, 'tremolo', dep, 7)
            gh, gl = level(wu >> 8), level(wd >> 8)
            c = gh + gl
            print("  %5d   %6.4f .. %6.4f     %6.4f .. %6.4f"
                  % (dep, np.percentile(r, 1), np.percentile(r, 99), gl / c, gh / c))

    # chorus delay tap
    path = os.path.join(root, '02_fx_chorus_depth.wav')
    if os.path.exists(path):
        sr, d = load(path)
        on = onsets(d[:, 0], sr)
        print("\nCHORUS delay tap: level >> k, from the sideband offset")
        print("  %5s %9s %11s %12s %7s" % ('depth', 'slope', 'half T ms', 'swing smp', 'k'))
        for t, dep in zip(on, (0, 1, 4, 7, 11, 15)):
            if dep == 0:
                continue
            s = d[int((t + 2) * sr):int((t + 9) * sr), 0]
            # high harmonics only: at h4 the two sidebands of a shallow setting sit
            # inside the guard band around the carrier and get thrown away
            sl = [sideband_offset(s, sr, h) for h in (8, 10, 12)]
            sl = [v for v in sl if v]
            if not sl:
                print("  %5d   no sidebands resolved" % dep)
                continue
            slope = float(np.median(sl))
            _, halfT, swing = predict(rom, 'chorus', dep, 7)
            smp = slope * ENGINE * halfT
            print("  %5d %9.5f %11.1f %12.1f %7.2f"
                  % (dep, slope, halfT * 1000, smp, np.log2(swing / smp)))

    # stereo arrangement
    path = os.path.join(root, '05_fx_shape.wav')
    if os.path.exists(path):
        sr, d = load(path)
        on = onsets(d[:, 0], sr)
        if len(on) >= 2:
            seg = d[int((on[1] + 3) * sr):int((on[1] + 23) * sr)]
            fc = NOTE60 * 8
            W, hop = int(0.15 * sr), int(0.05 * sr)

            def updown(x):
                w = x * np.hanning(len(x))
                S = np.abs(np.fft.rfft(w)) ** 2
                ff = np.fft.rfftfreq(len(x), 1 / sr)
                u = S[(ff > fc * 2 ** 0.055) & (ff < fc * 2 ** 0.095)].sum()
                v = S[(ff > fc * 2 ** -0.095) & (ff < fc * 2 ** -0.055)].sum()
                return 10 * np.log10((u + 1e-30) / (v + 1e-30))

            tL = np.array([updown(seg[i * hop:i * hop + W, 0])
                           for i in range((len(seg) - W) // hop)])
            tR = np.array([updown(seg[i * hop:i * hop + W, 1])
                           for i in range((len(seg) - W) // hop)])
            print("\nCHORUS stereo: which sideband is stronger, L against R")
            print("  correlation %+.3f   (-1 = two taps sweeping opposite ways,"
                  % np.corrcoef(tL, tR)[0, 1])
            print("                        +1 = one tap offset from the other)")

    # effect order, from the both-on trial
    path = os.path.join(root, '06_fx_stereo.wav')
    if os.path.exists(path):
        from scipy.signal import butter, sosfiltfilt
        sr, d = load(path)
        on = onsets(d[:, 0], sr)
        if len(on) >= 5:
            print("\nEFFECT ORDER: is the delayed copy panned too?")
            print("  %-12s %9s %10s %7s %7s" % ('trial', 'carrier', 'sidebands', 'S/C', 'corr'))
            for idx, nm in ((4, 'both_wet'), (0, 'chorus_wet')):
                seg = d[int((on[idx] + 2) * sr):int((on[idx] + 13) * sr), 0]
                W, hop = int(0.15 * sr), int(0.02 * sr)
                win = np.hanning(W)
                ff = np.fft.rfftfreq(W, 1 / sr)
                masks = []
                for h in (6, 8, 10):
                    fc = NOTE60 * h
                    masks.append(((ff > fc * 2 ** -0.0060) & (ff < fc * 2 ** 0.0060),
                                  ((ff > fc * 2 ** -0.070) & (ff < fc * 2 ** -0.011))
                                  | ((ff > fc * 2 ** 0.011) & (ff < fc * 2 ** 0.070))))
                nf = (len(seg) - W) // hop
                C, S = np.zeros(nf), np.zeros(nf)
                for i in range(nf):
                    P = np.abs(np.fft.rfft(seg[i * hop:i * hop + W] * win)) ** 2
                    for cm, sm in masks:
                        C[i] += P[cm].sum()
                        S[i] += P[sm].sum()
                C, S = 10 * np.log10(C + 1e-30), 10 * np.log10(S + 1e-30)
                fr = sr / hop
                sos = butter(3, [2.5, 4.5], 'bandpass', fs=fr, output='sos')
                cb, sb = sosfiltfilt(sos, C - C.mean()), sosfiltfilt(sos, S - S.mean())
                print("  %-12s %9.3f %10.3f %7.3f %+7.3f"
                      % (nm, cb.std(), sb.std(), sb.std() / cb.std(),
                         np.corrcoef(cb, sb)[0, 1]))
            print("  S/C ~1 with corr +1: the pan multiplies the delayed copy as well as the")
            print("  dry one, so the DELAY RUNS FIRST.  ~0 would mean only the dry is panned.")
            print("  chorus_wet is the control -- no tremolo, so nothing in that band.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hw', default=os.path.join(HERE, 'listen', 'hardware', 'effects'))
    ap.add_argument('--emu', default=None)
    args = ap.parse_args()
    if os.path.isdir(args.hw):
        analyse(args.hw, 'HARDWARE')
    if args.emu:
        analyse(args.emu, 'EMULATOR')


if __name__ == '__main__':
    main()
