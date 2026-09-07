#!/bin/bash
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The install instructions, in ONE place.
#
# The same words go in two different destinations -- the GitHub release page, and an
# INSTALL.txt inside each zip -- and the two must not drift.  Someone reading the release
# page and someone who has already downloaded the zip need to be told the same paths.
# The paths themselves come from README.md, and this file is the only copy of them the
# build system carries.
#
# usage: release-text.sh <section> [version]
#   linux-lv2 | linux-clap | windows-clap   the one platform, for INSTALL.txt
#   notes                                   the whole release body, markdown
set -euo pipefail

TAGLINE="Voltaire110, a plugin emulation of the classic Roland U-110 rompler for modern DAWs"

# ROMs are the user's to supply and this says so in both places.  Shipping them is not a
# thing we could do even if it were convenient.
roms_note() {
    cat <<'EOF'
ROMs are NOT included and cannot be: they are Roland's.  Dump your own U-110 EPROMs or
obtain the files some other way.  You need:

  U110v203.BIN                            the program ROM, version 2.03, exactly 65536 bytes
  roland_t110_u110_u220_waverom0.bin      the four wave ROMs
  roland_t110_u110_u220_waverom1.bin
  roland_t110_u110_u220_waverom2.bin
  roland_t110_u110_u220_waverom3.bin

Any SN-U110-xx card images you have are optional; drop them in the same directory and
they appear in the card slots.
EOF
}

linux_lv2() {
    cat <<'EOF'
Linux, LV2
==========

  mkdir -p ~/.lv2
  unzip Voltaire110-Linux-LV2.zip
  cp -r Voltaire110.lv2 ~/.lv2/

Then put the ROMs in ~/.local/share/Voltaire110/roms/ :

  mkdir -p ~/.local/share/Voltaire110/roms
  cp U110v203.BIN roland_t110_u110_u220_waverom*.bin ~/.local/share/Voltaire110/roms/

Now tell your DAW to rescan for plugins.
If the plugin does not boot, click the LCD screen for the self-check report.
EOF
}

linux_clap() {
    cat <<'EOF'
Linux, CLAP
===========

  mkdir -p ~/.clap
  unzip Voltaire110-Linux-CLAP.zip
  cp Voltaire110.clap ~/.clap/

Then put the ROMs in ~/.local/share/Voltaire110/roms/ :

  mkdir -p ~/.local/share/Voltaire110/roms
  cp U110v203.BIN roland_t110_u110_u220_waverom*.bin ~/.local/share/Voltaire110/roms/

Now tell your DAW to rescan for plugins.
If the plugin does not boot, click the LCD screen for the self-check report.
EOF
}

windows_clap() {
    cat <<'EOF'
Windows, CLAP
=============

Copy Voltaire110.clap to:

  C:\Program Files\Common Files\CLAP\Voltaire110.clap

Then put the ROMs in:

  C:\Users\<your username>\AppData\Local\Voltaire110\roms\

Now tell your DAW to rescan for plugins.
If the plugin does not boot, click the LCD screen for the self-check report.
EOF
}

# The release body.  GitHub lists a release's assets as one flat, alphabetical list --
# there is no such thing as an asset category -- so the platform split is done twice over:
# in these headings, and in the file names, which sort the Linux ones together and the
# Windows ones together.
notes() {
    local version="${1:-}"
    cat <<EOF
**$TAGLINE**

Version \`$version\`.  Linux and Windows, 64-bit.

## What is in this release

| Download | Platform | Format |
|---|---|---|
| \`Voltaire110-Linux-LV2.zip\` | Linux x86-64 | LV2 |
| \`Voltaire110-Linux-CLAP.zip\` | Linux x86-64 | CLAP |
| \`Voltaire110-Windows-CLAP.zip\` | Windows x86-64 | CLAP |

## You supply the ROMs

\`\`\`
$(roms_note)
\`\`\`

---

# Linux

### \`Voltaire110-Linux-LV2.zip\`

\`\`\`bash
mkdir -p ~/.lv2
unzip Voltaire110-Linux-LV2.zip
cp -r Voltaire110.lv2 ~/.lv2/
\`\`\`

### \`Voltaire110-Linux-CLAP.zip\`

\`\`\`bash
mkdir -p ~/.clap
unzip Voltaire110-Linux-CLAP.zip
cp Voltaire110.clap ~/.clap/
\`\`\`

### ROMs, either format

\`\`\`bash
mkdir -p ~/.local/share/Voltaire110/roms
cp U110v203.BIN roland_t110_u110_u220_waverom*.bin ~/.local/share/Voltaire110/roms/
cp *sn-u110* ~/.local/share/Voltaire110/roms/     # cards, if you have any
\`\`\`

---

# Windows

### \`Voltaire110-Windows-CLAP.zip\`

Copy the plugin to:

\`\`\`
C:\\Program Files\\Common Files\\CLAP\\Voltaire110.clap
\`\`\`

Copy the ROM files (all of them) to:

\`\`\`
C:\\Users\\<your username>\\AppData\\Local\\Voltaire110\\roms\\
\`\`\`

---

Now tell your DAW to rescan for plugins.

**If the plugin does not boot, click the LCD screen.**  It shows a self-check report
naming every directory it looked in and what it found there, which answers nearly every
"it loads but makes no sound" question on its own.
EOF
}

case "${1:-}" in
    linux-lv2)    linux_lv2 ;;
    linux-clap)   linux_clap ;;
    windows-clap) windows_clap ;;
    roms)         roms_note ;;
    notes)        notes "${2:-}" ;;
    *) echo "usage: $0 {linux-lv2|linux-clap|windows-clap|roms|notes [version]}" >&2; exit 2 ;;
esac
