#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble a U-110 ping-pong loop offline, treating the ROM bytes as deltas.

Nothing here touches the emulator.  The point is to demonstrate the scheme end to end on
its own, and to show whether the integrator needs a leak.

  read byte -> expand via the 1-3-4 rule -> negate the delta when running backwards
            -> accumulate -> (optional leak) -> pitch shift -> Fig. 4 filter -> WAV

The integration is done at BYTE granularity along the path the chip actually walks, so the
reverse pass retraces the forward pass exactly; pitch shifting happens afterwards.

    python3 tools/stitch_pingpong.py --index 121 122
    python3 tools/stitch_pingpong.py --index 122 --leak 5 --cycles 8
    python3 tools/stitch_pingpong.py --index 122 --no-repeat-endpoint
"""
import argparse, os, sys, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from envelope_measure import load_roms, decode_float8, rom_slice, ENGINE_RATE, output_filter

# playback rates the firmware actually used for these two, from a register trace
NATIVE_RATE = {121: 0.74054, 122: 0.70026}


def entry(banks, i):
    b0 = banks[0]
    e = b0[0x100 + 10 * i:0x100 + 10 * i + 10]
    if e[0] == 0xff and e[1] == 0xff and e[2] == 0xff:
        return None
    return dict(start=(((int(e[2]) >> 4) & 3) << 20) | (int(e[0]) | int(e[1]) << 8 | (int(e[2]) & 7) << 16),
                lm=(int(e[2]) >> 6) & 3, length=int(e[3]) | int(e[4]) << 8,
                looplen=int(e[5]) | int(e[6]) << 8, ref=int(e[8]))


def path_indices(length, loop, cycles, repeat_endpoint):
    """The byte indices the chip visits: attack once, then ping-pong over [loop, length].

    `end` is INCLUSIVE.  The integral of the deltas returns to zero exactly at `end`
    (-1 on sample 121, -15 on 122, against a waveform swinging +/-800) and again at
    `loop` -- Roland put both turning points on zero crossings of the integrated
    waveform.  Turning at `end - 1` instead lands on +151, which is what makes an
    inverted reflection jump instead of carrying through."""
    hi = length
    out = [np.arange(0, hi + 1)]                     # attack + first forward pass
    for c in range(cycles):
        back = np.arange(hi - 1, loop - 1, -1) if not repeat_endpoint \
            else np.arange(hi, loop - 1, -1)
        fwd = np.arange(loop + 1, hi + 1) if not repeat_endpoint \
            else np.arange(loop, hi + 1)
        out += [back, fwd]
    return np.concatenate(out)


def integrate(deltas, idx, leak_hz, invert=False):
    """Walk the path, adding the delta going forward and subtracting it going back.

    With `invert`, the reverse pass is reflected about the accumulator's value AT THE TURN,
    not about zero.  Writing the output as out = s*acc + o with s = +/-1, continuity at a
    turn where acc = c requires

        o_new = o_old + 2*s_old*c ,   s_new = -s_old

    Reflecting about zero instead leaves a kink of 2c at every turn, and makes the
    waveform's DC offset alternate sign at the traverse rate -- a low-frequency square wave.
    The loop regions integrate to exactly zero, but the ATTACK does not (it leaves -1 on
    sample 121 and -15 on 122), so c is not zero and the distinction matters.
    """
    keep = 1.0 if leak_hz <= 0 else np.exp(-2 * np.pi * leak_hz / ENGINE_RATE)
    out = np.empty(len(idx))
    prev = int(idx[0])
    acc = float(deltas[prev])
    sign, off = 1.0, 0.0
    last_dir = 0
    out[0] = sign * acc + off
    for k in range(1, len(idx)):
        cur = int(idx[k])
        if cur > prev:
            step, d = deltas[cur], +1
        elif cur < prev:
            step, d = -deltas[prev], -1
        else:
            step, d = 0.0, last_dir
        if invert and d != 0 and last_dir != 0 and d != last_dir:
            off += 2.0 * sign * acc          # acc is still the value AT the turn
            sign = -sign
        if d != 0:
            last_dir = d
        acc = acc * keep + step
        out[k] = sign * acc + off
        prev = cur
    return out


def resample(x, rate):
    n = int(len(x) / rate)
    p = np.arange(n) * rate
    i = np.clip(p.astype(int), 0, len(x) - 2)
    f = p - i
    return x[i] * (1 - f) + x[i + 1] * f


def write(path, x, sr, peak_dbfs=-3.0):
    g = (10 ** (peak_dbfs / 20) * 32767) / max(np.abs(x).max(), 1e-9)
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(np.clip(x * g, -32768, 32767).astype('<i2').tobytes())
    w.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--index', type=int, nargs='+', default=[121, 122])
    ap.add_argument('--cycles', type=int, default=6)
    ap.add_argument('--leak', type=float, default=0.0, help='integrator leak corner, Hz (0 = none)')
    ap.add_argument('--rate', type=float, default=None, help='playback rate (default: as played)')
    ap.add_argument('--no-repeat-endpoint', action='store_true',
                    help='turn ON the endpoint sample instead of repeating it')
    ap.add_argument('--invert', action='store_true',
                    help='flip polarity on the reverse pass (continuous at a zero crossing)')
    ap.add_argument('--no-filter', action='store_true')
    ap.add_argument('--out-dir', default='listen/emulated/scratch')
    args = ap.parse_args()

    banks = load_roms()
    os.makedirs(args.out_dir, exist_ok=True)
    for i in args.index:
        E = entry(banks, i)
        if E is None:
            print("no entry %d" % i); continue
        length, loop = E['length'], E['length'] - E['looplen']
        rate = args.rate if args.rate else NATIVE_RATE.get(i, 1.0)
        d = decode_float8(rom_slice(banks, E['start'], length + 2))
        idx = path_indices(length, loop, args.cycles, not args.no_repeat_endpoint)
        w = integrate(d, idx, args.leak, args.invert)

        # --- diagnostics: what the data says about drift and about the turns
        dirs = np.sign(np.diff(idx.astype(np.int64)))
        nz = dirs[dirs != 0]
        pos = np.where(dirs != 0)[0]
        turns = [int(pos[k + 1]) for k in range(len(nz) - 1) if nz[k] != nz[k + 1]]
        step = np.abs(np.diff(w))
        typ = np.median(step[step > 0])
        print("sample %d  (ref %d, loop mode %d)  length %d, loop at %d, rate %.5f"
              % (i, E['ref'], E['lm'], length, loop, rate))
        print("   path %d samples, %d turns, leak %s"
              % (len(idx), len(turns), ("%g Hz" % args.leak) if args.leak else "none"))
        if turns:
            at = np.array([step[max(t - 1, 0):t + 1].max() for t in turns])
            print("   step across the turns: %s  (typical step %.1f)"
                  % (" ".join("%.0f" % v for v in at[:8]), typ))
            # Curvature is what tells a corner from a smooth continuation: a bounce and a
            # carry-through have the SAME |step|, but only the bounce reverses the slope.
            c = np.abs(np.diff(w, 2))
            ctyp = np.median(c[c > 0])
            cat = np.array([c[max(t - 2, 0):t + 1].max() for t in turns])
            print("   CURVATURE at the turns: %s  (typical %.1f)"
                  % (" ".join("%.0f" % v for v in cat[:8]), ctyp))
            print("   worst turn: %.2f x typical step, %.2f x typical curvature"
                  % (at.max() / typ, cat.max() / ctyp))
        # accumulator value at each turn tells us about drift
        accs = np.array([w[t] for t in turns])
        if len(accs) > 2:
            same_phase = accs[::2]
            print("   accumulator at like turns: %s" % (" ".join("%+.0f" % v for v in same_phase[:6])))
            drift = np.diff(same_phase)
            print("   drift per full cycle: %s  (max |drift| %.1f = %.3f x waveform RMS)"
                  % (" ".join("%+.1f" % v for v in drift[:5]), np.abs(drift).max(),
                     np.abs(drift).max() / np.sqrt((w ** 2).mean())))
        print("   waveform: mean %+.1f  RMS %.1f  min %.0f  max %.0f"
              % (w.mean(), np.sqrt((w ** 2).mean()), w.min(), w.max()))

        y = resample(w, rate)
        if not args.no_filter:
            y = output_filter(y, ENGINE_RATE)
        tag = "s%d_stitch%s%s%s" % (i, "_inv" if args.invert else "",
                                    "_leak%g" % args.leak if args.leak else "",
                                    "_nofilt" if args.no_filter else "")
        p = os.path.join(args.out_dir, tag + ".wav")
        write(p, y, ENGINE_RATE)
        print("   -> %s  (%.2f s)\n" % (p, len(y) / ENGINE_RATE))


if __name__ == '__main__':
    main()
