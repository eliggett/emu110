#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
scratch_analyse.py -- compare a `capture_env.py --set scratch` take against its render.

    python3 tools/scratch_analyse.py \
        --hw listen/hardware/env3 --emu listen/emulated/env3-emu

The scratch set dictates the whole patch over SysEx, so tone is the only variable left.
That makes the decay slope comparable trial by trial, which is what this measures: dB/s
fitted between 3 and 22 dB below each note's own peak, keeping clear of the noise floor.

Onsets are found in the audio, not read from trials.csv -- the CSV carries SESSION times
and the segment files start at the segment, which is a mismatch that silently produces
garbage.  Both sides are located the same way, so nothing depends on the two takes lining
up in absolute time.
"""

import argparse, os, wave
import numpy as np

SEGMENTS = [
    ('01_scratch_level_vib.wav',      8, [str(v) for v in (127,96,72,52,36,24,14,8)]),
    ('02_scratch_level_fbass.wav',    8, [str(v) for v in (127,96,72,52,36,24,14,8)]),
    ('03_scratch_level_marimba.wav',  8, [str(v) for v in (127,96,72,52,36,24,14,8)]),
    ('04_scratch_velocity_vib.wav',   7, ['vel%d'%v for v in (127,110,90,70,50,30,15)]),
    ('05_scratch_velocity_marimba.wav',7,['vel%d'%v for v in (127,110,90,70,50,30,15)]),
    ('06_scratch_env_release.wav',    7, ['rel%+d'%d for d in (-4,-3,-2,-1,0,1,2)]),
    ('07_scratch_env_attack.wav',     6, ['atk%+d'%d for d in (-4,-3,-2,-1,0,1)]),
    ('08_scratch_tones.wav',          6, ['vib','bell','slap','marimba','fbass','piano']),
    ('09_scratch_keys.wav',           7, ['n%d'%n for n in (24,36,48,60,72,84,96)]),
    ('10_scratch_slow_vib.wav',       3, ['rel+0','rel-4','rel-7']),
    ('11_scratch_slow_fbass.wav',     3, ['rel+0','rel-4','rel-7']),
    ('12_scratch_velo_sens.wav',      6, ['s0/v100','s0/v40','s8/v100','s8/v40','s15/v100','s15/v40']),
]


def envelope(path, step=0.01, win=0.03):
    with wave.open(path, 'rb') as w:
        sr, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, ch)
    x = x[:, 0].astype(np.float64)
    n = int(win * sr)
    c = np.cumsum(np.concatenate([[0.0], x ** 2]))
    idx = np.arange(0, len(x) - n, int(step * sr))
    return idx / sr, 10 * np.log10((c[idx + n] - c[idx]) / n + 1e-12)


def onsets(t, v, n_expect, spacing):
    """Peak of each note-on rise.  Spacing keeps a decaying tail from being re-triggered."""
    d = np.diff(v)
    out, i = [], 0
    guard = int(spacing * 0.6 / (t[1] - t[0]))
    while i < len(d) and len(out) < n_expect:
        if d[i] > 5 and (not out or t[i] - out[-1] > spacing * 0.6):
            j = i
            while j + 1 < len(v) and v[j + 1] > v[j]:
                j += 1
            out.append(t[j])
            i += guard
        else:
            i += 1
    return out


def slope(t, v, o, lo=3.0, hi=22.0, floor_margin=8.0, span=9.0):
    m = (t >= o - 0.05) & (t <= o + span)
    ts, vs = t[m] - o, v[m]
    if len(ts) < 20:
        return None
    pk, flr = vs.max(), np.percentile(v, 3)
    sel = (vs <= pk - lo) & (vs >= pk - hi) & (vs >= flr + floor_margin) & (ts > 0.02)
    if sel.sum() < 10:
        return None
    return -np.polyfit(ts[sel], vs[sel], 1)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hw', default='listen/hardware/env3')
    ap.add_argument('--emu', default='listen/emulated/env3-emu')
    ap.add_argument('--spacing', type=float, default=11.16)
    args = ap.parse_args()

    print('decay slope, dB/s, fitted 3-22 dB below each note\'s own peak')
    print('%-26s %-10s %9s %9s %8s' % ('segment', 'trial', 'hardware', 'emulator', 'error'))
    worst = []
    for fn, n, labels in SEGMENTS:
        hp, ep = os.path.join(args.hw, fn), os.path.join(args.emu, fn)
        if not (os.path.exists(hp) and os.path.exists(ep)):
            continue
        th, vh = envelope(hp)
        te, ve = envelope(ep)
        sp = args.spacing if 'slow' not in fn else 24.6
        oh, oe = onsets(th, vh, n, sp), onsets(te, ve, n, sp)
        name = fn[3:].replace('.wav', '')
        errs = []
        for i, lab in enumerate(labels):
            if i >= len(oh) or i >= len(oe):
                continue
            a, b = slope(th, vh, oh[i]), slope(te, ve, oe[i])
            if a is None or b is None:
                continue
            e = b / a - 1.0
            errs.append(abs(e))
            print('%-26s %-10s %9.2f %9.2f %+7.0f%%'
                  % (name if i == 0 else '', lab, a, b, 100 * e))
        if errs:
            worst.append((float(np.mean(errs)), name))
            print('%-26s %-10s %9s %9s %+7.0f%%' % ('', 'MEAN', '', '', 100 * np.mean(errs)))
        print()
    worst.sort(reverse=True)
    print('segments ranked by mean absolute error:')
    for e, n in worst:
        print('   %-28s %+5.0f%%' % (n, 100 * e))


if __name__ == '__main__':
    main()
