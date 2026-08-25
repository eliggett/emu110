#!/usr/bin/env python3
"""The U-110's 50 Output Modes (Owner's Manual p.27, "Output Modes").

Each mode partitions the 31 voices into Voice Groups.  A Part's Output Assign selects a
group, and group N drives Multi Output N.  Modes 21-50 treat outputs 1 and 2 as one group
with the effect switchable -- per the manual's footnote:

    "In the Output modes 21 to 50, Multi Outputs 1 and 2 are regarded as the same Voice
     Group, and effect can be turned on or off.  The one without effect (M) is set to the
     center position of the sound imaging, and the one with effect is stereo output (L,R)."

'LR' marks the wet stereo pair (occupying groups 1 and 2), 'M' the dry centred one (group 1).
The scan's OCR dropped some cells; every row is checked against the invariant that the voice
counts sum to 31, which is what recovers them.
"""

# (kind, [group sizes])   kind: None = plain, 'LR' = stereo pair, 'M' = mono centre
MODES = {
     1: (None, [31]),                  2: (None, [27, 4]),
     3: (None, [23, 8]),               4: (None, [23, 4, 4]),
     5: (None, [19, 12]),              6: (None, [19, 8, 4]),
     7: (None, [19, 4, 4, 4]),         8: (None, [15, 16]),
     9: (None, [15, 12, 4]),          10: (None, [15, 8, 8]),
    11: (None, [15, 8, 4, 4]),        12: (None, [15, 4, 4, 4, 4]),
    13: (None, [11, 12, 8]),          14: (None, [11, 12, 4, 4]),
    15: (None, [11, 8, 8, 4]),        16: (None, [11, 8, 4, 4, 4]),
    17: (None, [11, 4, 4, 4, 4, 4]),  18: (None, [7, 8, 8, 8]),
    19: (None, [7, 8, 8, 4, 4]),      20: (None, [7, 8, 4, 4, 4, 4]),

    21: ('LR', [31]),                 22: ('M',  [31]),
    23: ('LR', [16, 15]),             24: ('M',  [16, 15]),
    25: ('LR', [16, 11, 4]),          26: ('M',  [16, 11, 4]),
    27: ('LR', [16, 7, 8]),           28: ('M',  [16, 7, 8]),
    29: ('LR', [16, 7, 4, 4]),        30: ('M',  [16, 7, 4, 4]),
    31: ('LR', [16, 3, 4, 4, 4]),     32: ('M',  [16, 3, 4, 4, 4]),
    33: ('LR', [8, 23]),              34: ('M',  [8, 23]),
    35: ('LR', [8, 19, 4]),           36: ('M',  [8, 19, 4]),
    37: ('LR', [8, 15, 8]),           38: ('M',  [8, 15, 8]),
    39: ('LR', [8, 15, 4, 4]),        40: ('M',  [8, 15, 4, 4]),
    41: ('LR', [8, 11, 12]),          42: ('M',  [8, 11, 12]),
    43: ('LR', [8, 11, 8, 4]),        44: ('M',  [8, 11, 8, 4]),
    45: ('LR', [8, 11, 4, 4, 4]),     46: ('M',  [8, 11, 4, 4, 4]),
    47: ('LR', [8, 7, 8, 8]),         48: ('M',  [8, 7, 8, 8]),
    49: ('LR', [8, 7, 8, 4, 4]),      50: ('M',  [8, 7, 8, 4, 4]),
}


def groups(mode):
    """[(first_voice, last_voice, output_or_pair)] for a mode; voices are numbered 1..31."""
    kind, sizes = MODES[mode]
    out, v = [], 1
    for i, n in enumerate(sizes):
        if i == 0 and kind:
            out.append((v, v + n - 1, 'LR' if kind == 'LR' else 'M'))
            first_plain = 3            # groups 1+2 consumed by the pair
        else:
            out.append((v, v + n - 1, (i + first_plain) if kind else i + 1))
        if i == 0 and not kind:
            first_plain = 1
        v += n
    return out


if __name__ == '__main__':
    bad = [m for m, (k, s) in MODES.items() if sum(s) != 31]
    print("modes defined : %d" % len(MODES))
    print("rows summing to 31: %d/%d %s" % (50 - len(bad), 50, "" if not bad else "FAILED: %s" % bad))
    print("\nmode 20 (Wide Piano):")
    for a, b, o in groups(20):
        print("   voices %2d-%2d -> output %s" % (a, b, o))
    print("mode 22 (Ac.Piano):")
    for a, b, o in groups(22):
        print("   voices %2d-%2d -> %s" % (a, b, o))
