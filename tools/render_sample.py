#!/usr/bin/env python3
"""Render a wave-ROM sample under competing decoder hypotheses, for listening tests.

No envelope, no mixing, no interpolation -- one sample played at 1 byte per output frame,
so the pitch is the sample's own reference note.  The ONLY difference between the output
files is how a ROM byte becomes a waveform value.
"""
import numpy as np, wave, sys

RATE = 34000        # U-110 engine rate

def bitswap(v, o):
    r = 0; n = len(o)
    for i, b in enumerate(o):
        if v >> b & 1: r |= 1 << (n - 1 - i)
    return r

A = [18,17,15,14,16,12,11,7,9,13,10,8,3,2,1,6,4,5,0]
D = [1,2,7,3,5,0,4,6]

def load_banks():
    amap = np.array([bitswap(i, A) for i in range(1 << 19)], dtype=np.int32)
    dmap = np.array([bitswap(v, D) for v in range(256)], dtype=np.uint8)
    banks = []
    for i in range(4):
        raw = np.frombuffer(open('roms/roland_t110_u110_u220_waverom%d.bin' % i, 'rb').read(), dtype=np.uint8)
        o = np.zeros(1 << 19, dtype=np.uint8); o[amap] = dmap[raw]; banks.append(o)
    return banks

def unit(b):
    """The 1-3-4 float rule: sign, 3-bit exponent, 4-bit mantissa."""
    sign = -1 if b & 0x80 else 1
    v = b & 0x7f; sh = v >> 4; v &= 0x0f
    return sign * (v if sh == 0 else (0x10 + v) << (sh - 1))

TBL = np.array([unit(i) for i in range(256)], dtype=np.int64)

def sample_table(bank0):
    ent = []
    for i in range(256):
        e = bank0[0x100 + 10*i : 0x100 + 10*i + 10]
        if e[0] == 0xff and e[1] == 0xff and e[2] == 0xff: break
        ent.append(dict(i=i, start=int(e[0]) | int(e[1]) << 8 | (int(e[2]) & 7) << 16,
                        card=(int(e[2]) >> 3) & 1, bank=(int(e[2]) >> 4) & 3,
                        loopmode=(int(e[2]) >> 6) & 3,
                        last=int(e[3]) | int(e[4]) << 8,
                        looplen=int(e[5]) | int(e[6]) << 8, ref=int(e[8])))
    return ent

def render(banks, s, mode, loops=3):
    b = banks[s['bank']]; n = s['last'] + 1
    raw = b[s['start'] : s['start'] + n]
    ls = max(n - s['looplen'], 0)
    # play the body once, then repeat the loop region
    idx = list(range(n))
    if s['loopmode'] == 0 and s['looplen'] >= 16:
        for _ in range(loops): idx += list(range(ls, n))
    d = TBL[raw[idx]]
    if mode == 'memoryless':
        out = d.astype(np.float64)
    else:
        out = np.cumsum(d).astype(np.float64)
        if mode == 'differential_clamped':
            acc = 0.0; o = np.empty(len(d))
            for k, v in enumerate(d):
                acc = max(-0x7FF, min(0x7FF, acc + v)); o[k] = acc
            out = o
    m = np.abs(out).max()
    return (out / m * 0.85 * 32767).astype('<i2') if m else out.astype('<i2')

def write(path, data):
    w = wave.open(path, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(data.tobytes()); w.close()

if __name__ == '__main__':
    banks = load_banks(); tab = sample_table(banks[0])
    for want in sys.argv[1:]:
        i = int(want)
        s = next(x for x in tab if x['i'] == i)
        print("sample %3d: bank %d start 0x%05X len %5d loop %5d mode %d ref-note %d"
              % (i, s['bank'], s['start'], s['last']+1, s['looplen'], s['loopmode'], s['ref']))
        for mode in ('differential_clamped', 'differential_raw', 'memoryless'):
            p = 'listen/sample%03d_%s.wav' % (i, mode)
            write(p, render(banks, s, mode)); print("   ->", p)
