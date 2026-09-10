#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode the samples on a Roland SN-R8 PCM card to WAVs, so they can be heard.

Each R-8 tone carries TWO sample blocks, and the R-8 sounds them together -- block A is
the attack/body and block B the low end (a 909 kick's click and its boom, a snare's snap
and its shell tone).  So three files come out per tone by default: the two layers on their
own and the sum, which is what the machine actually makes.

    python3 tools/r8_extract.py SN-R8-10_Dance.bin              # list the tones
    python3 tools/r8_extract.py SN-R8-10_Dance.bin --tone 0 4 13
    python3 tools/r8_extract.py SN-R8-10_Dance.bin --all -o listen/r8-decode/dance

No pitch shift, no envelope, no output filter -- the stored waveform at the engine's own
32 kHz, so the source material can be judged on its own terms.
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roland_card as rc


def layers(logical, tone):
    """(A, B) as float arrays.  Either may be empty if the card leaves that block unused."""
    out = []
    for b in tone['blocks']:
        out.append(rc.integrate(logical, b['start'], b['end']) if b['used'] else np.zeros(0))
    return out


def mix(a, b):
    """A + B, zero-padded to the longer.  Both layers are stored at full scale, so this
    sums them as the chip's mixer would with equal voice levels."""
    n = max(len(a), len(b))
    if n == 0:
        return np.zeros(0)
    out = np.zeros(n)
    out[:len(a)] += a
    out[:len(b)] += b
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('card', help='an SN-R8 card dump (.bin, as read off the chip)')
    ap.add_argument('--tone', type=int, nargs='*', help='tone indices to write')
    ap.add_argument('--all', action='store_true', help='write every tone')
    ap.add_argument('-o', '--outdir', default='listen/r8-decode')
    ap.add_argument('--layers', action='store_true', help='also write the A and B layers')
    a = ap.parse_args()

    logical, phys = rc.load_bank(a.card)
    kind = rc.identify(phys)
    if kind != 'r8':
        print("%s: not an SN-R8 card (magic says %r)" % (a.card, kind), file=sys.stderr)
        return 1
    card = rc.r8_card(logical, phys)
    print("SN-R8-%02d  id %#04x  %d tones  label %r"
          % (card['part'], card['card_id'], card['count'], card['label']))

    want = set(range(card['count'])) if a.all else set(a.tone or [])
    if not want:
        print("\n%3s %-9s %-11s  %9s %9s %8s  %9s %9s %8s"
              % ('#', 'name', 'header', 'A start', 'A end', 'A bytes', 'B start', 'B end', 'B bytes'))
        for t in card['tones']:
            A, B = t['blocks']
            print("%3d %-9s %-11s  %#9x %#9x %8d  %#9x %#9x %8d%s"
                  % (t['index'], t['name'], t['header'].hex(' '),
                     A['start'], A['end'], A['end'] - A['start'],
                     B['start'], B['end'], B['end'] - B['start'],
                     '' if B['used'] else '  (B unused)'))
        return 0

    for t in card['tones']:
        if t['index'] not in want:
            continue
        A, B = layers(logical, t)
        stem = "%s/%02d_%s" % (a.outdir.rstrip('/'), t['index'],
                               t['name'].replace('/', '-').replace(' ', '_') or 'unnamed')
        M = mix(A, B)
        if len(M) == 0:
            print("  %2d %-9s  nothing to decode" % (t['index'], t['name'])); continue
        rc.write_wav(stem + '.wav', M)
        made = ['%s.wav' % os.path.basename(stem)]
        if a.layers:
            if len(A): rc.write_wav(stem + '_A.wav', A); made.append('_A')
            if len(B): rc.write_wav(stem + '_B.wav', B); made.append('_B')
        print("  %2d %-9s  A %6d + B %6d -> %6d frames (%5.0f ms)  %s"
              % (t['index'], t['name'], len(A), len(B), len(M),
                 1000.0 * len(M) / rc.ENGINE_RATE, ' '.join(made)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
