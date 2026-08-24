#!/usr/bin/env python3
"""Plot the Sound Check waveform (wave-ROM sample 212) to a PDF.

The point of interest: the ROM holds a trapezoid, but real hardware emits a near-pure
sine.  This draws the ROM bytes as they actually are, with a best-fit sine for reference.
"""
import numpy as np, sys, os
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from render_sample import load_banks, sample_table

OUT = sys.argv[1] if len(sys.argv) > 1 else 'sample212_trapezoid.pdf'
IDX = 212
CYC = 67                     # samples per cycle; the 268-byte loop holds exactly 4

banks = load_banks()
e = {x['i']: x for x in sample_table(banks[0])}[IDX]
rom = banks[e['bank']]
full = rom[e['start']:e['start'] + e['last'] + 1].astype(np.int8).astype(float)
loop = full[4:4 + 4*CYC]      # 4 whole cycles, past the leading zeros:
                              # windowing the raw 268 would break periodicity
                              # and manufacture spurious even harmonics
cyc  = full[4:4 + CYC]        # skip the 4 leading zeros; one clean cycle

# best-fit sine at the fundamental, for reference only
t   = np.arange(CYC)
s, c = np.sin(2*np.pi*t/CYC), np.cos(2*np.pi*t/CYC)
d   = cyc - cyc.mean()
fit = (np.dot(d, s)*s + np.dot(d, c)*c) * 2/CYC
corr = np.dot(d, fit)/np.sqrt(np.dot(d, d)*np.dot(fit, fit))

with PdfPages(OUT) as pdf:
    fig, ax = plt.subplots(4, 1, figsize=(8.27, 11.69))
    fig.suptitle('Roland U-110 — Sound Check waveform (wave-ROM sample %d)' % IDX,
                 fontsize=13, y=0.982)

    a = ax[0]
    a.step(np.arange(len(full)), full, where='post', lw=.9, color='#1f77b4')
    a.axvspan(0, e['looplen'], color='#1f77b4', alpha=.07)
    a.axvline(e['looplen'], color='#d62728', lw=.9, ls='--')
    a.text(e['looplen'], full.max()*.92, ' loop end (%d)' % e['looplen'],
           color='#d62728', fontsize=8, va='top')
    a.set_title('Complete sample: %d bytes at 0x%X in waverom%d, loop %d = 4 cycles of %d'
                % (len(full), e['start'], e['bank'], e['looplen'], CYC), fontsize=9)
    a.set_xlabel('byte offset'); a.set_ylabel('int8 value')

    a = ax[1]
    a.step(t, cyc, where='post', lw=1.3, color='#1f77b4', label='ROM bytes (trapezoid)')
    a.plot(t, cyc.mean()+fit, lw=1.1, ls='--', color='#d62728',
           label='best-fit sine (r = %.4f)' % corr)
    a.plot(t, cyc, '.', ms=4, color='#1f77b4')
    a.set_title('One cycle — the flat top and straight flanks are the whole point', fontsize=9)
    a.set_xlabel('sample within cycle'); a.set_ylabel('int8 value'); a.legend(fontsize=8)

    a = ax[2]
    a.step(t, d-fit, where='post', lw=1.1, color='#2ca02c')
    a.axhline(0, color='k', lw=.6)
    a.set_title('Residual (ROM − fitted sine): peaks near ±%.0f LSB at the flanks'
                % np.abs(d-fit).max(), fontsize=9)
    a.set_xlabel('sample within cycle'); a.set_ylabel('LSB')

    a = ax[3]
    V = np.abs(np.fft.rfft(loop - loop.mean()))
    f0 = 4                                            # 4 cycles in the loop
    h  = np.arange(1, 13)
    db = 20*np.log10(V[h*f0]/V[f0] + 1e-15)
    BOT = -75
    a.bar(h, np.maximum(db, BOT) - BOT, .55, bottom=BOT,
          color=['#1f77b4' if k % 2 else '#bbbbbb' for k in h])
    a.axhline(-43.0, color='#d62728', lw=1.1, ls='--')
    a.annotate('hardware suppresses h3 by 29 dB\nrelative to this data',
               xy=(11.6, -43.0), xytext=(8.4, -8), fontsize=8, color='#d62728', ha='center',
               arrowprops=dict(arrowstyle='->', color='#d62728', lw=.9))
    for k, v in zip(h, db):
        if v > BOT + 4:
            a.text(k, v + 1.5, '%.0f' % v, ha='center', fontsize=7,
                   color='#1f77b4' if k % 2 else '#777')
    a.set_title('Harmonics of the ROM data — odd harmonics (blue) are what hardware suppresses',
                fontsize=9)
    a.set_xlabel('harmonic'); a.set_ylabel('dB rel. fundamental')
    a.set_xticks(h); a.set_ylim(BOT, 10)

    fig.tight_layout(rect=[0, 0.02, 1, 0.975])
    fig.text(0.5, 0.008, 'sine correlation %.4f · slope up to %d LSB/sample · '
             'hardware emits a near-pure sine from this data'
             % (corr, int(np.abs(np.diff(cyc)).max())), ha='center', fontsize=8, color='#555')
    pdf.savefig(fig); plt.close(fig)

print("wrote %s   (sine corr %.4f, peak %d, max slope %d)"
      % (OUT, corr, int(np.abs(cyc).max()), int(np.abs(np.diff(cyc)).max())))
