#!/bin/bash
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compile MAME's U-110 device sources against plugin/compat/emu.h.
#
# The point of this script is that the file list below is the SAME source MAME builds --
# no copies, no patches.  If a file here needs editing to compile, the shim is wrong.
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
MAME="$HERE/mame"
OUT="$HERE/plugin/build"
mkdir -p "$OUT"

GEN="$OUT/generated"
INCS=(-I "$HERE/plugin/compat"
      -I "$MAME/src/devices"
      -I "$MAME/src/lib/util"
      -I "$MAME/src/osd"
      -I "$MAME/src/mame/roland"
      -I "$GEN"
      -I "$HERE/plugin/core")

# NOTE: mame/src/emu is deliberately NOT on the include path.  If it were, `#include
# "emu.h"` would find MAME's and the whole exercise would silently compile the wrong thing.

SOURCES=(
  "$MAME/src/devices/sound/roland_lp.cpp"
  "$MAME/src/devices/cpu/mcs96/mcs96.cpp"
  "$MAME/src/devices/cpu/mcs96/i8x9x.cpp"
  "$MAME/src/devices/video/msm6222b.cpp"
  "$MAME/src/devices/sound/flt_biquad.cpp"
  "$MAME/src/devices/sound/flt_rc.cpp"
  "$MAME/src/devices/cpu/mcs96/mcs96d.cpp"
  "$MAME/src/devices/cpu/mcs96/i8x9xd.cpp"
  "$HERE/plugin/compat/emu_shim.cpp"
  "$HERE/plugin/core/u110_core.cpp"
  # Standalone MAME utility sources -- used as-is, they pull in nothing but the standard
  # library.  Reimplementing them would be pointless duplication.
  "$MAME/src/lib/util/strformat.cpp"
  "$MAME/src/lib/util/disasmintf.cpp"
)

CXXFLAGS=(-std=c++20 -O2 -g -Wall -Wno-unused-variable -Wno-unused-but-set-variable
          -Wno-unused-function -fno-strict-aliasing)

# mcs96ops.lst is compiled into mcs96.hxx / i8x9x.hxx by MAME's own generator.  The plugin
# build runs the same Python step rather than checking generated files in.
MK="$MAME/src/devices/cpu/mcs96/mcs96make.py"
LST="$MAME/src/devices/cpu/mcs96/mcs96ops.lst"
mkdir -p "$GEN/cpu/mcs96"
# "s" emits the CPU's opcode switch, "d" the disassembler's table.  Same generator and
# same .lst MAME uses, so the two builds cannot diverge here either.
for spec in "s mcs96" "s i8x9x" "d i8x9x"; do
  set -- $spec
  out="$GEN/cpu/mcs96/$2$([ "$1" = d ] && echo d).hxx"
  if [ ! -f "$out" ] || [ "$LST" -nt "$out" ]; then
    echo "generating $(basename "$out")"
    python3 "$MK" "$1" "$2" "$LST" "$out" || exit 1
  fi
done

OBJS=()
fail=0
for src in "${SOURCES[@]}"; do
  obj="$OUT/$(basename "${src%.cpp}").o"
  printf '  %-24s ' "$(basename "$src")"
  if g++ "${CXXFLAGS[@]}" "${INCS[@]}" -c "$src" -o "$obj" 2> "$obj.log"; then
    echo "ok"
    OBJS+=("$obj")
  else
    echo "FAILED  ($(grep -c 'error:' "$obj.log") errors, see $(realpath --relative-to="$HERE" "$obj.log"))"
    fail=1
  fi
done
[ $fail -ne 0 ] && exit 1

# Compiling proves the declarations line up; only linking proves the definitions do.
echo
printf '  %-24s ' "link + smoke test"
if g++ "${CXXFLAGS[@]}" "${INCS[@]}" "$HERE/plugin/tools/link_check.cpp" "${OBJS[@]}" \
        -o "$OUT/link_check" 2> "$OUT/link_check.log"; then
  echo "linked"
  echo
  "$OUT/link_check" || fail=1
else
  echo "FAILED  (see $(realpath --relative-to="$HERE" "$OUT/link_check.log"))"
  fail=1
fi

# The core renderer the null test drives.
echo
printf '  %-24s ' "u110_render"
if g++ "${CXXFLAGS[@]}" "${INCS[@]}" "$HERE/plugin/tools/u110_render.cpp" "${OBJS[@]}" \
        -o "$OUT/u110_render" 2> "$OUT/u110_render.log"; then
  echo "built"
else
  echo "FAILED  (see $(realpath --relative-to="$HERE" "$OUT/u110_render.log"))"
  fail=1
fi
exit $fail
