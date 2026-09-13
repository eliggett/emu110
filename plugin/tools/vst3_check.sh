#!/bin/sh
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build tools/vst3_selftest.cpp, point it at the VST3 build, and insist on two things:
# that the UI saw panel updates come across from the DSP, and that the plugin exists once
# per instance rather than twice.
#
# Both were wrong and nothing noticed, because nothing in the suite had ever opened a UI
# under VST3.  DistrhoPluginVST3.cpp passed a nullptr where every other backend passes the
# updateStateValue callback, so the LCD was blank in every VST3 host forever; and DPF's
# separate edit controller constructed a second Plugin, which for this plugin means a
# second emulated machine, booted, never heard, and the one the UI was reading its patch
# and tone lists from.  See patches/0005 and 0006.
#
# A display is needed -- the queue is only drained while a UI exists, so proving the channel
# works means opening one.  Xvfb is used if there is no DISPLAY, and the check skips rather
# than fails if there is neither.
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 1

VST3="${1:-bin/Voltaire110.vst3}"
BIN="build/vst3_selftest"
LOG="build/vst3_selftest.log"

if [ ! -e "$VST3" ]; then
    echo "vst3: $VST3 is not built -- skipped"
    exit 0
fi

g++ -std=c++20 -O2 -Wall -o "$BIN" tools/vst3_selftest.cpp -Idpf/distrho/src -ldl -lX11 \
    || exit 1

XVFB=""
if [ -z "${DISPLAY:-}" ]; then
    if command -v Xvfb > /dev/null 2>&1; then
        DISPLAY=":$(( ($$ % 100) + 70 ))"
        export DISPLAY
        Xvfb "$DISPLAY" -screen 0 1400x900x24 > /dev/null 2>&1 &
        XVFB=$!
        sleep 2
    else
        echo "vst3: no DISPLAY and no Xvfb -- skipped"
        exit 0
    fi
fi

VOLTAIRE_TRACE_STATE=1 U110_DATA_DIR="${U110_DATA_DIR:-$HERE/..}" "$BIN" "$VST3" \
    > "$LOG" 2>&1
rc=$?
[ -n "$XVFB" ] && kill "$XVFB" 2> /dev/null

if [ "$rc" != 0 ]; then
    echo "vst3: the host harness exited $rc -- see $LOG"
    tail -5 "$LOG"
    exit 1
fi

fail=0

if grep -q "edit controller is SEPARATE" "$LOG"; then
    echo "vst3: FAILED -- the factory offers a separate edit controller, so every instance"
    echo "vst3: builds a second machine whose state is what the UI is shown."
    echo "vst3: See patches/0005-vst3-single-component.patch."
    fail=1
fi

panels=$(grep -c "ui panel #" "$LOG" 2> /dev/null)
[ -n "$panels" ] || panels=0

if [ "$panels" -lt 1 ]; then
    echo "vst3: FAILED -- the UI decoded no panel updates in a whole run."
    echo "vst3: the DSP -> UI channel is dead, which looks like a working plugin with a"
    echo "vst3: blank LCD. See patches/0006-vst3-dsp-to-ui-state.patch."
    fail=1
fi

# The plugin unrefs every message it is given, so a leak here is thousands over a session.
leaked=$(sed -n 's/^vst3: ran .* \([0-9]*\) host message(s) still alive$/\1/p' "$LOG")
if [ -n "$leaked" ] && [ "$leaked" != 0 ]; then
    echo "vst3: FAILED -- $leaked host messages were never released."
    fail=1
fi

[ "$fail" = 0 ] || exit 1

echo "vst3: the UI decoded $panels panel updates -- the DSP -> UI channel is open"
echo "vst3: one plugin instance per instance, no messages leaked"
