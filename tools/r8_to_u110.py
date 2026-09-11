#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""R-8 SPECIFIC.  Build a U-110 PCM card out of Roland SN-R8 cards.

Convert one card whole, or assemble your own kit from several.  No sample is transcoded:
the R-8 and the U-110 store PCM identically (analysis/R8-CONVERSION.md section 1), so the
bytes are carried across and only the tables around them are authored.

    r8_to_u110.py SN-R8-10_Dance.bin -o roms/sn-u110-26_r8-10.bin
    r8_to_u110.py *.bin --list
    r8_to_u110.py *.bin --kit mykit.txt -o roms/sn-u110-30_mykit.bin
    r8_to_u110.py roms/sn-u110-30_mykit.bin --verify

A kit entry is `[card:]name[+n|-n]` -- `909_K`, `10:909_S`, `09:ATAK_T1+5`.  Listing the
same instrument more than once at different offsets builds a tuned set out of one drum,
which is how the factory cards make their toms, and the sample is stored only once however
many keys it lands on.  A kit file holds one entry per line with `#` for comments.

Each R-8 tone carries TWO sample blocks that the machine sounds together -- a kick's click
and its boom, a snare's noise and its shell tone -- and both are summed onto the key
(`--no-premix` keeps only the first).  A U-110 drum tone takes 22 keys, so a 26-instrument
card becomes two tones.

The output filename matters: the plugin finds cards by looking for `snu110NN` in the name
with separators stripped (Voltaire110Plugin.cpp, cardNumberFromName), so keep `sn-u110-NN`
in it and let the rest of the name say what the card is.
"""
import argparse, os, re, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roland_card as rc

# --- the U-110 card layout, measured across all fifteen SN-U110 cards ------------------
# R8-CONVERSION.md section 3.3.  Every real card starts its sample data at 0x4001-0x4004,
# because the full 99-record tone table is reserved whether the card fills it or not.
SAMPLE_TABLE   = 0x0100
SAMPLE_RECORDS = 384                      # (0x1000 - 0x0100) / 10
TONE_TABLE     = 0x1000
TONE_STRIDE    = 0x50
TONE_RECORDS   = 99
DATA_BASE      = 0x4000
DATA_END       = rc.BANK_SIZE

ZONES_PER_PARTIAL = 12                    # 11 splits + a twelfth "past every split" zone
MAX_KEYS          = ZONES_PER_PARTIAL - 1 # one zone must hold silence, so 11 keys

# Parameter bytes lifted from SN-U110-10 "ROCK DRUMS", a real U-110 drum tone.  The nine
# per-partial bytes are level and envelope and are NOT decoded yet (R8-CONVERSION.md
# section 4), so phase 1 copies values known to work rather than inventing them.
ROCK_DRUMS_HEADER = bytes([0x80, 0x00, 0x40, 0x00, 0x00, 0x00])
ROCK_DRUMS_P1     = bytes([0x7F, 0x7F, 0x7F, 0x6D, 0x00, 0x55, 0x7F, 0x50, 0xA0])
ROCK_DRUMS_P2     = bytes([0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x4C, 0x7F, 0x4C, 0x20])

BLANK_TONE_HEADER = bytes([0x00, 0x00, 0x40, 0x00, 0x02, 0x00])

# --- the "TABLE" record ---------------------------------------------------------------
#
# The tone record at logical 0x3760 -- index 126, past anything a program change can reach
# -- whose name field reads "TABLE".  Its partial-1 sample indices are the eight drum maps
# described above.  A drum tone is silent on almost every key without it.
TABLE_NAME    = b'TABLE     '
TABLE_HEADER  = bytes([0x00, 0x00, 0x2A, 0x00, 0x00, 0x00])
TABLE_SPLITS  = bytes([0x1F, 0x2B, 0x31, 0x37, 0x3F, 0x47, 0x4B, 0xFF, 0xFF, 0xFF, 0xFF])
TABLE_P1PAR   = bytes([0x00] * 8 + [0x71])
TABLE_P2PAR   = bytes([0x7F] * 8 + [0x00])
TABLE_RECORD  = 126

SILENCE_BYTES = 8                         # the real one is 8 bytes of nothing, ref 126
SILENCE_REF   = 0x7E

# --- how a U-110 drum tone actually maps keys -----------------------------------------
#
# A tone whose type byte (record +0x0A) has bit 7 set is a DRUM tone, and the firmware
# does NOT compare the MIDI note against the tone's split points.  It looks the note up in
# a 128-byte table first (routine 0x7BFE, ROM-ANALYSIS.md section 6.6):
#
#     mapped = card[ start_of( TABLE[(type >> 4) & 7] ) + note ]
#
# and everything downstream -- which zone, hence which sample, and the pitch -- uses
# `mapped` in place of the note.  0x80 means the key is unassigned and stays silent, which
# is why a factory drum tone speaks on so few keys.
#
# TABLE is the tone record at logical 0x3760, and its twelve partial-1 sample indices are
# eight usable entries: one drum map per tone type 0x80, 0x90 ... 0xF0.  Each names a
# sample whose 128 bytes are the map rather than audio.  SN-U110-10 defines three maps,
# SN-U110-02 and waverom0 eight each.
#
# The mapped value is a note number, so the natural-rate pitch for a kit piece is
# `mapped == the sample's reference note`, and that is what the factory cards do: 17 of
# SN-U110-10's 35 assigned keys sit exactly there.  The deviations are deliberate tuning --
# sample 12 appears at mapped values 50, 55 and 60 on three keys, which is a three-tom set
# at -5, 0 and +5 semitones, and the kick appears at 5, 18, 20 and 23.  This converter
# writes `mapped == reference note` for every key, i.e. every drum at its own natural
# pitch, and `--spread` adds the R-8's own tuning on top.
MAP_BYTES    = 128      # the map is exactly 128 bytes, one per MIDI note
MAP_UNUSED   = 0x80     # "this key is not assigned"
MAP_REF      = 72       # the reference note the factory maps carry; never played as audio
TONE_TYPE    = 0x80     # bit 7 = drum; bits 4-6 pick the TABLE entry
MAX_DRUM_MAPS = 8       # bits 4-6, so eight tones per card at most
# Worst case, one 0x40000 boundary wastes almost a whole sample's length; 512 KB
# holds one such boundary above DATA_BASE, so one sample's slack is enough.


# The reference notes SN-U110-10 "ROCK DRUMS" gives its 23 samples, in tone order:
# eleven for partial 1, the silence, eleven for partial 2.
#
# These are used verbatim rather than computed, and that is a deliberate and unsatisfying
# choice.  Setting the reference note to the key each sample sits on -- the obvious thing,
# and what a melodic multisample wants -- makes the firmware refuse to start a voice on
# SOME keys.  Which keys depends on the (sample, reference note) PAIR and not on the value:
# reference note 48 works on one sample and is silent on another, and a constant 60 across
# the board fails on exactly the same keys as key-tracking does.  Every other field was
# cleared by bisection against a working card (start, length, loop mode, loop length, byte
# 7, byte 9, the tone record, the card ID); byte 8 is the one that breaks it.  Reusing a
# real drum tone's values sidesteps a constraint that is not yet understood, and costs
# nothing here: a drum map is not played as a melody, so absolute tuning of a kit piece is
# not meaningful.  R8-CONVERSION.md section 6 records the open question.
ROCK_DRUMS_REFS = [18, 35, 41, 42, 43, 44, 45, 46, 47, 48, 48,
                   SILENCE_REF,
                   55, 65, 70, 84, 96, 103, 106, 105, 106, 109, 108]


def r8_blocks(card, min_layer_bytes, stub_repeats):
    """Which R-8 sample blocks to carry over, as (tone, A, B or None, why not) per instrument.

    Block A is always real -- the A blocks tile the card.  Block B is real for most tones
    and a placeholder for the rest, and WHICH is not settled (R8-CONVERSION.md section
    2.3): the two candidate flag rules each agree with the other only ~85% of the time.  So
    this decides from geometry, which is checkable, on two tests:

      * OVERLAP.  Real samples tile -- a tone's B block starts exactly where its A block
        ends.  A block claiming bytes some other block already owns is not a sample.
      * REPEATED LENGTH.  The placeholders are identical stubs reused across tones: 688
        bytes eight times on SN-R8-10, 152 bytes three times on SN-R8-11.  A length that
        recurs that often is a stub signature; a length that occurs once is a sample.

    The second test replaced a plain minimum length, which was wrong: 909_K's B block is
    852 bytes and is the kick drum's entire low end, so any floor high enough to catch the
    688-byte stubs also threw away real audio.  `min_layer_bytes` survives only as a floor
    against degenerate spans.
    """
    taken = []
    exact = set()          # spans already accepted, so a REPEAT is sharing, not overlapping
    def overlaps(s, e):
        if (s, e) in exact:
            return False
        return any(max(0, min(e, y) - max(s, x)) > 0 for x, y in taken)

    for t in card['tones']:
        A = t['blocks'][0]
        if 0 < A['end'] - A['start'] <= rc.BANK_SIZE:
            taken.append((A['start'], A['end']))
            exact.add((A['start'], A['end']))

    lengths = {}
    for t in card['tones']:
        n = t['blocks'][1]['end'] - t['blocks'][1]['start']
        lengths[n] = lengths.get(n, 0) + 1

    out = []
    for t in card['tones']:
        A, B = t['blocks']
        if not (0 < A['end'] - A['start'] <= rc.BANK_SIZE):
            out.append((t, None, None, 'block A unreadable'))
            continue
        n = B['end'] - B['start']
        why = None
        if not (0 < n <= rc.BANK_SIZE):        why = 'B address out of range'
        elif n < min_layer_bytes:              why = 'B is only %d bytes' % n
        elif overlaps(B['start'], B['end']):   why = 'B overlaps another sample'
        elif lengths[n] >= stub_repeats:
            why = 'B is %d bytes, a length reused by %d tones -- a stub' % (n, lengths[n])
        keep = None
        if why is None:
            keep = B
            taken.append((B['start'], B['end']))
            exact.add((B['start'], B['end']))
        out.append((t, A, keep, why))
    return out


MAX_STEP = 1984         # the largest delta the 8-bit float can represent
MAX_AMP  = 1984         # and the largest level


def premix(insts, fade_ms):
    """Sum each instrument's two sample blocks into one waveform, phase 2 of section 5.

    The R-8 sounds block A and block B together -- a kick's click and its boom, a snare's
    noise and its shell tone -- so carrying only A, as phase 1 does, loses half of every
    two-layer sound.  Summing them here puts both back on one key, at the cost of a fixed
    balance between the layers (the R-8's own per-block levels are in w6..w8 and are not
    decoded).

    Returns (waveforms, scale).  A SINGLE scale is applied to every instrument on the card,
    never a per-instrument one: per-instrument normalising would maximise each hit but
    silently rewrite the kit's internal balance, and the U-110 sample record has no level
    field to put that back (byte 9 is the fine tune).  The scale is the smaller of what the
    LEVEL allows and what the SLEW allows -- two summed samples can ask for a step larger
    than the format's biggest, which is what makes a summed hi-hat saturate -- and comes out
    near 0.5, i.e. about 6 dB below a factory card.
    """
    # A short fade to true silence at the end of every sample.
    #
    # When a one-shot reaches its end the firmware slams the envelope to zero at the
    # MAXIMUM rate -- traced: register 06 = 0x80 (the most negative ramp) with register 07 =
    # 0x00 -- which closes it in about 0.85 ms.  Whatever the waveform is doing at that
    # instant is cut off square, and the U-110's de-emphasis (a one-pole at 20 Hz with 182x
    # makeup, roland_u110.cpp) turns the resulting low-frequency step into a thump several
    # times louder than the decaying tail it interrupts.
    #
    # Factory U-110 samples do not do this because they were authored to reach silence by
    # their last byte.  R-8 samples were not: the R-8 closes its own envelope over the top.
    # Measured on SN-R8-11, the last 3 ms of a rendered CLSD_H2 came out 3.7x the body level
    # with the bytes copied verbatim and 10.4x pre-mixed -- so this is inherited from the
    # source, not caused by the re-encoding.
    #
    # Fading the tail ourselves costs a few milliseconds of decay that was inaudible anyway
    # and leaves the envelope slam nothing to cut.  It also guarantees the waveform ends at
    # exactly zero, which a few R-8 samples otherwise do not (OPEN_H2 ends at +88).
    fade_n = max(0, int(round(fade_ms * 1e-3 * rc.ENGINE_RATE)))

    waves = []
    for (t, A, B, _why) in insts:
        img = t['img']
        a = rc.integrate(img, A['start'], A['end'])
        b = (rc.integrate(img, B['start'], B['end']) if B is not None else np.zeros(0))
        n = max(len(a), len(b))
        m = np.zeros(n)
        m[:len(a)] += a
        m[:len(b)] += b
        if fade_n and m.size > 4:
            k = min(fade_n, m.size)
            # raised cosine: starts at unity slope zero, so it does not put a corner of its
            # own where it begins
            m[-k:] *= 0.5 * (1.0 + np.cos(np.pi * np.arange(k) / max(k - 1, 1)))
            m[-1] = 0.0
        waves.append(m)
    scale = 1.0
    for m in waves:
        if m.size < 2:
            continue
        scale = min(scale, MAX_AMP / max(np.abs(m).max(), 1e-9))
        d = np.diff(np.concatenate(([0.0], m)))
        scale = min(scale, MAX_STEP / max(np.abs(d).max(), 1e-9))
    return [m * scale for m in waves], scale


def build(insts_in, args):
    """Author the logical image, then scramble it and drop the plain header on top.

    The layout copies SN-U110-10 "ROCK DRUMS" exactly, because that is the one arrangement
    the firmware is known to play (R8-CONVERSION.md section 3.2):

      * the two partials are a KEY SPLIT, not a layer -- partial 1 takes the lower keys and
        partial 2 the upper, each muting itself over the other's range with a silent sample;
      * the sample index runs are CONSECUTIVE -- 0..11 in partial 1 and 11..22 in partial 2,
        with index 11 the silence they share;
      * partial 2's first split equals partial 1's last, which is where the two meet.

    22 keys per tone, one R-8 sample block each.  Phase 1 carries block A only: the earlier
    plan put A on partial 1 and B on partial 2 with identical splits so the two would sound
    together, and the machine will not play that -- see section 6.  Layering waits for the
    phase 2 pre-mix, which needs only one sample per key.
    """
    logical = np.full(rc.BANK_SIZE, 0xFF, dtype=np.uint8)

    # The SAMPLE DATA area is zero-filled, not 0xFF.  0xFF is not blank here: the stored
    # byte is a DELTA and 0xFF decodes to -1, so a stretch of it is a downward ramp rather
    # than silence.  0x00 and 0x80 both decode to zero and real cards pad with 0x80.
    logical[DATA_BASE:] = 0x00

    insts = list(insts_in)
    if args.max_instruments:
        insts = insts[:args.max_instruments]

    mixed, mix_scale = (premix(insts, args.fade_ms) if args.premix else (None, 1.0))

    # A 512K R-8 card can hold more sample data than a 512K U-110 card has room for, because
    # the U-110 reserves the first 16K for its tables and we spend a byte per sample on the
    # interpolation point.  SN-R8-09 needs 550K of the 496K available.  Drop from the end
    # rather than refusing: 22 instruments is a full tone either way, and the caller is told
    # exactly what went.
    room = DATA_END - DATA_BASE
    dropped_for_room = []
    while insts:
        per_partial_ = min(args.keys_per_partial, MAX_KEYS)
        ntone = (len(insts) + 2 * per_partial_ - 1) // (2 * per_partial_)
        uniq = {}
        for k, (t, A, B, _w) in enumerate(insts):
            uniq[(id(t['img']), A['start'], A['end'],
                  B['start'] if B is not None else None,
                  B['end'] if B is not None else None)] = \
                (mixed[k].size if mixed is not None else A['end'] - A['start'])
        need = sum(v + 1 for v in uniq.values())
        need += ntone * (SILENCE_BYTES + 1 + MAP_BYTES + 1)   # a silence and a map per tone
        need += max(uniq.values(), default=0)                 # boundary padding, see _place
        if need <= room:
            break
        dropped_for_room.append(insts.pop()[0]['name'])
        if mixed is not None:
            mixed.pop()
    if dropped_for_room:
        print("  no room for %d instrument(s), dropped: %s"
              % (len(dropped_for_room), ' '.join(reversed(dropped_for_room))), file=sys.stderr)

    # Instruments that resolve to identical bytes are stored ONCE.  R-8 cards carry the same
    # drum under several names -- ATAK_T1/ATAK_T2 and HOLO_T1/HOLO_T2 on SN-R8-09, and
    # CRSH_C6/CRSH_C7/SPLA_C3 on SN-R8-11 -- differing only in the envelope words, which are
    # not decoded (R8-DRUM-TONES.md section 4).  Sharing the bytes buys the space back and,
    # more usefully, lets the copies be PITCHED apart instead of sounding identical: the
    # group takes one reference note and each key's mapped value sits a few semitones off
    # it, which is exactly how a factory tom set is built.
    payload_cache = {}

    def _payload(n, t, A, B):
        """(bytes, length, group key) for instrument n."""
        gkey = (id(t['img']), A['start'], A['end'],
                B['start'] if B is not None else None,
                B['end'] if B is not None else None)
        if gkey in payload_cache:
            return payload_cache[gkey] + (gkey,)
        if mixed is None:
            out = (None, A['end'] - A['start'])
        else:
            code, _ = rc.encode_float8_delta(mixed[n])
            out = (code, code.size)
        payload_cache[gkey] = out
        return out + (gkey,)

    per_partial = min(args.keys_per_partial, MAX_KEYS)
    per_tone = 2 * per_partial

    # The MAPPED values, which are what the tone's splits are compared against and what
    # sets the pitch -- not the MIDI keys.  Partial 1 takes a run of `per_partial`, then a
    # gap of one so partial 2's first split can sit in it, then partial 2's run.  They only
    # have to ASCEND, so a repeated sample can be spaced further along and sound higher.
    def _mapped(slot):
        return args.map_base + (slot if slot < per_partial else slot + 1)

    # --- one tone at a time, each with its own sample records and its own drum map
    samples = []            # dicts: data / src, reference note, length
    group_ref = {}          # payload -> the reference note its first key set
    tones = []
    ntones = (len(insts) + per_tone - 1) // per_tone
    if ntones > MAX_DRUM_MAPS:
        print("  only %d drum maps exist (tone type bits 4-6); keeping the first %d tones"
              % (MAX_DRUM_MAPS, MAX_DRUM_MAPS), file=sys.stderr)
        ntones = MAX_DRUM_MAPS
        insts = insts[:ntones * per_tone]
    for ti in range(ntones):
        group = insts[ti * per_tone:(ti + 1) * per_tone]
        first = len(samples)
        lo, hi = group[:per_partial], group[per_partial:]
        keys = []
        for n, (t, A, B, _w) in enumerate(lo):
            key = args.base_note + n
            data, length, gkey = _payload(ti * per_tone + n, t, A, B)
            mv = _mapped(n)
            # Reference note == mapped value, so a drum plays at its own stored rate -- the
            # convention the factory cards use.  A repeat of an earlier sample keeps THAT
            # sample's reference note instead, so its own mapped value pitches it up.
            ref = (mv - t.get('pitch', 0)) if t.get('pitch') \
                  else group_ref.setdefault(gkey, mv)
            keys.append((t, key, mv, mv - ref))
            samples.append(dict(src=(t['img'], A['start'], A['end']), data=data, group=gkey,
                                ref=ref, length=length))
        while len(samples) - first < per_partial:      # pad so the index runs stay uniform
            samples.append(dict(src=None, data=None, ref=SILENCE_REF, length=SILENCE_BYTES))
        silence = len(samples)
        samples.append(dict(src=None, data=None, ref=SILENCE_REF, length=SILENCE_BYTES))
        for n, (t, A, B, _w) in enumerate(hi):
            key = args.base_note + per_partial + n
            data, length, gkey = _payload(ti * per_tone + per_partial + n, t, A, B)
            mv = _mapped(per_partial + n)
            ref = (mv - t.get('pitch', 0)) if t.get('pitch') \
                  else group_ref.setdefault(gkey, mv)
            keys.append((t, key, mv, mv - ref))
            samples.append(dict(src=(t['img'], A['start'], A['end']), data=data, group=gkey,
                                ref=ref, length=length))
        while len(samples) - silence - 1 < per_partial:
            samples.append(dict(src=None, data=None, ref=SILENCE_REF, length=SILENCE_BYTES))

        # The drum map: 128 bytes, one per MIDI note, holding the mapped value for the keys
        # this tone assigns and MAP_UNUSED everywhere else.  It is stored as a sample and
        # read as data, never played (routine 0x7BFE).
        kmap = bytearray([MAP_UNUSED]) * MAP_BYTES
        for (_t, key, mv, _d) in keys:
            if 0 <= key < MAP_BYTES:
                kmap[key] = mv & 0x7F
        mapidx = len(samples)
        samples.append(dict(src=None, data=np.frombuffer(bytes(kmap), dtype=np.uint8),
                            ref=MAP_REF, length=MAP_BYTES))
        tones.append(dict(index=ti, first=first, silence=silence, keys=keys, map=mapidx))

    if len(samples) > SAMPLE_RECORDS:
        raise SystemExit("needs %d sample records, the table holds %d"
                         % (len(samples), SAMPLE_RECORDS))
    # Count each payload once: repeats share their bytes (see payload_cache).
    _seen, need = set(), 0
    for s in samples:
        g = s.get('group')
        if g is not None:
            if g in _seen:
                continue
            _seen.add(g)
        need += s['length'] + 1
    need += max((s['length'] for s in samples), default=0)     # 0x40000 boundary slack
    if DATA_BASE + need > DATA_END:
        raise SystemExit("samples need %d bytes, the card has %d free above %#x -- "
                         "use --max-instruments to drop some"
                         % (need, DATA_END - DATA_BASE, DATA_BASE))

    # --- sample data.  One byte past each sample is copied too: the chip fetches it to
    # interpolate against, which is the "+1" the third-party card decodes note.
    # A sample may not cross a 0x40000 boundary.  The chip's phase accumulator supplies
    # only address bits 0-17; bit 18 comes from the bank register, which is written once at
    # voice start (ROM-ANALYSIS.md section 6.2), so a sample straddling the boundary wraps
    # back on itself instead of carrying.  Not a guess: ZERO of the 1172 samples across all
    # fifteen factory SN-U110 cards cross one.  Ours did, once -- a 53 KB reverb clap at
    # 0x33632 -- and it played as a runaway ramp 10x too loud.
    BANK_SPAN = 1 << 18

    def _place(at, n):
        if (at >> 18) != ((at + n) >> 18):
            at = ((at >> 18) + 1) << 18
        return at

    at = DATA_BASE
    placed = {}                            # payload -> where its bytes went, so repeats share
    for s in samples:
        g = s.get('group')
        if g is not None and g in placed:
            s['start'] = placed[g]
            continue
        at = _place(at, s['length'])       # the +1 interpolation byte may share the next bank
        s['start'] = at
        if g is not None:
            placed[g] = at
        if s.get('data') is not None:
            # Generated bytes: one extra zero-delta byte so the chip's interpolation fetch
            # past the end lands on the same value rather than stepping.
            logical[at:at + s['length']] = s['data']
            logical[at + s['length']] = 0x00
        elif s['src'] is not None:
            img, a, b = s['src']
            logical[at:at + s['length'] + 1] = img[a:b + 1]
        at += s['length'] + 1

    for i, s in enumerate(samples):
        o = SAMPLE_TABLE + 10 * i
        # byte 2: start bits 16-18, then bit 3 = "this is a card" (the log's segment 1)
        b2 = ((s['start'] >> 16) & 0x07) | (1 << 3) | (args.loopmode << 6)
        last = s['length'] - 1
        logical[o + 0] = s['start'] & 0xFF
        logical[o + 1] = (s['start'] >> 8) & 0xFF
        logical[o + 2] = b2
        logical[o + 3] = last & 0xFF
        logical[o + 4] = (last >> 8) & 0xFF
        logical[o + 5] = args.looplen & 0xFF
        logical[o + 6] = (args.looplen >> 8) & 0xFF
        logical[o + 7] = args.byte7
        logical[o + 8] = s['ref']
        logical[o + 9] = args.fine
    if len(samples) < SAMPLE_RECORDS:                       # terminate the table
        o = SAMPLE_TABLE + 10 * len(samples)
        logical[o:o + 3] = 0xFF

    # --- tone records
    if len(tones) > TONE_RECORDS:
        raise SystemExit("needs %d tone records, the table holds %d"
                         % (len(tones), TONE_RECORDS))
    for T in tones:
        # The splits are compared against the MAPPED value, not the key.  Partial 1's run
        # is map_base .. map_base+10; partial 2's first split sits in the one-value gap
        # after it, so a mapped value at or below that lands on partial 2's silence.
        splits1 = [_mapped(k) for k in range(per_partial)]
        splits2 = [_mapped(per_partial - 1) + 1] + [_mapped(per_partial + k)
                                                    for k in range(per_partial - 1)]
        idx1 = list(range(T['first'], T['first'] + per_partial)) + [T['silence']]
        idx2 = [T['silence']] + list(range(T['silence'] + 1, T['silence'] + 1 + per_partial))
        name = args.tone_name % dict(part=args.part_for_name, n=T['index'] + 1)
        rec = bytearray(0x50)
        rec[0:10] = ("%-10.10s" % name).encode('ascii', 'replace')
        # bit 7 marks a drum tone; bits 4-6 pick which of the eight drum maps it uses, so
        # each tone on the card gets its own -- exactly as ROCK DRUMS (0x80) and ELEC DRUMS
        # (0x90) do on SN-U110-10.
        rec[0x0A:0x10] = bytes([TONE_TYPE | (T['index'] << 4)]) + ROCK_DRUMS_HEADER[1:]
        for base, splits, idx, params in ((0x10, splits1, idx1, ROCK_DRUMS_P1),
                                          (0x30, splits2, idx2, ROCK_DRUMS_P2)):
            rec[base:base + 11] = bytes(x & 0x7F for x in splits)
            rec[base + 11:base + 23] = bytes(x & 0xFF for x in idx)
            rec[base + 23:base + 32] = params
        logical[TONE_TABLE + TONE_STRIDE * T['index']:
                TONE_TABLE + TONE_STRIDE * (T['index'] + 1)] = \
            np.frombuffer(bytes(rec), dtype=np.uint8)

    # The firmware finds the end of the tone list by scanning for an all-spaces name
    # (ROM-ANALYSIS.md section 6.6).  Blank ALL the unused records rather than only the
    # next: the table is reserved either way, real cards do exactly this (SN-U110-10
    # declares two tones and carries 99 records), and it leaves nothing to mistake.
    for ti in range(len(tones), TONE_RECORDS):
        rec = bytearray(b'\xFF' * 0x50)
        rec[0:10] = b' ' * 10
        rec[0x0A:0x10] = BLANK_TONE_HEADER
        logical[TONE_TABLE + TONE_STRIDE * ti:
                TONE_TABLE + TONE_STRIDE * (ti + 1)] = np.frombuffer(bytes(rec), dtype=np.uint8)

    # --- the TABLE record a drum tone needs (see TABLE_NAME above).  Its zones point at
    # the first tone's silence, so it satisfies the firmware and makes no sound of its own.
    tbl = bytearray(0x50)
    tbl[0:10] = TABLE_NAME
    tbl[0x0A:0x10] = TABLE_HEADER
    tbl[0x10:0x1B] = TABLE_SPLITS
    # entry k is read by a tone whose type byte is 0x80 | (k << 4), so entry k names tone
    # k's own drum map.  Spare entries point at the last map rather than 0xFF, which the
    # firmware would read as an unusable sample index.
    entries = [tones[min(k, len(tones) - 1)]['map'] for k in range(MAX_DRUM_MAPS)]
    tbl[0x1B:0x27] = bytes(entries + [0xFF] * 4)
    tbl[0x27:0x30] = TABLE_P1PAR
    tbl[0x30:0x3B] = b'\xFF' * 11
    tbl[0x3B:0x47] = b'\xFF' * 12
    tbl[0x47:0x50] = TABLE_P2PAR
    at_tbl = TONE_TABLE + TONE_STRIDE * TABLE_RECORD
    logical[at_tbl:at_tbl + 0x50] = np.frombuffer(bytes(tbl), dtype=np.uint8)

    # --- scramble, then write the plain 48-byte ID header over physical 0x00..0x2F.
    # Physical 0..0x2F maps onto the logical lattice {0x00-0x05, 0x10-0x15 ... 0x70-0x75},
    # all below 0x76, and we left logical 0x00-0xFF alone -- so nothing is lost.
    phys = bytearray(rc.scramble(logical).tobytes())
    phys[0x00:0x10] = rc.U110_MAGIC
    phys[0x10:0x20] = ("%-16.16s" % args.label).encode('ascii', 'replace')
    phys[0x20] = args.card_id
    phys[0x21:0x30] = b'\xFF' * 0x0F
    return bytes(phys), samples, tones, insts


def describe(path, base_note):
    """Re-read a card this tool wrote, through the same primitives that read a real one."""
    logical, phys = rc.load_bank(path)
    kind = rc.identify(phys)
    print("%s\n  magic %-6s  label %r  card id %d (0x%02X)"
          % (path, kind, bytes(phys[0x10:0x20]).decode('latin1').rstrip(), phys[0x20], phys[0x20]))
    if kind != 'u110':
        print("  the firmware will REJECT this: the 16-byte magic is not the U-110's")
    smp = rc.u110_samples(logical)
    live = []
    for t in rc.u110_tones(logical, TONE_RECORDS):
        # Stop where the firmware stops: the first record whose name is blank.  Anything
        # unprintable ends the list too -- that is not a name the machine would show.
        if t['name'].strip() == '' or any(c < ' ' or c > '~' for c in t['name']):
            break
        live.append(t)
    print("  %d sample records, %d tones" % (len(smp), len(live)))
    for t in live:
        keys = [s for s in t['partials'][0]['splits'] if s != 0xFF]
        if not keys:
            print("  tone %d %r  has no key splits" % (t['index'], t['name']))
            continue
        print("  tone %d %r  header %s" % (t['index'], t['name'], t['header'].hex(' ')))
        lo, hi = min(keys), max(keys)
        for n in range(lo, hi + 1):
            a = rc.u110_pick(t['partials'][0], n)
            b = rc.u110_pick(t['partials'][1], n)
            ra = smp.get(a, {}); rb = smp.get(b, {})
            print("    note %3d -> P1 s%-3d ref %3d %6d B%s   P2 s%-3d ref %3d %6d B%s"
                  % (n, a, ra.get('ref', -1), ra.get('length', 0),
                     ' (silence)' if ra.get('length') == SILENCE_BYTES else '',
                     b, rb.get('ref', -1), rb.get('length', 0),
                     ' (silence)' if rb.get('length') == SILENCE_BYTES else ''))
        print("    note %3d -> P1 s%-3d   P2 s%-3d   (above the map)"
              % (hi + 1, rc.u110_pick(t['partials'][0], hi + 1),
                 rc.u110_pick(t['partials'][1], hi + 1)))
    return 0



def load_r8(paths, min_layer_bytes, stub_repeats):
    """Every usable instrument on every card given, in card then catalogue order.

    Each instrument keeps a reference to the image it came from, so one U-110 card can be
    built from several R-8 cards.  The block-A/block-B geometry tests (section 2.3) are
    per-card, because they reason about how a card tiles its own sample area.
    """
    out = []
    for path in paths:
        logical, phys = rc.load_bank(path)
        if rc.identify(phys) != 'r8':
            raise SystemExit("%s: not an SN-R8 card (magic says %r)"
                             % (path, rc.identify(phys)))
        card = rc.r8_card(logical, phys)
        for (t, A, B, why) in r8_blocks(card, min_layer_bytes, stub_repeats):
            if A is None:
                continue
            t['img'] = logical
            t['part'] = card['part']
            t['pitch'] = 0
            out.append((t, A, B, why))
    return out


def parse_pick(spec):
    """One kit entry: `[card:]name[+n|-n]`.

    `909_K`            the first instrument of that name on any card given
    `10:909_K`         from SN-R8-10 specifically
    `909_T+5`          five semitones up -- the same sample can be listed more than once at
                       different offsets, which is how a tom set is built out of one drum
    """
    spec = spec.strip()
    pitch = 0
    m = re.search(r'([+-]\d+)$', spec)
    if m:
        pitch = int(m.group(1))
        spec = spec[:m.start()].strip()
    part = None
    if ':' in spec:
        p, spec = spec.split(':', 1)
        part = int(p)
    return part, spec.strip().upper(), pitch


def select(insts, picks):
    """Resolve kit entries against the loaded instruments, in the order asked for."""
    chosen, missing = [], []
    for spec in picks:
        part, name, pitch = parse_pick(spec)
        hit = None
        for (t, A, B, why) in insts:
            if t['name'].strip().upper() != name:
                continue
            if part is not None and t['part'] != part:
                continue
            hit = (t, A, B, why)
            break
        if hit is None:
            missing.append(spec)
            continue
        # A repeat at a different pitch needs its own tone dict, or the two entries would
        # share one `pitch` field; the heavy sample data is shared by the payload cache.
        t = dict(hit[0])
        t['pitch'] = pitch
        chosen.append((t, hit[1], hit[2], hit[3]))
    if missing:
        raise SystemExit("no such instrument: " + ', '.join(missing)
                         + "\n(use --list to see what the given cards offer)")
    return chosen


def card_number_from_name(name):
    """The plugin's own rule, from Voltaire110Plugin.cpp cardNumberFromName().

    Separators are thrown away, then "snu110" followed by exactly two digits names the
    card.  Kept in step with that function deliberately: a card image the scan cannot name
    is invisible however good it is.
    """
    flat = ''.join(c.lower() for c in name if c not in '-_ .')
    tag, at = 'snu110', 0
    while True:
        at = flat.find(tag, at)
        if at < 0:
            return None
        d = at + len(tag)
        if (d + 1 < len(flat) and flat[d].isdigit() and flat[d + 1].isdigit()
                and not (d + 2 < len(flat) and flat[d + 2].isdigit())):
            return int(flat[d:d + 2])
        at += 1


def read_kit(path):
    """A kit file: one entry per line, `#` starts a comment, blank lines ignored."""
    out = []
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if line:
            out.extend(x for x in re.split(r'[,\s]+', line) if x)
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Build a U-110 PCM card out of Roland SN-R8 cards",
        epilog="With no --pick or --kit the first card's instruments are used in order, "
               "which is the whole-card conversion.  Give several cards and a kit to "
               "assemble your own: r8_to_u110.py *.bin --kit mykit.txt -o card.bin")
    ap.add_argument('card', nargs='+',
                    help='one or more SN-R8 card dumps, or one converted card with --verify')
    ap.add_argument('--list', action='store_true',
                    help='print the instruments the given cards offer, and stop')
    ap.add_argument('--pick', default=None,
                    help='kit entries, comma separated: "909_K,10:78_S,909_T+5"')
    ap.add_argument('--kit', default=None,
                    help='a file of kit entries, one per line, # for comments')
    ap.add_argument('-o', '--out', help='where to write the U-110 card image')
    ap.add_argument('--verify', action='store_true', help='read the named card back and stop')
    ap.add_argument('--card-id', type=int, default=None,
                    help='U-110 catalogue ID.  The part record holds 5 bits so 1..31 are '
                         'addressable while the real catalogue stops around 15; the default '
                         'is 16 + the R-8 part number, to stay clear of real cards')
    ap.add_argument('--label', default=None, help='16-byte card label (default names the R-8 card)')
    ap.add_argument('--tone-name', default='R8-%(part)02d SET%(n)d',
                    help='10-char tone name template')
    ap.add_argument('--base-note', type=int, default=36, help='MIDI note of the first key (36 = C2)')
    ap.add_argument('--map-base', type=int, default=36,
                    help='first MAPPED value; each sample\'s reference note is set to its '
                         'own mapped value, so every drum plays at its stored rate')
    ap.add_argument('--keys-per-partial', type=int, default=MAX_KEYS,
                    help='keys on each partial; 11 is the most a partial can hold')
    ap.add_argument('--max-instruments', type=int, default=0, help='0 = all that fit')
    ap.add_argument('--fade-ms', type=float, default=6.0,
                    help='fade the last N ms of every sample to silence (default 6).  The '
                         'firmware cuts a finished one-shot with a 0.85 ms envelope slam, '
                         'and R-8 samples still have tail left when it happens; 0 disables')
    ap.add_argument('--premix', dest='premix', action='store_true', default=True,
                    help='sum each R-8 tone\'s two sample blocks into one key (default)')
    ap.add_argument('--no-premix', dest='premix', action='store_false',
                    help='carry block A only, as phase 1 did')
    ap.add_argument('--min-layer-bytes', type=int, default=64,
                    help='floor against degenerate block-B spans; the stub test is the real one')
    ap.add_argument('--stub-repeats', type=int, default=3,
                    help='a block-B length shared by this many tones is a placeholder')
    ap.add_argument('--loopmode', type=int, default=1, help='1 = off, which is every R-8 sample')
    ap.add_argument('--looplen', type=int, default=4)
    ap.add_argument('--fine', type=int, default=0x40,
                    help='sample record byte 9 -- the FINE TUNE, measured (0x40 = centre)')
    ap.add_argument('--byte7', type=int, default=0x40,
                    help='sample record byte 7; 0x40 on every real card, and it has no '
                         'effect on pitch that could be measured')
    a = ap.parse_args()

    if a.verify:
        return describe(a.card[0], a.base_note)

    insts = load_r8(a.card, a.min_layer_bytes, a.stub_repeats)
    if a.list:
        print("%d instruments on %d card(s):" % (len(insts), len(a.card)))
        part = None
        for (t, A, B, _w) in insts:
            if t['part'] != part:
                part = t['part']
                print("  SN-R8-%02d" % part)
            print("    %-9s %6d bytes%s" % (t['name'], A['end'] - A['start'],
                                            '  + layer B' if B is not None else ''))
        return 0

    if not a.out:
        ap.error("-o/--out is required unless --verify or --list")

    picks = read_kit(a.kit) if a.kit else (
            [x for x in re.split(r'[,\s]+', a.pick) if x] if a.pick else None)
    if picks:
        insts = select(insts, picks)
    else:
        first = insts[0][0]['part'] if insts else 0
        insts = [i for i in insts if i[0]['part'] == first]
    if not insts:
        raise SystemExit("nothing to put on the card")

    parts = sorted({t['part'] for (t, _A, _B, _w) in insts})
    a.part_for_name = parts[0]
    if a.card_id is None:
        a.card_id = 16 + parts[0]
    if not 1 <= a.card_id <= 31:
        print("warning: card id %d is outside the 5 bits a patch part can name (1..31); "
              "the machine will mount it but no patch can select it" % a.card_id,
              file=sys.stderr)
    if a.label is None:
        a.label = ("SN-R8-%02d   0.01" % parts[0] if len(parts) == 1
                   else "R8 KIT     0.01")

    img, samples, tones, insts = build(insts, a)
    open(a.out, 'wb').write(img)

    # The plugin finds cards by NAME, not by content: Voltaire110Plugin.cpp's
    # cardNumberFromName() strips - _ space and . and then looks for "snu110" followed by
    # exactly two digits.  A file it cannot parse is never offered in the card list, however
    # valid the image is -- so say so here rather than let it look like a silent failure or,
    # worse, like an older card at the same slot is the new one.
    if card_number_from_name(os.path.basename(a.out)) is None:
        print("warning: %r has no `sn-u110-NN` in it, so the plugin's card scan will not\n"
              "         offer it.  Rename it, e.g. sn-u110-%02d_%s"
              % (os.path.basename(a.out), a.card_id,
                 os.path.basename(a.out).lstrip('0123456789-_ ')), file=sys.stderr)

    print("%s  ->  %s"
          % (' + '.join('SN-R8-%02d' % p for p in parts), a.out))
    print("  card id %d (0x%02X), label %r" % (a.card_id, a.card_id, a.label))
    dropped = []
    live_b = sum(1 for (_t, _A, B, _w) in insts if B is not None)
    print("  %d instruments, %d sample records, %d tones"
          % (len(insts), len(samples), len(tones)))
    print("  %s" % ("blocks A and B summed onto one key for the %d instruments that have "
                    "both" % live_b if a.premix
                    else "block A only; %d instruments have a live block B, not carried"
                         % live_b))
    used = max((s['start'] + s['length'] + 1 for s in samples), default=DATA_BASE) - DATA_BASE
    print("  sample data %d bytes of %d free (%.1f%%)"
          % (used, DATA_END - DATA_BASE, 100.0 * used / (DATA_END - DATA_BASE)))
    for T in tones:
        keys = [k for (_t, k, _m, _d) in T['keys']]
        print("  tone %d (type %#04x, drum map = sample %d)  notes %d..%d"
              % (T['index'], TONE_TYPE | (T['index'] << 4), T['map'], min(keys), max(keys)))
        print("     " + ' '.join('%s@%d%s' % (t['name'], k, '' if d == 0 else '%+d' % d)
                                  for (t, k, _m, d) in T['keys']))
    if dropped:
        print("  block B not carried over:")
        for t, why in dropped:
            print("    %-8s %s" % (t['name'], why))
    return 0


if __name__ == '__main__':
    sys.exit(main())
