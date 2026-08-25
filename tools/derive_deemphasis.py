#!/usr/bin/env python3
"""Derive the transfer function between the wave ROM and the U-110's analog output.

Divides a hardware capture's spectrum by a dry render of exactly the same wave-ROM data
(same samples, same pitch, same voice gains, no output filter), for many tones at once.
Whatever is left is everything the emulator does not model between ROM and jack.

    python3 tools/derive_deemphasis.py -o analysis/deemphasis.pdf
"""
import argparse, glob, re, sys
import numpy as np, wave
sys.path.insert(0, 'tools')
from envelope_measure import (load_roms, decode_float8, rom_slice, volume_gain,
                              ENGINE_RATE)
from scipy.signal import resample_poly, get_window
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt

# render-time windows, from tools/capture_u110.py --dry-run-midi
SEG = {'piano_scale':(23.00,44.80),'flute':(59.60,74.40),'strings1':(76.90,88.70),
       'slap':(91.20,105.00),'drums':(116.80,125.10),'choir3_pingpong':(127.60,157.90),
       'strings3_pingpong':(160.40,190.70),'choir3_sustain':(193.20,229.00),
       'fing_bass':(231.50,248.85),'fless_bass':(251.35,268.70),
       'shakuhachi':(271.20,289.75),'fantasy':(292.25,323.20)}


def trace_voices(path):
    t, out = 0.0, []
    pat = (r'TG ([\d.]+) |Starting channel (\d+), bank 0x([0-9A-F]+), addr 0x([0-9A-F]+)\.\d+\s*\n'
           r'[^\n]*Smpl End Ofs: 0x([0-9A-F]+), Loop Ofs 0x([0-9A-F]+), Step 0x([0-9A-F]+), '
           r'Volume ([0-9A-F]+)')
    for m in re.finditer(pat, open(path, errors='replace').read()):
        if m.group(1):
            t = float(m.group(1)); continue
        if int(m.group(7), 16) == 0:
            continue
        base = (int(m.group(3), 16) & 0x3C00) << 8
        out.append((t, dict(start=base | int(m.group(4), 16), end=base | int(m.group(5), 16),
                            loop=base | int(m.group(6), 16), step=int(m.group(7), 16),
                            vol=int(m.group(8), 16))))
    return out


def read(p):
    w = wave.open(p); sr = w.getframerate()
    return (np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(float)
            .reshape(-1, w.getnchannels()).mean(1) / 32768.), sr


def render(banks, v, dur):
    """Dry: decoded ROM bytes, pitch-shifted and looped.  No filter of any kind."""
    n = int(dur * ENGINE_RATE)
    body = decode_float8(rom_slice(banks, v['start'], v['end'] - v['start'] + 2))
    L, LL, r = v['end'] - v['start'], v['end'] - v['loop'], v['step'] / 0x4000
    pos = np.arange(n) * r
    pos = np.where(pos >= L, L - LL + (pos - (L - LL)) % LL, pos) if LL > 0 \
        else np.minimum(pos, L - 2)
    i = np.clip(pos.astype(int), 0, len(body) - 2); f = pos - i
    return (body[i] * (1 - f) + body[i + 1] * f) * volume_gain(v['vol'])


def welch(x, sr, nfft=8192):
    win = get_window('hann', nfft); acc = np.zeros(nfft // 2 + 1); n = 0
    for i in range(0, len(x) - nfft, nfft // 2):
        acc += np.abs(np.fft.rfft(x[i:i + nfft] * win)) ** 2; n += 1
    return np.fft.rfftfreq(nfft, 1 / sr), acc / max(n, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default='analysis/deemphasis.pdf')
    ap.add_argument('--trace', default='mame/error.log')
    args = ap.parse_args()

    banks = load_roms(); vox = trace_voices(args.trace)
    F = np.logspace(np.log10(80), np.log10(16000), 48)
    curves, names = [], []
    for name, (a, b) in SEG.items():
        f = glob.glob('listen/3/[0-9][0-9]_%s.wav' % name)
        if not f:
            continue
        hw, sr = read(f[0])
        vs = [v for t, v in vox if a <= t <= b]
        if not vs:
            continue
        dry = sum(render(banks, v, min(b - a, 12.0)) for v in vs)
        dry = resample_poly(dry, sr, ENGINE_RATE)
        fh, Ph = welch(hw[:len(dry)], sr); _, Pd = welch(dry, sr)
        ratio = 10 * np.log10((Ph + 1e-30) / (Pd + 1e-30))
        strong = Ph > Ph.max() * 1e-6           # only bins where hardware has real content
        v = []
        for k in range(len(F) - 1):
            m = (fh >= F[k]) & (fh < F[k + 1]) & strong
            v.append(np.median(ratio[m]) if m.sum() >= 3 else np.nan)
        v = np.array(v)
        i0 = np.nanargmin(np.abs(np.sqrt(F[:-1] * F[1:]) - 500))
        curves.append(v - v[i0]); names.append(name)

    fc = np.sqrt(F[:-1] * F[1:])
    C = np.array(curves)
    med = np.nanmedian(C, axis=0)

    ok = np.isfinite(med) & (fc >= 120) & (fc <= 12000)
    slope, icept = np.polyfit(np.log2(fc[ok]), med[ok], 1)
    print("fit over 120 Hz - 12 kHz:  %.2f dB/octave" % slope)
    print("(a first-order integrator is exactly -6.02 dB/octave)")

    integ = -20 * np.log10(fc / 500.0)          # ideal 1/f, normalised at 500 Hz
    resid = med - integ

    fig, ax = plt.subplots(2, 1, figsize=(9, 9.5), sharex=True)
    fig.suptitle("U-110: hardware output divided by a dry render of the same wave-ROM data",
                 fontsize=12.5, y=0.97)
    a0 = ax[0]
    for v, n in zip(C, names):
        a0.semilogx(fc, v, lw=0.8, alpha=0.45)
    a0.semilogx(fc, med, 'k', lw=2.4, label='median of %d tones' % len(C))
    a0.semilogx(fc, integ, 'r--', lw=1.8, label='ideal $1/f$ integrator ($-6.02$ dB/oct)')
    a0.semilogx(fc, slope * np.log2(fc) + icept, 'b:', lw=1.6,
                label='best fit: %.2f dB/oct' % slope)
    a0.set_ylabel("hardware / dry ROM   (dB)")
    a0.grid(alpha=0.3, which='both'); a0.legend(fontsize=8.5, loc='lower left')
    a0.set_title("thin lines: one per tone, normalised at 500 Hz", fontsize=9.5)
    a1 = ax[1]
    a1.semilogx(fc, resid, 'k', lw=2.0)
    a1.axhline(0, color='r', ls='--', lw=1.2)
    a1.fill_between(fc, -3, 3, color='#8fbf8f', alpha=0.25, label='$\\pm$3 dB')
    a1.set_ylim(-20, 20); a1.set_xlim(80, 16000)
    a1.set_xlabel("frequency (Hz)"); a1.set_ylabel("residual after $1/f$ (dB)")
    a1.grid(alpha=0.3, which='both'); a1.legend(fontsize=8.5)
    a1.set_title("what is left once a single integration is taken out", fontsize=9.5)
    fig.tight_layout(rect=[0, 0, 1, 0.955])
    fig.savefig(args.out, bbox_inches='tight')
    print("wrote %s" % args.out)

    print("\n   f(Hz)   median   1/f    residual")
    for i in range(0, len(fc), 3):
        if np.isfinite(med[i]):
            print("  %7.0f  %+7.1f %+7.1f  %+7.1f" % (fc[i], med[i], integ[i], resid[i]))


if __name__ == '__main__':
    main()
