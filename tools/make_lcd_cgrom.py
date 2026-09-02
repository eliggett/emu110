#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Bake a 5x8 character generator ROM for the U-110's LCD from MatrixSans Screen.

This is NOT a dump.  The real controller's CGROM has never been dumped, so without some
font the emulated display renders blank glyphs.  MatrixSans is SIL OFL with NO Reserved
Font Name, so a modified version may even keep the name; attribution is still owed and
resources/fonts/MatrixSans/{OFL,FONTLOG}.txt stay in the tree.

WHY THIS FONT.  The FONTLOG describes the Screen variant as "separate square dots, like an
LCD screen", and it is drawn on a real dot grid: glyphs span 486 x 686 font units with dots
at a 97.2 x 98 pitch, so sampling dot CENTRES recovers the designer's bitmap exactly rather
than guessing at a rasterisation.  DejaVu rasterised to 5x8, which this replaces, is mush.

THE ONE COMPROMISE.  MatrixSans wants NINE rows -- 7 above the baseline and 2 of descender.
The hardware has eight.  Descenders are clipped from two rows to one, which is exactly what
the real HD44780 A00 table does, so the result is more faithful to a character LCD, not
less.

Output layout, as msm6222b_device::render() expects it:
    16 bytes per character, rows 0..7 used, bits 4..0 = pixels left to right.
Codes 0x00-0x0F come from the controller's CGRAM at runtime and are left zero -- the
firmware writes them itself, and the boot logo is an ANIMATION of them (PLUGIN-PLAN.md 7).
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WOFF2 = os.path.join(HERE, 'resources/fonts/MatrixSans/fonts/webfonts/'
                           'MatrixSansScreen-Regular.woff2')

# Measured from the font itself; see the module docstring.
EM = 1000
X0, X1 = 57, 543          # horizontal extent of the dot grid
CAP_TOP, CAP_BOT = 693, 7  # the seven rows above the baseline
ASCENT = 1000
COLS, ROWS_FONT, ROWS_LCD = 5, 9, 8

# Hand corrections.  Five columns is a brutal grid and a few glyphs cannot survive an
# automatic sampling; each of these was compared against the rendered proof sheet.
# Rows are top to bottom, '#' lit.  Anything not listed comes straight from the font.
HAND = {
    '#': ('.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.', '.....'),
    '%': ('##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##', '.....'),
    '@': ('.###.', '#...#', '#.###', '#.#.#', '#.###', '#....', '.###.', '.....'),
}


def load_font():
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit('fontTools is required (python3-fonttools)')
    import tempfile
    f = TTFont(WOFF2)
    f.flavor = None
    tmp = os.path.join(tempfile.gettempdir(), 'MatrixSansScreen.ttf')
    f.save(tmp)
    return tmp


def sample_glyph(font, ch):
    """Render one character and read the dots back at their centres."""
    img = Image.new('L', (EM, EM + 400), 0)
    ImageDraw.Draw(img).text((0, ASCENT), ch, font=font, fill=255, anchor='ls')

    dot_w = (X1 - X0) / COLS
    dot_h = (CAP_TOP - CAP_BOT) / (ROWS_FONT - 2)      # seven rows span cap height

    rows = []
    for r in range(ROWS_FONT):
        bits = 0
        for c in range(COLS):
            fx = X0 + dot_w * (c + 0.5)
            fy = CAP_TOP - dot_h * (r + 0.5)
            px, py = int(fx), int(ASCENT - fy)
            if 0 <= px < img.width and 0 <= py < img.height and img.getpixel((px, py)) > 127:
                bits |= 1 << (4 - c)
        rows.append(bits)

    # Nine rows down to eight: fold the second descender row into the first.
    return rows[:ROWS_LCD - 1] + [rows[ROWS_LCD - 1] | rows[ROWS_LCD]]


def hand_rows(pattern):
    return [sum(1 << (4 - c) for c, ch in enumerate(line) if ch == '#') for line in pattern]


def proof_sheet(rom, path, scale=4):
    """A picture of all 96 glyphs, because five columns mangles a few and the only way to
    know which is to look at them."""
    cw, chh, pad = COLS, ROWS_LCD, 1
    cols, rows = 16, 6
    W = cols * (cw + pad) * scale
    H = rows * (chh + pad) * scale
    img = Image.new('RGB', (W, H), (10, 24, 10))
    d = ImageDraw.Draw(img)
    for i in range(96):
        code = 0x20 + i
        gx, gy = (i % cols) * (cw + pad) * scale, (i // cols) * (chh + pad) * scale
        for r in range(chh):
            bits = rom[16 * code + r]
            for c in range(cw):
                on = bits & (1 << (4 - c))
                col = (120, 255, 120) if on else (18, 40, 18)
                x, y = gx + c * scale, gy + r * scale
                d.rectangle([x, y, x + scale - 2, y + scale - 2], fill=col)
    img.save(path)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(HERE, 'mame/roms/u110/u110_lcd_cgrom.bin'))
    ap.add_argument('--proof', default=None, help='write a PNG proof sheet here')
    ap.add_argument('--header', default=None,
                    help='also write the table as a C header for the plugin UI')
    args = ap.parse_args()

    ttf = load_font()
    font = ImageFont.truetype(ttf, EM)

    rom = bytearray(0x1000)
    fixed = []
    for code in range(0x20, 0x80):
        ch = chr(code)
        if ch in HAND:
            rows = hand_rows(HAND[ch])
            fixed.append(ch)
        else:
            rows = sample_glyph(font, ch)
        for r in range(ROWS_LCD):
            rom[16 * code + r] = rows[r]

    with open(args.out, 'wb') as f:
        f.write(rom)
    print('wrote %s (%d bytes)' % (os.path.relpath(args.out, HERE), len(rom)))
    if fixed:
        print('hand-corrected: %s' % ' '.join(fixed))

    if args.header:
        with open(args.header, 'w') as f:
            f.write('// GENERATED by tools/make_lcd_cgrom.py -- do not edit.\n'
                    '//\n'
                    '// The U-110 LCD character set, baked from MatrixSans Screen.  NOT a dump:\n'
                    '// the real controller CGROM has never been dumped.  See the tool for the\n'
                    '// licensing (SIL OFL) and for the one compromise, descenders clipped from\n'
                    '// two rows to one, which is what the real HD44780 A00 table does too.\n'
                    '//\n'
                    '// Five bits per row, bit 4 leftmost; eight rows per character; codes\n'
                    '// 0x00-0x0F are CGRAM and come from the running firmware instead.\n'
                    '#pragma once\n\n'
                    'static const unsigned char kU110Cgrom[96][8] = {\n')
            for code in range(0x20, 0x80):
                rows = ', '.join('0x%02x' % rom[16 * code + r] for r in range(8))
                ch = chr(code)
                f.write('    { %s },  // 0x%02X  %s\n'
                        % (rows, code, ("'%s'" % ch) if ch != '\\' else "backslash"))
            f.write('};\n')
        print('header: %s' % os.path.relpath(args.header, HERE))

    if args.proof:
        print('proof sheet: %s' % proof_sheet(rom, args.proof))


if __name__ == '__main__':
    main()
