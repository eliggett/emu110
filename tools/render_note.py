#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a U-110 preset tone at a given MIDI note, straight from the wave ROMs.

Resolves the tone's keymap to pick the right multisample, pitch-shifts from that
sample's reference note with linear interpolation, loops the sustain, and applies a
companding expansion.  Decoder model: the ROM byte is TWO'S COMPLEMENT 8-bit PCM.
"""
import numpy as np, wave, sys
from scipy.signal import resample_poly, butter, sosfilt, firwin, lfilter

RATE = 34000

def bitswap(v, o):
    r = 0; n = len(o)
    for i, b in enumerate(o):
        if v >> b & 1: r |= 1 << (n - 1 - i)
    return r

def load():
    A = [18,17,15,14,16,12,11,7,9,13,10,8,3,2,1,6,4,5,0]
    D = [1,2,7,3,5,0,4,6]
    amap = np.array([bitswap(i, A) for i in range(1 << 19)], dtype=np.int32)
    dmap = np.array([bitswap(v, D) for v in range(256)], dtype=np.uint8)
    banks = []
    for i in range(4):
        raw = np.frombuffer(open('roms/roland_t110_u110_u220_waverom%d.bin' % i, 'rb').read(), dtype=np.uint8)
        o = np.zeros(1 << 19, dtype=np.uint8); o[amap] = dmap[raw]; banks.append(o)
    tab = {}
    b0 = banks[0]
    for i in range(256):
        e = b0[0x100 + 10*i : 0x100 + 10*i + 10]
        if e[0] == 0xff and e[1] == 0xff and e[2] == 0xff: break
        tab[i] = dict(start=int(e[0]) | int(e[1]) << 8 | (int(e[2]) & 7) << 16,
                      bank=(int(e[2]) >> 4) & 3, loopmode=(int(e[2]) >> 6) & 3,
                      last=int(e[3]) | int(e[4]) << 8,
                      looplen=int(e[5]) | int(e[6]) << 8,
                      fine=int(e[7]), ref=int(e[8]))
    return banks, tab

def tone_rec(banks, tone):
    e = banks[0][0x1000 + 0x50*(tone-1) : 0x1000 + 0x50*(tone-1) + 0x50]
    return (bytes(e[:10]).decode('ascii', 'replace'),
            [b for b in e[0x10:0x1B] if b != 0xFF],
            [b for b in e[0x1B:0x27] if b != 0xFF])

def pick(splits, samples, note):
    z = sum(1 for s in splits if note > s)
    return samples[min(z, len(samples) - 1)]

def render(banks, tab, tone, note, a=3.0, seconds=3.0):
    name, splits, samples = tone_rec(banks, tone)
    sid = pick(splits, samples, note)
    s = tab[sid]
    b = banks[s['bank']]; n = s['last'] + 1
    pcm = b[s['start']:s['start'] + n].astype(np.int16)
    pcm = np.where(pcm > 127, pcm - 256, pcm).astype(np.float64)   # two's complement
    ls = max(n - s['looplen'], 0)
    looped = s['loopmode'] == 0 and s['looplen'] >= 16
    # pitch: semitones from the sample's reference note, plus its fine-tune (~1.3 cents/unit)
    semis = (note - s['ref']) + (s['fine'] - 0x40) * -0.013
    step = 2.0 ** (semis / 12.0)
    # build the byte-index trajectory, then resample with a windowed-sinc kernel rather
    # than linear interpolation (linear is a poor lowpass and audibly adds "fuzz")
    want = int(RATE * seconds)
    idxs = np.empty(want); pos = 0.0
    for k in range(want):
        if pos >= n - 1:
            if looped: pos = ls + (pos - n)
            else: idxs[k:] = -1; break
        idxs[k] = pos; pos += step
    valid = idxs >= 0
    out = np.zeros(want)
    ii = idxs[valid]
    # 8-tap windowed sinc interpolation
    base = np.floor(ii).astype(int); frac = ii - base
    acc = np.zeros(len(ii))
    for t in range(-3, 5):
        j = np.clip(base + t, 0, n - 1)
        x = frac - t
        w = np.sinc(x) * np.hanning(8)[t + 3]
        acc += pcm[j] * w
    out[valid] = acc
    if a > 0:
        m = np.abs(out) / 128.0
        out = np.sign(out) * np.expm1(a * m) / np.expm1(a)
    env = np.exp(-np.linspace(0, 2.2, len(out)))       # synthetic decay, not the real envelope
    out = out * env
    # --- output stage: PCM54HP DAC -> I-V amp -> analog LPF (IC30-35).  Without a
    # reconstruction filter the images above Nyquist are audible as "fuzz".
    out = resample_poly(out, 480, 340)                 # 34 kHz -> 48 kHz, polyphase
    sos = butter(6, 15000, 'lp', fs=48000, output='sos')
    out = sosfilt(sos, out)
    return name, sid, s['ref'], out

def write(path, x):
    p = np.percentile(np.abs(x), 99.9) + 1e-9
    d = np.clip(x / p * 0.85, -1, 1)
    w = wave.open(path, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes((d * 32767).astype('<i2').tobytes()); w.close()

if __name__ == '__main__':
    banks, tab = load()
    for spec in sys.argv[1:]:
        tone, note, a = spec.split(':')
        tone, note, a = int(tone), int(note), float(a)
        name, sid, ref, x = render(banks, tab, tone, note, a)
        p = 'listen/tone%02d_%s_n%d_a%g.wav' % (tone, name.strip().replace(' ', '').replace('.', ''), note, a)
        write(p, x)
        print("tone %2d %-11s note %3d -> sample %3d (ref %3d)  %s" % (tone, name, note, sid, ref, p))
