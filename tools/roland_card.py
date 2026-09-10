#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read Roland PCM cards and wave ROMs -- the U-110's own format and the R-8's.

Both machines hang their PCM memory off the same Fujitsu MB87419/MB87420 pair, wired the
same way, so one descrambler serves both.  Confirmed the cheap way: the dumps in
`~/Downloads/roland_sn-u-stuff_for-elliott/` ship a `.bin` (as read off the chip) beside a
`.opn`/`.ope` (descrambled), and `descramble(bin) == opn` byte for byte for every R-8 card,
every D-70 wave ROM that has a pair, and the SN-SPLA card -- using the ADDR_ORDER and
DATA_ORDER this project derived from U-110 firmware alone.

Two address spaces are in play and mixing them up is the easy mistake:

  PHYSICAL   offsets into the dump file, which is what the chip's pins see.
  LOGICAL    what the firmware asks for.  Tone records, sample records and sample data
             all live here.  descramble() maps physical -> logical.

The 48-byte ID header is the exception: it sits linear in PHYSICAL space at offset 0,
because the firmware reads it through the compensation tables at 0x9357/0x9257 rather than
through the tone generator (ROM-ANALYSIS.md section 6.4).  So read the header off the raw
bytes and everything else off the descrambled image.
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from envelope_measure import ADDR_ORDER, DATA_ORDER, _bitswap, decode_float8, ENGINE_RATE

BANK_SIZE = 1 << 19                      # 512 KB, one card or one internal wave ROM

_AMAP = np.array([_bitswap(i, ADDR_ORDER) for i in range(BANK_SIZE)], dtype=np.int32)
_DMAP = np.array([_bitswap(v, DATA_ORDER) for v in range(256)], dtype=np.uint8)


def descramble(raw):
    """Physical dump bytes -> logical image.  Undoes both the address and the data
    permutation, which is what the hardware wiring does between the ROM and IC15."""
    raw = np.asarray(raw, dtype=np.uint8)
    if raw.size != BANK_SIZE:
        raise ValueError("expected a %d-byte bank, got %d" % (BANK_SIZE, raw.size))
    out = np.zeros(BANK_SIZE, dtype=np.uint8)
    out[_AMAP] = _DMAP[raw]
    return out


def scramble(logical):
    """Logical image -> physical dump bytes, the inverse of descramble().  This is what a
    converter has to write: a file that looks like it was read off a real card."""
    logical = np.asarray(logical, dtype=np.uint8)
    if logical.size != BANK_SIZE:
        raise ValueError("expected a %d-byte bank, got %d" % (BANK_SIZE, logical.size))
    inv_d = np.zeros(256, dtype=np.uint8)
    inv_d[_DMAP] = np.arange(256, dtype=np.uint8)
    return inv_d[logical[_AMAP]]


def load_bank(path, offset=0):
    """Returns (logical, physical).  `offset` picks a 512 KB window out of a larger dump --
    the R-8 MK II's wave ROMs are 1 MB, two banks each."""
    raw = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8)
    if raw.size < offset + BANK_SIZE:
        raise ValueError("%s: no 512 KB bank at offset %#x (file is %d bytes)"
                         % (path, offset, raw.size))
    phys = raw[offset:offset + BANK_SIZE]
    return descramble(phys), phys


# --------------------------------------------------------------- identification

U110_MAGIC = b'RolandU-110 N\xb1S\xac'
R8_MAGIC   = b'RolandR-8   N\xb1S\xac'


def identify(phys):
    """What kind of card this is, from the 16-byte magic the firmware checks on mount."""
    magic = bytes(phys[0:16])
    if magic == U110_MAGIC: return 'u110'
    if magic == R8_MAGIC:   return 'r8'
    if bytes(phys[0:6]) == b'Roland': return 'roland-other'
    return 'unknown'


def _le16(b, o):
    return int(b[o]) | int(b[o + 1]) << 8


# --------------------------------------------------------------- U-110 side

def u110_samples(logical, limit=256):
    """The 10-byte sample records at logical 0x0100.  See R8-CONVERSION.md section 3.

    byte 0-1  start address, low 16 bits          (bytes, not words)
    byte 2    bits 0-2  start bits 16-18
              bit  3    "this is a card" flag  |  the log calls bits 3-5 the "segment"
              bits 4-5  bank / slot            |
              bits 6-7  loop mode: 0 forward, 1 off, 2 ping-pong
    byte 3-4  length - 1, in bytes
    byte 5-6  loop length, counted back from the end
    byte 7    fine tune, 0x40 = centre
    byte 8    reference note -- the MIDI note at which the sample plays at its stored rate
    byte 9    per-sample level trim, 0x40 = nominal
    """
    out = {}
    for i in range(limit):
        e = logical[0x100 + 10 * i: 0x100 + 10 * i + 10]
        if len(e) < 10 or (e[0] == 0xFF and e[1] == 0xFF and e[2] == 0xFF):
            break
        out[i] = dict(start=int(e[0]) | int(e[1]) << 8 | (int(e[2]) & 7) << 16,
                      segment=(int(e[2]) >> 3) & 7, loopmode=(int(e[2]) >> 6) & 3,
                      length=(_le16(e, 3) + 1), looplen=_le16(e, 5),
                      fine=int(e[7]), ref=int(e[8]), level=int(e[9]))
    return out


def u110_tones(logical, count=99):
    """The 80-byte tone parameter records at logical 0x1000.

    Each record is a 10-byte name, 6 header bytes, then TWO 32-byte partials.  A partial is
    11 key split points, then 12 sample indices, then 9 parameter bytes -- NOT the 8-and-9
    an earlier revision of ROM-ANALYSIS.md section 6.6 recorded.  Cross-checked against the
    third-party `*_list.log` decodes that ship with the dumps, which agree exactly.

    Zone selection, from the firmware's own routine: walk the splits and take the first one
    the note does not exceed; if the note is past every split, take the twelfth sample.
    """
    out = []
    for t in range(count):
        r = logical[0x1000 + 0x50 * t: 0x1000 + 0x50 * t + 0x50]
        if len(r) < 0x50:
            break
        name = bytes(r[:10]).decode('latin1')
        partials = []
        for p in (0x10, 0x30):
            partials.append(dict(splits=[int(x) for x in r[p:p + 11]],
                                 samples=[int(x) for x in r[p + 11:p + 23]],
                                 params=bytes(r[p + 23:p + 32])))
        out.append(dict(index=t, name=name, header=bytes(r[10:16]), partials=partials,
                        raw=bytes(r)))
    return out


def u110_pick(partial, note):
    """Which sample index a partial plays for a given MIDI note."""
    z = sum(1 for s in partial['splits'] if s != 0xFF and note > s)
    return partial['samples'][min(z, len(partial['samples']) - 1)]


# --------------------------------------------------------------- R-8 side

def r8_card(logical, phys):
    """An SN-R8 card's directory.

    logical 0x0080  card id  (catalogue number MINUS ONE: SN-R8-10 carries 0x09)
    logical 0x0081  tone count -- 26 on every card seen
    logical 0x0090  tone offset table, `count` little-endian words, relative to 0x0100

    Each tone record is 48 bytes: an 8-byte NUL-padded name, 4 header bytes, then TWO
    18-byte sample blocks.  A block is nine little-endian words that are close to a
    register image for the MB87419 voice that will play it:

      w0  registers 02/03  bank + mode.  Bit 10 (0x0400) is wave address bit 18.
      w1  registers 0A/0B  start address, in 4-byte units
      w2  registers 08/09  start fraction -- 0x4000 on every block seen
      w3  registers 0C/0D  END address, in 4-byte units
      w4  registers 0E/0F  loop address; sits PAST the end on one-shots, which is all
                           of them, so nothing on these cards loops
      w5  registers 04/05  playback step
      w6..w8              level and envelope; all zero on blocks the card does not use
    """
    cid, ntone = int(logical[0x80]), int(logical[0x81])
    tones = []
    for i in range(ntone):
        base = 0x100 + _le16(logical, 0x90 + 2 * i)
        r = logical[base:base + 0x30]
        blocks = []
        for bo in (0x0c, 0x1e):
            w = [_le16(r, bo + 2 * k) for k in range(9)]
            bank = 0x40000 if (w[0] & 0x0400) else 0
            blocks.append(dict(words=w, bank=bank,
                               start=(w[1] << 2) | bank, end=(w[3] << 2) | bank,
                               loop=(w[4] << 2) | bank, step=w[5],
                               used=any(w[6:9])))
        tones.append(dict(index=i, base=base,
                          name=bytes(r[:8]).decode('latin1').rstrip('\x00 '),
                          header=bytes(r[8:12]), blocks=blocks, raw=bytes(r)))
    return dict(card_id=cid, part=cid + 1, count=ntone,
                label=bytes(phys[16:32]).decode('latin1'), tones=tones)


# --------------------------------------------------------------- audio

def integrate(logical, start, end):
    """Decode a span of sample bytes to audio.

    The stored byte is a DELTA, not a level (ROM-ANALYSIS.md section 3a), so the decoded
    values are summed.  Verified independently for the R-8: over each of its samples the
    deltas sum to zero and the running total stays inside the decoder's own +/-2048, both
    of which are true of the U-110's data and neither of which would hold if these bytes
    were absolute levels.
    """
    if not (0 < end - start <= BANK_SIZE):
        return np.zeros(0)
    return np.cumsum(decode_float8(logical[start:end]))


def write_wav(path, x, rate=ENGINE_RATE, peak=0.89):
    import wave as _wave
    x = np.asarray(x, dtype=np.float64)
    p = np.abs(x).max()
    d = np.clip(x / p * peak, -1, 1) if p > 0 else x
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    w = _wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(rate))
    w.writeframes((d * 32767).astype('<i2').tobytes()); w.close()
