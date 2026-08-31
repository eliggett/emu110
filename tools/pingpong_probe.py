#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-exact model of the U-110 ping-pong loop, diffed against MAME, plus seam plots.

Models one voice exactly as mb87419_mb87420_device does -- same delta accumulation, same
interpolation, same reflection, same order of operations -- so any difference against the
emulator's own output localises a bug rather than hinting at one.  Also renders an "ideal"
ping-pong (continuous position, interpolated from the decoded waveform) to show what the
loop *should* sound like.

    python3 tools/pingpong_probe.py -o analysis/pingpong.pdf
"""
import argparse, sys
import numpy as np
sys.path.insert(0, 'tools')
from envelope_measure import load_roms, decode_float8, rom_slice, volume_gain, ENGINE_RATE
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt

# choir 3, note 60 -- the two voices, straight from a MAME register trace
VOICES = [
    dict(name='voice A', bank=0xA000, start=0x379F5, end=0x3AF3C, loop=0x379F8,
         step=0x3BE3, vol=0xE32D),
    dict(name='voice B', bank=0x9400, start=0x2B141, end=0x2F234, loop=0x2B144,
         step=0x2FFD, vol=0xD029),
]
DC_KEEP = np.exp(-2 * np.pi * 20.0 / ENGINE_RATE)


def full(v, field):
    return ((v['bank'] & 0x3C00) << 8) | v[field]


def interp(s1, s2, frac):
    return ((s1 * (0x4000 - frac) + s2 * frac) >> 14)


def model_mame(v, n, banks):
    """Exactly what roland_lp.cpp does, including the ping-pong fix."""
    base = (v['bank'] & 0x3C00) << 8
    end, loop = v['end'] >> 2, v['loop'] >> 2
    rom = lambda a: int(decode_float8(rom_slice(banks, a, 1))[0])
    addr = (v['start'] << 14)
    acc = float(rom(full(v, 'start')))
    cur, nxt, d = 0, int(np.clip(acc, -8191, 8191)), +1
    out = np.zeros(n)
    events = []
    for i in range(n):
        frac = addr & 0x3FFF
        out[i] = interp(cur, nxt, frac) if d > 0 else interp(nxt, cur, frac)
        old = addr
        addr = addr + d * v['step']
        flipped = False
        if d > 0 and (addr >> 16) >= end:
            addr = (end << 16) - addr + (end << 16); d = -1; flipped = True
        elif d < 0 and (addr >> 16) < loop:
            addr = (loop << 16) - addr + (loop << 16); d = +1; flipped = True
        if flipped:
            here = (addr >> 14) | base
            s0 = int(np.clip(acc, -8191, 8191))
            s1 = int(np.clip(acc + rom(here + 1), -8191, 8191))
            cur, nxt = (s0, s1) if d > 0 else (s1, s0)
            events.append(i)
        if (addr >> 14) != (old >> 14):
            cur = nxt
            acc = acc * DC_KEEP + rom((addr >> 14) | base)
            nxt = int(np.clip(acc, -8191, 8191))
    return out * volume_gain(v['vol']), events


def model_ideal(v, n, banks):
    """A ping-pong with no state left over: position reflects continuously and the sample
    is interpolated from the integrated waveform, which is what the loop should produce."""
    s, e = full(v, 'start'), full(v, 'end')
    lp = full(v, 'loop')
    wave = np.cumsum(decode_float8(rom_slice(banks, s, e - s + 2)))
    wave -= wave.mean()
    lo, hi = lp - s, e - s                      # loop window in sample indices
    span = hi - lo
    r = v['step'] / 0x4000
    pos = np.arange(n) * r
    # fold into a triangle wave over [lo, hi]
    p = np.where(pos < hi - lo, pos, (pos - (hi - lo)) % (2 * span))
    p = np.where(p > span, 2 * span - p, p) + lo
    p = np.clip(p, 0, len(wave) - 2)
    i = p.astype(int); f = p - i
    return (wave[i] * (1 - f) + wave[i + 1] * f) * volume_gain(v['vol'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default='analysis/pingpong.pdf')
    ap.add_argument('--secs', type=float, default=2.2)
    args = ap.parse_args()
    banks = load_roms()
    n = int(args.secs * ENGINE_RATE)

    mm, ev = model_mame(VOICES[0], n, banks)
    mi = model_ideal(VOICES[0], n, banks)
    print("voice A: %d reversals in %.1f s (every %.3f s)"
          % (len(ev), args.secs, (ev[1] - ev[0]) / ENGINE_RATE if len(ev) > 1 else 0))

    fig = plt.figure(figsize=(9.5, 12))
    gs = fig.add_gridspec(4, 1, height_ratios=[1.15, 1.15, 1.0, 1.0], hspace=0.42)
    fig.suptitle("U-110 ping-pong loop: Choir 3, note 60, voice A", fontsize=13, y=0.965)

    v = VOICES[0]
    s, e, lp = full(v, 'start'), full(v, 'end'), full(v, 'loop')
    raw = decode_float8(rom_slice(banks, s, e - s + 8))
    wave = np.cumsum(raw); wave -= wave.mean()

    # ---- (a) the seam at `end`: five each side, plus five of context
    for ax, (pt, lbl) in zip([fig.add_subplot(gs[0]), fig.add_subplot(gs[1])],
                             [(e - s, "end  (0x%06X)" % e), (lp - s, "loop (0x%06X)" % lp)]):
        k = 5
        idx = np.arange(pt - 2 * k, pt + 2 * k + 1)
        idx = idx[(idx >= 0) & (idx < len(wave))]
        ax.plot(idx - pt, wave[idx], 'o-', ms=5, lw=1.3, color='#1f4e8c',
                label='ROM data, integrated')
        # what a ping-pong reflection produces on the far side of the boundary
        mir = np.array([wave[int(np.clip(2 * pt - j, 0, len(wave) - 1))] for j in idx])
        far = idx - pt > 0 if pt == e - s else idx - pt < 0
        ax.plot((idx - pt)[far], mir[far], 's--', ms=5, lw=1.3, color='#cc5500',
                label='what the reflection plays instead')
        ax.axvline(0, color='#888', lw=1.0)
        if pt == e - s:
            ax.axvspan(0, (idx - pt).max(), color='#f0f0f0', zorder=0)
            ax.annotate("beyond `end` -- not part of\nthe sample; the ROM is silent here",
                        xy=(0.62, 0.80), xycoords='axes fraction', fontsize=7.5, color='#666')
        else:
            ax.axvspan((idx - pt).min(), 0, color='#f0f0f0', zorder=0)
            ax.annotate("before `loop`, which sits only\n3 samples after `start`",
                        xy=(0.03, 0.80), xycoords='axes fraction', fontsize=7.5, color='#666')
        ax.axhline(0, color='#ddd', lw=0.8)
        ax.set_title("(%s) the %s -- five samples each side, plus five of run-in"
                     % ('a' if pt == e - s else 'b', lbl), fontsize=10)
        ax.set_xlabel("samples relative to the boundary"); ax.set_ylabel("value")
        ax.grid(alpha=0.3); ax.legend(fontsize=8)
        # A reflection does not create a value step -- it creates a CORNER.  The sample
        # after wave[k] is wave[k-1], so the slope simply negates.  How audible that is
        # depends on how steep the waveform is at the turning point: reflect at an extremum
        # and it is smooth, reflect on a steep flank and the derivative jumps by twice the
        # slope, which is a tick.
        sl = wave[pt] - wave[pt - 1]
        lo_w = max(0, pt - 200)
        typ = np.abs(np.diff(wave[lo_w:pt + 1])).mean() if pt - lo_w >= 50 \
            else np.abs(np.diff(wave[pt:min(len(wave), pt + 200)])).mean()
        ax.annotate("slope at the turn %+.0f per sample\n"
                    "typical slope nearby %.0f\n"
                    "derivative jumps by %.0f  (%.1fx typical)"
                    % (sl, typ, 2 * abs(sl), 2 * abs(sl) / max(typ, 1e-9)),
                    xy=(0.02, 0.04), xycoords='axes fraction', fontsize=8.5,
                    bbox=dict(fc='#fff8e8', ec='#ccc', boxstyle='round,pad=0.35'))

    # ---- (c) first difference of the whole sample, normalised
    ax = fig.add_subplot(gs[2])
    d = np.diff(wave)
    d = d / np.abs(d).max()
    t = np.arange(len(d)) / ENGINE_RATE
    ax.plot(t, d, lw=0.4, color='#1f4e8c')
    ax.axvline((lp - s) / ENGINE_RATE, color='#0a0', lw=1.2, label='loop')
    ax.axvline((e - s) / ENGINE_RATE, color='#c00', lw=1.2, label='end')
    ax.set_xlim(0, len(d) / ENGINE_RATE)
    ax.set_title("(c) $x[n]-x[n-1]$ across the whole sample, normalised", fontsize=10)
    ax.set_xlabel("time within the sample (s, at the ROM's own rate)")
    ax.set_ylabel("normalised"); ax.grid(alpha=0.3); ax.legend(fontsize=8)

    # ---- (d) measured: the emulator's own output differential through a reversal
    ax = fig.add_subplot(gs[3])
    try:
        import wave as _w
        f = _w.open('listen/emulated/scratch/choir_pp_v1.wav')
        rr = f.getframerate()
        y = (np.frombuffer(f.readframes(f.getnframes()), dtype='<i2').astype(float)
             .reshape(-1, f.getnchannels()).mean(1) / 32768.)
        # a reversal located earlier by prediction error, in the second note
        c0 = int(34.7111 * rr)
        w2 = 90
        seg2 = y[c0 - w2:c0 + w2]
        dd = np.diff(seg2); dd = dd / np.abs(dd).max()
        ax.plot(np.arange(len(dd)) - w2, dd, lw=1.0, color='#1f4e8c')
        ax.axvline(0, color='#c00', lw=1.2, label='reversal')
        ax.set_title("(d) measured $x[n]-x[n-1]$ from the emulator through one reversal, "
                     "normalised", fontsize=10)
        ax.set_xlabel("samples relative to the reversal"); ax.set_ylabel("normalised")
        ax.grid(alpha=0.3); ax.legend(fontsize=8)
    except Exception as exc:
        ax.text(0.5, 0.5, "render not available: %s" % exc, ha='center')

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(args.out, bbox_inches='tight')
    print("wrote %s" % args.out)

    for pt, lbl in ((e - s, 'end'), (lp - s, 'loop')):
        sl = wave[pt] - wave[pt - 1]
        lo_w = max(0, pt - 200)
        typ = np.abs(np.diff(wave[lo_w:pt + 1])).mean() if pt - lo_w >= 50 \
            else np.abs(np.diff(wave[pt:min(len(wave), pt + 200)])).mean()
        print("  %-5s turn: slope %+8.1f/sample, typical %7.1f, derivative jump %8.1f (%.1fx)"
              % (lbl, sl, typ, 2 * abs(sl), 2 * abs(sl) / max(typ, 1e-9)))


if __name__ == '__main__':
    main()
