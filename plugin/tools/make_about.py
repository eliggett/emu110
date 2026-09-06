#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn resources/about.txt into a header the UI compiles in.

The About box has to be readable in a plugin bundle that a host has copied somewhere
we never see, so the text cannot be a file looked up at runtime -- there would be
nowhere to look it up from.  Compiling it in also means the credits cannot go missing
from a build, which for a GPL plugin carrying BSD sources is the point of them.

Kept out of the artwork stamp deliberately: this needs nothing but python3, so a
clone regenerates it every time and it is never checked in.  The artwork is the
opposite case -- it needs Inkscape and a font we cannot ship -- which is why that one
is committed and this one is not.

usage: make_about.py <about.txt> <about_text.h>
"""
import sys


def c_string(s):
    """One line as a C string literal.  Escaped by hand rather than with repr(),
    which would give Python's escapes, not C's."""
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\t':
            out.append('    ')          # the box draws no tab stops
        elif ord(ch) < 0x20 or ord(ch) == 0x7f:
            continue                    # control characters have no meaning here
        else:
            out.append(ch)
    return '"' + ''.join(out) + '\\n"'


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__.rstrip() + '\n')
        return 2

    with open(argv[1], encoding='utf-8') as f:
        # '#' in the FIRST column only, so a URL fragment or a sharp in prose survives.
        lines = [ln.rstrip('\n').rstrip()
                 for ln in f if not ln.startswith('#')]

    # Blank lines at either end would show as an empty band inside the box.
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()

    out = ['// GENERATED FILE -- do not edit.',
           f'// Produced by plugin/tools/make_about.py from {argv[1].split("/")[-1]}.',
           '// The text lives in resources/about.txt; edit that and rebuild.',
           '',
           '#pragma once',
           '',
           'namespace voltaire {',
           '',
           'inline constexpr const char *kAboutText =']
    if lines:
        out += ['    ' + c_string(ln) for ln in lines]
    else:
        out.append('    ""')
    out[-1] += ';'
    out += ['', '} // namespace voltaire', '']

    with open(argv[2], 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
