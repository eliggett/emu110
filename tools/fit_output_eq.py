#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
fit_output_eq.py -- measure the emulator's output response against hardware, and fit the
correction that flattens it.

    python3 tools/fit_output_eq.py --emu listen/emulated/fit-eqoff
    python3 tools/fit_output_eq.py --emu listen/emulated/fit-eqoff --match-band 200 800 --plot out.pdf

MEASURE THE RAW CHAIN.  The emulator render must be made with the output EQ correction
OFF, or this fits a residual on top of an existing filter and the two stack:

    python3 tools/render_u110.py --raw --out-dir listen/emulated/fit-eqoff \
        --mame-arg=-autoboot_script --mame-arg=/path/to/eq_off.lua

Three things this is careful about, each of which has produced a wrong answer before:

  * THE LEVEL-MATCH BAND IS PART OF THE RESULT.  Matching emulator to hardware over
    200 Hz - 2 kHz -- as the correction currently in the driver was fitted -- absorbs part
    of a monotonic tilt into the scalar, and makes the residual look both smaller and
    note-dependent, because that band holds a different part of the spectrum for a low
    note than for a high one.  Match LOW and state where.

  * THE HARDWARE CAPTURE HAS TWO NOISE FLOORS.  listen/hardware/3's recording floor is ~33 dB in
    these units, but during notes the U-110's own analog output noise sits ~13 dB above
    that and is flat to 16 kHz.  Any band where the hardware is near that level is
    measuring hiss, and fitting to it fits the hiss.  Bands below --min-level are dropped.

  * NO TIME ALIGNMENT IS ATTEMPTED.  Frames are gated by level and averaged, so the two
    sides need only contain the same notes, not the same phase.  Alignment error was what
    made an earlier attempt at this comparison unusable.

  * PATCHES WITH CHORUS OR TREMOLO ARE NOT COMPARABLE.  Neither effect is emulated, so
    on a chorused patch the hardware carries a detuned, modulated copy that the emulator
    cannot produce, and the difference lands in the response measurement as if it were
    frequency response.  listen/hardware/3 uses P-48 Shakuhachi and P-52 Fantasy, both capable of
    it; --exclude shakuhachi,fantasy drops them.  Below 9 kHz that changes the result by
    less than 0.15 dB (eleven other segments carry it), but those two are most of what
    reaches beyond 9 kHz, so ABOVE 9 kHz THIS MATERIAL CANNOT MEASURE ANYTHING -- see the
    n column, which falls to 1-3 segments there.  New captures are needed for the top
    two octaves, on patches with chorus and tremolo confirmed off.

  * --min-level IS NOT A FORMALITY.  Dropped too low it lets in bands where the hardware
    is on its own noise floor while the emulator still has signal, which reads as a huge
    fake EXCESS.  Measured on listen/hardware/4 at --min-level 50: piano and fantasy showed +12 to
    +15 dB at 6-9 kHz, on hardware band levels of 46-58 -- i.e. hiss -- while shakuhachi
    and strings, at 71-80, showed +5.6 to +7.2 there and agreed with each other to under
    1 dB.  The default of 60 is about right for these captures; check the n column and the
    per-segment levels before trusting anything near it.

  * WHICH MATERIAL REACHES HIGH.  Of everything captured so far, only P-48 Shakuhachi has
    real content past 10 kHz (band level 70 at 10.2 kHz, 64 at 11.4).  P-31 Strings, added
    for exactly this and expected to be broadband, turns out to be band-limited: it dies
    from 71 at 9 kHz to 46 by 11.4.  Piano and fantasy are done by 8 kHz.  So the top
    octave still rests on one segment.

The validity check is cross-segment agreement: a band where independent segments -- a
piano, a choir, a shakuhachi -- agree to a dB or so is measuring the response.  A band
where they scatter is measuring something else, and the spread column says so.
"""

import argparse, glob, os, sys, wave
import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_wav(path):
    with wave.open(path, 'rb') as w:
        n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
        a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch).astype(np.float64)
    return a, sr


def gated_spectrum(x, sr, nfft, gate_db):
    """Average |FFT| over frames within gate_db of the file's loudest frame.

    Level gating rather than time alignment: silence and deep decay tails are where the
    hardware sits on its own noise floor, and including them would drag the measurement
    toward hiss on one side and toward digital silence on the other."""
    hop = nfft // 2
    win = np.hanning(nfft)
    frames = []
    lev = []
    for i in range(0, len(x) - nfft, hop):
        s = x[i:i + nfft]
        rms = np.sqrt((s ** 2).mean())
        if rms <= 0:
            continue
        frames.append(s)
        lev.append(20 * np.log10(rms))
    if not frames:
        return None
    lev = np.array(lev)
    keep = lev >= lev.max() - gate_db
    acc = None
    for s, k in zip(frames, keep):
        if not k:
            continue
        S = np.abs(np.fft.rfft(s * win)) ** 2
        acc = S if acc is None else acc + S
    n = int(keep.sum())
    return np.sqrt(acc / n), np.fft.rfftfreq(nfft, 1 / sr), n


def band_levels(f, S, centres, bpo):
    half = 2 ** (0.5 / bpo)
    out = np.full(len(centres), np.nan)
    for i, c in enumerate(centres):
        m = (f >= c / half) & (f < c * half)
        if m.sum() >= 3:
            out[i] = 20 * np.log10(np.sqrt((S[m] ** 2).mean()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hw', default='listen/hardware/3', help='hardware capture directory')
    ap.add_argument('--emu', default='listen/emulated/fit-eqoff',
                    help='emulator render directory, made with the EQ correction OFF')
    ap.add_argument('--match-band', nargs=2, type=float, default=[200.0, 800.0],
                    metavar=('LO', 'HI'),
                    help='band over which each segment is level-matched (default 200-800 Hz). '
                         'Keep it low and well below the error, and say what you used.')
    ap.add_argument('--gate-db', type=float, default=20.0,
                    help='include frames within this many dB of the loudest frame')
    ap.add_argument('--min-level', type=float, default=60.0,
                    help='drop bands where the HARDWARE is below this level, in the dB '
                         'units of this analysis; the U-110 own-noise floor is near 46')
    ap.add_argument('--bpo', type=int, default=6, help='bands per octave')
    ap.add_argument('--nfft', type=int, default=8192)
    ap.add_argument('--fmin', type=float, default=100.0)
    ap.add_argument('--fmax', type=float, default=16000.0)
    ap.add_argument('--exclude', default='', help='comma-separated segment names to skip')
    args = ap.parse_args()

    hw_dir = args.hw if os.path.isabs(args.hw) else os.path.join(HERE, args.hw)
    em_dir = args.emu if os.path.isabs(args.emu) else os.path.join(HERE, args.emu)
    skip = {x.strip() for x in args.exclude.split(',') if x.strip()}

    nb = int(round(args.bpo * np.log2(args.fmax / args.fmin)))
    centres = args.fmin * 2.0 ** (np.arange(nb + 1) / args.bpo)

    rows = {}
    for hp in sorted(glob.glob(os.path.join(hw_dir, '[0-9][0-9]_*.wav'))):
        name = os.path.basename(hp)
        if name.rsplit('.', 1)[0].split('_', 1)[1] in skip:
            continue
        ep = os.path.join(em_dir, name)
        if not os.path.exists(ep):
            continue
        (hx, sr), (ex, _) = read_wav(hp), read_wav(ep)
        gh = gated_spectrum(hx[:, 0], sr, args.nfft, args.gate_db)
        ge = gated_spectrum(ex[:, 0], sr, args.nfft, args.gate_db)
        if gh is None or ge is None:
            continue
        H, f, nh = gh
        E, _, ne = ge
        hb = band_levels(f, H, centres, args.bpo)
        eb = band_levels(f, E, centres, args.bpo)
        m = (centres >= args.match_band[0]) & (centres <= args.match_band[1])
        m &= np.isfinite(hb) & np.isfinite(eb)
        if m.sum() < 2:
            continue
        eb = eb - (np.mean(eb[m]) - np.mean(hb[m]))       # level-match this segment
        d = eb - hb
        d[~np.isfinite(hb) | (hb < args.min_level)] = np.nan
        rows[name] = d

    if not rows:
        sys.exit('no matching segment pairs found')

    M = np.vstack([rows[k] for k in sorted(rows)])
    print('emulator response error vs hardware -- EMULATOR MINUS HARDWARE, in dB')
    print('  %d segments, %d bands/octave, level-matched %g-%g Hz, frame gate %g dB,'
          % (len(rows), args.bpo, args.match_band[0], args.match_band[1], args.gate_db))
    print('  bands with hardware below %g dB dropped as noise floor' % args.min_level)
    print()
    print('   freq      error    spread     n   segments agreeing')
    for i, c in enumerate(centres):
        col = M[:, i]
        col = col[np.isfinite(col)]
        if len(col) == 0:
            continue
        med = float(np.median(col))
        iqr = float(np.percentile(col, 75) - np.percentile(col, 25)) if len(col) > 2 else float('nan')
        bar = ('+' if med >= 0 else '-') * min(30, int(round(abs(med) * 3)))
        print('  %7.0f  %+7.2f  %7.2f  %4d   %s'
              % (c, med, iqr, len(col), bar))

    # machine-readable, for whatever fits the filter next
    out = os.path.join(em_dir, 'response_error.txt')
    with open(out, 'w') as fh:
        fh.write('# freq_hz  error_db  iqr_db  n_segments\n')
        fh.write('# emulator minus hardware; match %g-%g Hz; gate %g dB; min level %g dB\n'
                 % (args.match_band[0], args.match_band[1], args.gate_db, args.min_level))
        for i, c in enumerate(centres):
            col = M[:, i][np.isfinite(M[:, i])]
            if len(col) == 0:
                continue
            iqr = np.percentile(col, 75) - np.percentile(col, 25) if len(col) > 2 else float('nan')
            fh.write('%.2f %.4f %.4f %d\n' % (c, np.median(col), iqr, len(col)))
    print('\nwrote %s' % out)


if __name__ == '__main__':
    main()
