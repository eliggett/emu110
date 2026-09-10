#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""R-8 SPECIFIC.  Build a U-110 PCM card out of a Roland SN-R8 card's samples.

Phase 1 of analysis/R8-CONVERSION.md section 5, and deliberately the boring phase: NO
sample is re-encoded.  R-8 and U-110 PCM data are the same 8-bit float delta format on the
same wiring (section 1 of that document), so every sample byte is copied verbatim and the
only things this tool authors are the tables around them.  If the card mounts, it plays
what the R-8 played.

Each R-8 tone carries two sample blocks that the machine SUMS -- a snare's noise in one and
its shell tone in the other -- so each becomes one U-110 key with block A on partial 1 and
block B on partial 2, both partials given identical key splits so the two sound together.
That costs both partials per key, which caps a U-110 tone at 11 keys, so a 26-instrument
R-8 card becomes three U-110 tones.  Phase 2 lifts that to 22 by pre-mixing.

    python3 tools/r8_to_u110.py SN-R8-10_Dance.bin -o roms/sn-u110-26_r8-10_dance.bin
    python3 tools/r8_to_u110.py SN-R8-10_Dance.bin -o out.bin --card-id 26 --base-note 36
    python3 tools/r8_to_u110.py out.bin --verify          # re-read a card this made

The output filename matters: the plugin finds cards by looking for `snu110NN` in the name
with separators stripped (Voltaire110Plugin.cpp, cardNumberFromName), so keep the
`sn-u110-NN` in it and let the rest of the name say where it came from.
"""
import argparse, os, sys
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

# --- the "TABLE" record --------------------------------------------------------------
#
# A tone record at logical 0x3760 -- index 126, well past the 99 the machine can select --
# whose name field literally reads "TABLE".  A DRUM tone (header byte +0x0A = 0x80) will
# not sound without it: with 0xFF there the firmware refuses to start a voice on almost
# every key, and writing this record is what makes a converted card play.  Found by
# bisecting a working card against ours one byte range at a time; the whole difference came
# down to the sixteen bytes at 0x3770.
#
# It is present on exactly the images that carry drum or percussion tones -- SN-U110-02
# (Latin/FX Percussion), -08, -10 (Rock Drums) and internal waverom0, which holds tone 99
# "DRUMS" -- and absent from the twelve melodic cards.  The split points are the SAME on
# all four (31, 43, 49, 55, 63, 71, 75); only the sample indices differ per card.
#
# What the firmware DOES with it is not yet known.  Pointing its indices at real samples
# makes those samples sound on every key at a uniform level, drowning the selected tone;
# pointing them at a silent sample leaves the tone audible, which is what we want.  So this
# is written to satisfy the firmware and contribute nothing.  See R8-CONVERSION.md.
TABLE_NAME    = b'TABLE     '
TABLE_HEADER  = bytes([0x00, 0x00, 0x2A, 0x00, 0x00, 0x00])
TABLE_SPLITS  = bytes([0x1F, 0x2B, 0x31, 0x37, 0x3F, 0x47, 0x4B, 0xFF, 0xFF, 0xFF, 0xFF])
TABLE_P1PAR   = bytes([0x00] * 8 + [0x71])
TABLE_P2PAR   = bytes([0x7F] * 8 + [0x00])
TABLE_RECORD  = 126

SILENCE_BYTES = 8                         # the real one is 8 bytes of nothing, ref 126
SILENCE_REF   = 0x7E

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
    def overlaps(s, e):
        return any(max(0, min(e, y) - max(s, x)) > 0 for x, y in taken)

    for t in card['tones']:
        A = t['blocks'][0]
        if 0 < A['end'] - A['start'] <= rc.BANK_SIZE:
            taken.append((A['start'], A['end']))

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
        out.append((t, A, keep, why))
    return out


def build(card, args):
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

    insts = r8_blocks(card, args.min_layer_bytes, args.stub_repeats)
    insts = [i for i in insts if i[1] is not None]
    if args.max_instruments:
        insts = insts[:args.max_instruments]

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
        need = sum(i[1]['end'] - i[1]['start'] + 1 for i in insts)
        need += ntone * (SILENCE_BYTES + 1)              # one silence per tone
        if need <= room:
            break
        dropped_for_room.append(insts.pop()[0]['name'])
    if dropped_for_room:
        print("  no room for %d instrument(s), dropped: %s"
              % (len(dropped_for_room), ' '.join(reversed(dropped_for_room))), file=sys.stderr)

    per_partial = min(args.keys_per_partial, MAX_KEYS)
    per_tone = 2 * per_partial

    def _ref(slot):
        """The reference note for the slot-th sample of a tone.  See ROCK_DRUMS_REFS."""
        return ROCK_DRUMS_REFS[slot] if slot < len(ROCK_DRUMS_REFS) else SILENCE_REF

    # --- one tone at a time, each with its own sample records
    samples = []            # dicts: src span or None for silence, ref note, key
    tones = []
    for ti in range((len(insts) + per_tone - 1) // per_tone):
        group = insts[ti * per_tone:(ti + 1) * per_tone]
        first = len(samples)
        lo = group[:per_partial]
        hi = group[per_partial:]
        keys = []
        for n, (t, A, _B, _w) in enumerate(lo):
            key = args.base_note + n
            keys.append((t, key))
            samples.append(dict(src=(A['start'], A['end']), ref=_ref(n),
                                length=A['end'] - A['start']))
        # pad partial 1 out to a full run so the index arithmetic stays uniform
        while len(samples) - first < per_partial:
            samples.append(dict(src=None, ref=SILENCE_REF, length=SILENCE_BYTES))
        silence = len(samples)
        samples.append(dict(src=None, ref=SILENCE_REF, length=SILENCE_BYTES))
        for n, (t, A, _B, _w) in enumerate(hi):
            key = args.base_note + per_partial + 1 + n
            keys.append((t, key))
            samples.append(dict(src=(A['start'], A['end']), ref=_ref(per_partial + 1 + n),
                                length=A['end'] - A['start']))
        while len(samples) - silence - 1 < per_partial:
            samples.append(dict(src=None, ref=SILENCE_REF, length=SILENCE_BYTES))
        tones.append(dict(index=ti, first=first, silence=silence, keys=keys))

    if len(samples) > SAMPLE_RECORDS:
        raise SystemExit("needs %d sample records, the table holds %d"
                         % (len(samples), SAMPLE_RECORDS))
    need = sum(s['length'] + 1 for s in samples)
    if DATA_BASE + need > DATA_END:
        raise SystemExit("samples need %d bytes, the card has %d free above %#x -- "
                         "use --max-instruments to drop some"
                         % (need, DATA_END - DATA_BASE, DATA_BASE))

    # --- sample data.  One byte past each sample is copied too: the chip fetches it to
    # interpolate against, which is the "+1" the third-party card decodes note.
    at = DATA_BASE
    for s in samples:
        s['start'] = at
        if s['src'] is not None:
            a, b = s['src']
            logical[at:at + s['length'] + 1] = card['image'][a:b + 1]
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
        p1_lo = args.base_note
        splits1 = [p1_lo + k for k in range(per_partial)]
        splits2 = [splits1[-1] + k for k in range(per_partial)]
        idx1 = list(range(T['first'], T['first'] + per_partial)) + [T['silence']]
        idx2 = [T['silence']] + list(range(T['silence'] + 1, T['silence'] + 1 + per_partial))
        name = args.tone_name % dict(part=card['part'], n=T['index'] + 1)
        rec = bytearray(0x50)
        rec[0:10] = ("%-10.10s" % name).encode('ascii', 'replace')
        rec[0x0A:0x10] = ROCK_DRUMS_HEADER
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
    tbl[0x1B:0x27] = bytes([tones[0]['silence']] * 8 + [0xFF] * 4)
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


def main():
    ap = argparse.ArgumentParser(description="Convert a Roland SN-R8 card into a U-110 card")
    ap.add_argument('card', help='an SN-R8 card dump (.bin), or a converted card with --verify')
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
    ap.add_argument('--keys-per-partial', type=int, default=MAX_KEYS,
                    help='keys on each partial; 11 is the most a partial can hold')
    ap.add_argument('--max-instruments', type=int, default=0, help='0 = all that fit')
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
        return describe(a.card, a.base_note)
    if not a.out:
        ap.error("-o/--out is required unless --verify")

    logical, phys = rc.load_bank(a.card)
    if rc.identify(phys) != 'r8':
        print("%s: not an SN-R8 card (magic says %r)" % (a.card, rc.identify(phys)),
              file=sys.stderr)
        return 1
    card = rc.r8_card(logical, phys)
    card['image'] = logical
    if a.card_id is None:
        a.card_id = 16 + card['part']
    if not 1 <= a.card_id <= 31:
        print("warning: card id %d is outside the 5 bits a patch part can name (1..31); "
              "the machine will mount it but no patch can select it" % a.card_id,
              file=sys.stderr)
    if a.label is None:
        a.label = "SN-R8-%02d   0.01" % card['part']

    img, samples, tones, insts = build(card, a)
    open(a.out, 'wb').write(img)

    print("SN-R8-%02d %r  ->  %s" % (card['part'], card['label'], a.out))
    print("  card id %d (0x%02X), label %r" % (a.card_id, a.card_id, a.label))
    dropped = []
    print("  %d instruments, %d sample records, %d tones  (block A only in phase 1; %d "
          "instruments also carry a live block B, unused for now)"
          % (len(insts), len(samples), len(tones),
             sum(1 for (_t, _A, B, _w) in insts if B is not None)))
    used = sum(s['length'] + 1 for s in samples)
    print("  sample data %d bytes of %d free (%.1f%%)"
          % (used, DATA_END - DATA_BASE, 100.0 * used / (DATA_END - DATA_BASE)))
    for T in tones:
        keys = [k for (_t, k) in T['keys']]
        print("  tone %d  notes %d..%d: %s"
              % (T['index'], min(keys), max(keys),
                 ' '.join('%s@%d' % (t['name'], k) for (t, k) in T['keys'])))
    if dropped:
        print("  block B not carried over:")
        for t, why in dropped:
            print("    %-8s %s" % (t['name'], why))
    return 0


if __name__ == '__main__':
    sys.exit(main())
