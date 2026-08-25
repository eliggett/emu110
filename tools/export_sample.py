#!/usr/bin/env python3
"""Export a wave-ROM sample straight to a WAV, decoded and optionally EQ'd.

No looping, no pitch shifting, no envelope -- just the stored waveform at the engine's
own 32 kHz, so the source material can be examined on its own terms.

    python3 tools/export_sample.py --index 121 122        # by sample-table index
    python3 tools/export_sample.py --tone 58              # every multisample a tone uses
    python3 tools/export_sample.py --index 122 --flat     # skip the EQ

Writes to listen/renders/ by default.  Each sample produces:
    <name>.wav        decoded, de-emphasised, through the Fig. 4 output filter
    <name>_flat.wav   decoded only, with --both
"""
import argparse, os, sys, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from envelope_measure import load_roms, decode_float8, rom_slice, ENGINE_RATE, output_filter
from scipy.signal import lfilter

DEEMPH_HZ = 20.0        # matches roland_u110.cpp
DEEMPH_MAKEUP = 182.0


def table(banks):
    b0 = banks[0]
    out = {}
    for i in range(256):
        e = b0[0x100 + 10 * i:0x100 + 10 * i + 10]
        if e[0] == 0xff and e[1] == 0xff and e[2] == 0xff:
            break
        out[i] = dict(
            start=(((int(e[2]) >> 4) & 3) << 20) | (int(e[0]) | int(e[1]) << 8 | (int(e[2]) & 7) << 16),
            lm=(int(e[2]) >> 6) & 3, length=int(e[3]) | int(e[4]) << 8,
            looplen=int(e[5]) | int(e[6]) << 8, fine=int(e[7]), ref=int(e[8]))
    return out


def deemphasise(x):
    k = np.exp(-2 * np.pi * DEEMPH_HZ / ENGINE_RATE)
    return lfilter([1 - k], [1, -k], x) * DEEMPH_MAKEUP


def write(path, x, peak_dbfs=-3.0):
    pk = np.abs(x).max()
    g = (10 ** (peak_dbfs / 20) * 32767) / max(pk, 1e-9)
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(ENGINE_RATE)
    w.writeframes(np.clip(x * g, -32768, 32767).astype('<i2').tobytes())
    w.close()
    return pk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--index', type=int, nargs='*', default=[])
    ap.add_argument('--out-dir', default='listen/renders')
    ap.add_argument('--tail', type=int, default=64, help='extra bytes to include past `end`')
    ap.add_argument('--flat', action='store_true', help='no EQ')
    ap.add_argument('--both', action='store_true', help='write EQ and flat versions')
    args = ap.parse_args()

    banks = load_roms(); tab = table(banks)
    os.makedirs(args.out_dir, exist_ok=True)
    for i in args.index:
        if i not in tab:
            print("no table entry %d" % i); continue
        E = tab[i]
        n = E['length'] + args.tail
        raw = decode_float8(rom_slice(banks, E['start'], n))
        loop_at = E['length'] - E['looplen']
        base = "sample%03d_ref%d" % (i, E['ref'])
        print("sample %3d: start 0x%06X  length %6d (%.3f s at 32 kHz)  loop at %6d  "
              "loopmode %d  ref note %d"
              % (i, E['start'], E['length'], E['length'] / ENGINE_RATE, loop_at, E['lm'], E['ref']))
        for tag, eq in ((("", True), ("_flat", False)) if args.both
                        else ((("_flat", False),) if args.flat else (("", True),))):
            y = raw.copy()
            if eq:
                y = output_filter(deemphasise(y), ENGINE_RATE)
            p = os.path.join(args.out_dir, base + tag + ".wav")
            pk = write(p, y)
            print("    %-52s raw peak %8.0f" % (p, pk))
        print("    loop point at sample %d (%.4f s), `end` at %d (%.4f s), "
              "%d bytes of tail beyond it"
              % (loop_at, loop_at / ENGINE_RATE, E['length'], E['length'] / ENGINE_RATE, args.tail))


if __name__ == '__main__':
    main()
