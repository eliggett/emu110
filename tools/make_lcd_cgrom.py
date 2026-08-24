#!/usr/bin/env python3
"""
Generate a SYNTHETIC 5x8 character generator ROM for the U-110's LCD.

This is NOT a dump.  The real controller's CGROM has not been dumped, and without
some font the emulated display renders blank glyphs.  This synthesizes an ASCII
font (0x20-0x7F) from DejaVu Sans Mono so the machine is legible on screen.
Codes 0x00-0x0F come from the controller's CGRAM at runtime and are left zero;
0x80-0xFF (katakana on the real part) are left blank.

Layout expected by msm6222b_device::render():
    16 bytes per character, rows 0..7 used, bits 4..0 = pixels left..right.
"""
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
SIZE, BASELINE, XOFF = 8, 7, -1

def main():
    font = ImageFont.truetype(FONT, SIZE)
    rom = bytearray(0x1000)
    for code in range(0x20, 0x80):
        img = Image.new("1", (5, 8), 0)
        ImageDraw.Draw(img).text((XOFF, BASELINE), chr(code), font=font, fill=1, anchor="ls")
        for row in range(8):
            bits = 0
            for x in range(5):
                if img.getpixel((x, row)):
                    bits |= 1 << (4 - x)
            rom[16 * code + row] = bits
    with open('mame/roms/u110/u110_lcd_cgrom.bin', 'wb') as f:
        f.write(rom)
    print("wrote mame/roms/u110/u110_lcd_cgrom.bin (%d bytes)" % len(rom))

if __name__ == '__main__':
    main()
