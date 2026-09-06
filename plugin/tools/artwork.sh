#!/bin/sh
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decide whether the panel artwork in generated/ still matches the Inkscape files it
# came from, and re-export it only if it does not.
#
# The artwork is CHECKED IN, which is unusual for a build product and is the point: the
# panel is lettered in a font we have no right to redistribute (see CLONING.md), so a
# clone must be able to build the exact panel we ship without that font, without
# Inkscape and without rsvg-convert.  Only someone CHANGING the artwork needs any of
# those.
#
# [!] This cannot be make's job, and that is the whole reason this script exists.  Git
# does not record modification times -- every file in a fresh clone is stamped with the
# moment it was checked out, in whatever order the checkout happened to write them.  A
# timestamp rule therefore fires or does not fire at random on a clone, and when it
# fires on a machine without the font, Inkscape substitutes SILENTLY and the build
# quietly replaces our panel with one lettered in Noto Sans.  Content hashes are the
# only thing that survives a checkout intact.
#
# usage: artwork.sh [--force]
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 1

GFX=../resources/graphics
OUT=generated
STAMP="$OUT/artwork.stamp"

# Everything the exported artwork depends on.  The exporters are in here too: a change
# to how a header is written has to re-export just as surely as a change to a shape.
INPUTS="$GFX/overall_panel_inkscape.svg
$(ls $GFX/dive_*.svg 2>/dev/null | grep -v dive_panel_inkscape.svg)
tools/panel_export.py
../tools/make_lcd_cgrom.py"

# What has to be present for a build to succeed without re-exporting.
OUTPUTS="$OUT/panel_geometry.h $OUT/panel_geometry.json $OUT/panel_svg.h
$OUT/panel_background.h $OUT/dive_pages.h $OUT/u110_cgrom.h $OUT/panel_flat.svg"

force=0
[ $# -gt 0 ] && [ "$1" = "--force" ] && force=1

stamp_now() {
    echo "# plugin/tools/artwork.sh v1 -- sha256 of every artwork input"
    for f in $INPUTS; do
        [ -e "$f" ] || { echo "MISSING $f"; continue; }
        printf '%s  %s\n' "$(sha256sum < "$f" | cut -d' ' -f1)" "$(basename "$f")"
    done
}

missing=""
for f in $OUTPUTS; do
    [ -e "$f" ] || missing="$missing $(basename "$f")"
done

now="$(stamp_now)"

if [ "$force" = 0 ] && [ -z "$missing" ] && [ -e "$STAMP" ] \
        && [ "$now" = "$(cat "$STAMP")" ]; then
    echo "  artwork is current -- using the checked-in export (no fonts needed)"
    exit 0
fi

# From here on we are re-exporting, which needs the tools AND the fonts.  Say why
# before spending anyone's time on a failure they did not ask for.
if [ -n "$missing" ]; then
    echo "  artwork: missing$missing -- exporting"
elif [ ! -e "$STAMP" ]; then
    echo "  artwork: no stamp -- exporting"
else
    echo "  artwork: the Inkscape files changed -- re-exporting"
fi

for t in inkscape rsvg-convert; do
    command -v "$t" > /dev/null 2>&1 && continue
    echo "  ERROR: the artwork needs re-exporting but $t is not installed." >&2
    echo "  ERROR: see CLONING.md.  The checked-in export in $OUT/ matches the" >&2
    echo "  ERROR: PREVIOUS artwork, so building on regardless would ship a panel" >&2
    echo "  ERROR: that is not the one in the Inkscape files." >&2
    exit 1
done

tools/panel_export.py --text-to-path --background || exit 1
mkdir -p "$OUT"
python3 ../tools/make_lcd_cgrom.py --header "$OUT/u110_cgrom.h" || exit 1

for f in $OUTPUTS; do
    [ -e "$f" ] && continue
    echo "  ERROR: the export did not produce $f" >&2
    exit 1
done

# Written LAST, so an export that died half way leaves a stamp that does not match and
# the next build tries again rather than trusting a partial result.
stamp_now > "$STAMP"
echo "  artwork re-exported; commit $OUT/ along with the SVG change"
