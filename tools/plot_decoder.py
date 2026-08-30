#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plot the U-110's 8-bit -> 16-bit companding curve and its step size.

The wave ROM byte is not linear PCM.  It is a small floating-point format applied to the
MAGNITUDE of the two's-complement byte:

    sign     the byte's own sign
    exp      bits 4-6 of |byte|          (call it `shift`)
    mant     bits 0-3 of |byte|

    shift == 0 :  value = mant                        identity, codes 0..15
    shift >= 1 :  value = (16 + mant) << (shift - 1)

so consecutive codes are one unit apart up to |31| and the step doubles with every
exponent after that.  Decoding is exact and memoryless -- the byte IS the sample, not a
delta.  (Credit for the 1-3-4 rule goes to Sarayan; this is the same arithmetic as
decode_sample() in mame/src/devices/sound/roland_lp.cpp.)

    python3 tools/plot_decoder.py [-o analysis/decoder_curve.pdf]
"""
import argparse
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def decode(b):
    """b: signed byte value.  Returns the decoded 16-bit-domain sample."""
    sign = -1 if b < 0 else 1
    v = abs(b)
    shift, mant = v >> 4, v & 0x0F
    return sign * (mant if shift == 0 else (0x10 + mant) << (shift - 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default='analysis/decoder_curve.pdf')
    args = ap.parse_args()

    codes = np.arange(-128, 128)
    vals = np.array([decode(int(c)) for c in codes])

    # step size: dv16 / dv8 between adjacent codes (dv8 is 1 everywhere)
    mid = codes[:-1] + 0.5
    step = np.diff(vals)

    fig = plt.figure(figsize=(9, 11))
    gs = fig.add_gridspec(3, 1, height_ratios=[5, 5, 1.5], hspace=0.32)
    ax = [fig.add_subplot(gs[0]), fig.add_subplot(gs[1])]
    axt = fig.add_subplot(gs[2]); axt.axis('off')
    fig.suptitle("Roland U-110 wave ROM: 8-bit companded sample $\\rightarrow$ 16-bit value",
                 fontsize=13, y=0.975)

    # ---------------------------------------------------------------- curve
    a = ax[0]
    a.plot(codes, vals, lw=1.4, color='#1f4e8c')
    a.plot(codes, vals, '.', ms=2.6, color='#1f4e8c')
    a.axhline(0, color='#999', lw=0.6)
    a.axvline(0, color='#999', lw=0.6)
    a.set_xlabel("8-bit ROM byte, two's complement")
    a.set_ylabel("decoded value")
    a.set_title("(a) transfer curve   —   full scale $+1984 / -2048$", fontsize=10)
    a.grid(alpha=0.25)
    a.set_xlim(-132, 132)
    a.set_ylim(-2300, 2300)
    a.set_xticks(range(-128, 129, 32))
    # mark where each exponent takes over
    for sh in range(1, 8):
        c = sh * 16
        a.axvline(c, color='#cc5500', lw=0.5, ls=':', alpha=0.7)
        a.axvline(-c, color='#cc5500', lw=0.5, ls=':', alpha=0.7)
        a.annotate("e%d" % sh, (c + 8, 2150), fontsize=7.5, color='#cc5500',
                   ha='center', va='bottom', annotation_clip=False)
    a.annotate("exponent:", (-4, 2150), fontsize=7.5, color='#cc5500',
               ha='right', va='bottom', annotation_clip=False)
    a.annotate("identity below |32|:\ncodes 0..31 decode to 0..31",
               xy=(24, 24), xytext=(-118, 1250), fontsize=8.5,
               arrowprops=dict(arrowstyle='->', lw=0.7, color='#444'))
    a.annotate("code $-128$ is the only\ncode with exponent 8", xy=(-128, -2048),
               xytext=(-118, -1500), fontsize=8.5,
               arrowprops=dict(arrowstyle='->', lw=0.7, color='#444'))

    # ---------------------------------------------------------------- step size
    b = ax[1]
    b.step(mid, step, where='mid', lw=1.3, color='#1f4e8c')
    b.set_yscale('log', base=2)
    b.set_xlabel("8-bit ROM byte, two's complement")
    b.set_ylabel("$dv_{16}/dv_{8}$   (change in decoded value per code)")
    b.set_title("(b) step size   —   doubles with every exponent; $dv_8$ is 1 everywhere",
                fontsize=10)
    b.grid(alpha=0.25, which='both')
    b.set_xlim(-132, 132)
    b.set_xticks(range(-128, 129, 32))
    b.set_yticks([1, 2, 4, 8, 16, 32, 64, 128])
    b.set_yticklabels(['1', '2', '4', '8', '16', '32', '64', '128'])
    for sh in range(1, 8):
        for c in (sh * 16, -sh * 16):
            b.axvline(c, color='#cc5500', lw=0.5, ls=':', alpha=0.7)
    b.annotate("the jump at 0 is the sign change:\n$-1 \\rightarrow +1$ across code 0 is one unit,\n"
               "so there is no dead zone",
               xy=(0, 1), xytext=(-124, 10), fontsize=8.5,
               arrowprops=dict(arrowstyle='->', lw=0.7, color='#444'))

    # a short table of the segments
    rows = []
    for sh in range(0, 8):
        lo, hi = (0, 15) if sh == 0 else (16 << (sh - 1), (31 << (sh - 1)))
        st = 1 if sh <= 1 else 1 << (sh - 1)
        rows.append("e=%d  codes %3d..%-3d  ->  %5d..%-5d  step %3d"
                    % (sh, sh * 16, sh * 16 + 15, lo, hi, st))
    # exponent 8 exists only as the single code -128; the rest of that segment is unreachable
    rows.append("e=8  code  128 only  ->   2048          (only as -128)")
    axt.text(0.5, 0.95, "segment table (magnitude side; the sign comes from the byte)",
             ha='center', va='top', fontsize=8.5, color='#333')
    axt.text(0.5, 0.74, "\n".join(rows), ha='center', va='top',
             fontsize=8.0, family='monospace', color='#222', linespacing=1.35)
    fig.savefig(args.out, bbox_inches='tight')
    print("wrote %s" % args.out)

    print("\nsegment table (magnitude side):")
    for r in rows:
        print("  " + r)
    print("\nfull scale: +%d / %d      distinct output values: %d"
          % (vals.max(), vals.min(), len(set(vals.tolist()))))
    print("dynamic range of the code space: %.1f dB"
          % (20 * np.log10(2048 / 1.0)))


if __name__ == '__main__':
    main()
