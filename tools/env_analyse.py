#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
env_analyse.py -- measure attack and release slopes from a tools/capture_env.py take.

    python3 tools/env_analyse.py listen/hardware/env
    python3 tools/env_analyse.py listen/hardware/env --emu listen/emulated/env-emu --only attack_sweep_vib

Per trial it reports, from the audio alone:

    peak      the plateau level, dB relative to the loudest trial in the take
    t_rise    onset -> within 1 dB of the plateau, seconds
    at_off    level at note-off, dB below the plateau (how much decayed while held)
    rel       release slope, dB/s, fitted in the log domain after note-off

Onsets are found in the audio, not taken from session.txt: those are send times and carry
the interface's latency.  Each segment has a known number of trials, so detection is
checked against that count and complains rather than silently misaligning.

With --emu it joins the rate byte the firmware computed for the same trial (see
tools/capture_env.py --emu-rates), which is the pairing the whole exercise is for.
"""

import argparse, csv, os, sys, wave
import numpy as np

HOP = 0.002          # 2 ms envelope hop; a 500 dB/s release still spans ~40 frames
WIN = 0.010


def load(path):
    with wave.open(path, 'rb') as w:
        n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
        a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch).astype(np.float64)
    return a.mean(1) / 32768.0, sr


def envelope(x, sr):
    """RMS envelope in dB, and the seconds-per-frame."""
    hop, win = int(HOP * sr), int(WIN * sr)
    n = (len(x) - win) // hop
    # cumulative sums make this O(n) rather than O(n * win)
    c = np.concatenate([[0.0], np.cumsum(x * x)])
    e = np.sqrt(np.maximum(c[win::hop][:n] - c[0:n * hop:hop], 0.0) / win)
    return 20 * np.log10(np.maximum(e, 1e-9)), HOP


def find_onsets(db, dt, rel):
    """Locate the trials by matched filter against their KNOWN relative spacing.

    Threshold crossings do not survive this take: where the release is slow the previous
    note is still ringing when the next arrives, so there is no quiet gap to cross, and
    small fluctuations in a tail produce extra edges.  The send times in trials.csv are
    unreliable in absolute terms (interface latency) but exact relative to each other, so
    only ONE unknown is left -- the offset of the whole sweep -- and a rise-energy score
    over the expected positions finds it.
    """
    floor = float(np.percentile(db, 5))
    pre, post = int(0.030 / dt), int(0.030 / dt)
    idx = np.array([int(round(r / dt)) for r in rel])
    best, best_off = None, 0
    for off in range(0, int(3.0 / dt)):
        p = idx + off
        if p[-1] + post >= len(db):
            break
        score = float(np.sum(db[p + post] - db[p - pre])) if p[0] - pre >= 0 else -1e9
        if best is None or score > best:
            best, best_off = score, off
    return list(idx + best_off), floor


def measure(db, dt, i0, hold, floor, i_next):
    """One trial: plateau, rise time, decay while held, release slope."""
    off = i0 + int(hold / dt)
    body = db[i0:off]
    if len(body) < 5:
        return None
    # The plateau is the loudest sustained part; use a high percentile of the second half
    # so a percussive tone's initial transient does not become the reference.
    peak = float(np.max(body))
    half = body[len(body) // 2:]
    at_off = float(np.median(db[max(off - 15, i0):off])) if off - 15 > i0 else float(half[-1])

    # rise: first frame within 1 dB of the plateau
    hit = np.where(body >= peak - 1.0)[0]
    t_rise = float(hit[0] * dt) if len(hit) else float('nan')

    # attack slope, dB/s, fitted over the straight part of the rise.  Measured the same
    # way as the release so the two are directly comparable; only meaningful when the
    # envelope is slower than the sample's own attack transient, which is why t_rise is
    # reported alongside (a t_rise at the tone's floor means `att` is that transient).
    top = int(np.argmax(body))                      # only the rise, never the decay after it
    up = body[:top + 1]
    lo_a, hi_a = floor + 12.0, peak - 3.0
    rise = np.where((up >= lo_a) & (up <= hi_a))[0]
    att = float('nan')
    if len(rise) >= 4 and hi_a - lo_a >= 6.0:
        a2, b2 = rise[0], rise[-1]
        if b2 - a2 >= 3:
            att = float(np.polyfit(np.arange(a2, b2 + 1) * dt, up[a2:b2 + 1], 1)[0])

    # release: fit a line in dB from 3 dB below the note-off level down to 6 dB above the
    # noise floor, or 60 dB of fall, whichever comes first.  Skip the first 10 ms: the
    # note-off itself lands somewhere inside a frame and the first frame straddles it.
    # Stop well before the next trial: with a short gap and a fast release the level is
    # back at the noise floor long before the next note-on, and a fit that runs into that
    # onset comes out an order of magnitude too shallow.  (This bit the level_sweep -- its
    # 1.3 s gap put the next attack inside a fixed 3 s window and turned an 890 dB/s
    # release into a reported 7.6.)
    end = min(off + int(24.0 / dt), i_next - int(0.05 / dt))
    seg = db[off + int(0.010 / dt): max(end, off + int(0.05 / dt))]
    if len(seg) < 4:
        return dict(peak=peak, t_rise=t_rise, at_off=at_off - peak, rel=float('nan'),
                        att=att, n=0)
    top = at_off - 3.0
    bot = max(floor + 6.0, at_off - 60.0)
    inside = np.where((seg <= top) & (seg >= bot))[0]
    if len(inside) < 3:
        return dict(peak=peak, t_rise=t_rise, at_off=at_off - peak, rel=float('nan'),
                        att=att, n=0)
    a, b = inside[0], inside[-1]
    # keep it monotone: stop at the first frame that climbs back up by 3 dB
    for k in range(a + 1, b + 1):
        if seg[k] > seg[k - 1] + 3.0:
            b = k - 1
            break
    if b - a < 2:
        return dict(peak=peak, t_rise=t_rise, at_off=at_off - peak, rel=float('nan'),
                        att=att, n=0)
    t = np.arange(a, b + 1) * dt
    slope = float(np.polyfit(t, seg[a:b + 1], 1)[0])
    return dict(peak=peak, t_rise=t_rise, at_off=at_off - peak, rel=slope,
                att=att, n=int(b - a + 1))


def emu_rates(emu_dir):
    """{(segment, trial): (reg07, reg06)} from an emulator render's rates.txt."""
    p = os.path.join(emu_dir, 'rates.txt')
    if not os.path.exists(p):
        return {}
    out, counts = {}, {}
    for line in open(p):
        f = line.split()
        if len(f) < 4 or f[0] == 'sweep':
            continue
        seg, tag, hi, lo = f[0], f[1], f[2], f[3]
        if not hi.lstrip('-').isdigit():
            continue
        i = counts.get(seg, 0)
        counts[seg] = i + 1
        out[(seg, i)] = (int(hi), int(lo), tag)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('take', help='directory from tools/capture_env.py')
    ap.add_argument('--emu', default=None, help='matching render from render_u110.py')
    ap.add_argument('--only', default=None, help='comma-separated sweep names')
    ap.add_argument('--csv', default=None, help='also write the table here')
    a = ap.parse_args()

    trials = list(csv.DictReader(open(os.path.join(a.take, 'trials.csv'))))
    by_seg = {}
    for r in trials:
        by_seg.setdefault(r['segment'], []).append(r)
    rates = emu_rates(a.emu) if a.emu else {}

    files = {}
    for f in sorted(os.listdir(a.take)):
        if f.endswith('.wav') and f != 'session.wav':
            files[f[3:-4]] = os.path.join(a.take, f)

    want = [s.strip() for s in a.only.split(',')] if a.only else list(by_seg)
    rows = []
    for seg in want:
        if seg not in files or seg not in by_seg:
            print("skipping %s (no wav or no trials)" % seg)
            continue
        x, sr = load(files[seg])
        db, dt = envelope(x, sr)
        t0 = float(by_seg[seg][0]['onset_s'])
        rel = [float(r['onset_s']) - t0 for r in by_seg[seg]]
        on, floor = find_onsets(db, dt, rel)
        print("\n== %s ==  %d trials, floor %.1f dB, first onset at %.3f s"
              % (seg, len(by_seg[seg]), floor, on[0] * dt))
        print("   %-20s %6s %6s %7s %8s %9s %9s %5s %6s"
              % ("trial", "reg07", "reg06", "peak", "t_rise", "att dB/s", "rel dB/s",
                 "n", "at_off"))
        for i, (r, i0) in enumerate(zip(by_seg[seg], on)):
            i_next = on[i + 1] if i + 1 < len(on) else len(db)
            m = measure(db, dt, i0, float(r['hold_s']), floor, i_next)
            if m is None:
                continue
            hi, lo, _ = rates.get((seg, i), ('', '', ''))
            # n is how many envelope frames the release fit had: below ~10 the 10 ms RMS
            # window is smearing the ramp and the slope is a lower bound, not a measurement.
            print("   %-20s %6s %6s %7.1f %8.3f %9.1f %9.1f %5d %6.1f"
                  % (r['tag'], hi, lo, m['peak'], m['t_rise'], m['att'], m['rel'],
                     m['n'], m['at_off']))
            rows.append(dict(segment=seg, trial=i, tag=r['tag'], note=r['note'],
                             velocity=r['velocity'], hold_s=r['hold_s'],
                             reg07=hi, reg06=lo, peak_db=round(m['peak'], 2),
                             attack_db_s=round(m['att'], 2),
                             t_rise_s=round(m['t_rise'], 4),
                             release_db_s=round(m['rel'], 2), release_frames=m['n'],
                             decay_while_held_db=round(m['at_off'], 2)))

    if a.csv and rows:
        with open(a.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
        print("\nwrote %s (%d rows)" % (a.csv, len(rows)))


if __name__ == '__main__':
    main()
