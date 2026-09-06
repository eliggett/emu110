#!/bin/sh
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build tools/clap_selftest.c, point it at the CLAP build, and insist that the UI saw at
# least one panel update come across from the DSP.
#
# Zero is the failure this exists to catch, and zero is what DPF shipped: its CLAP
# wrapper's updateState() discarded the value and returned true, so the plugin drew a dead
# LCD in every CLAP host while behaving perfectly under LV2.  Nothing in the suite noticed,
# because nothing in the suite had ever opened a UI under CLAP.
#
# A display is needed -- the queue is only drained while a UI exists, so proving the
# channel works means opening one.  Xvfb is used if there is no DISPLAY, and the check
# skips rather than fails if there is neither.
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 1

CLAP="${1:-bin/Voltaire110.clap}"
BIN="build/clap_selftest"
LOG="build/clap_selftest.log"

if [ ! -e "$CLAP" ]; then
    echo "clap: $CLAP is not built -- skipped"
    exit 0
fi

gcc -std=c11 -O2 -o "$BIN" tools/clap_selftest.c -Idpf/distrho/src -ldl || exit 1

XVFB=""
if [ -z "${DISPLAY:-}" ]; then
    if command -v Xvfb > /dev/null 2>&1; then
        DISPLAY=":$(( ($$ % 100) + 70 ))"
        export DISPLAY
        Xvfb "$DISPLAY" -screen 0 1400x900x24 > /dev/null 2>&1 &
        XVFB=$!
        sleep 2
    else
        echo "clap: no DISPLAY and no Xvfb -- skipped"
        exit 0
    fi
fi

VOLTAIRE_TRACE_STATE=1 U110_DATA_DIR="${U110_DATA_DIR:-$HERE/..}" "$BIN" "$CLAP" \
    > "$LOG" 2>&1
rc=$?
[ -n "$XVFB" ] && kill "$XVFB" 2> /dev/null

if [ "$rc" != 0 ]; then
    echo "clap: the host harness exited $rc -- see $LOG"
    tail -5 "$LOG"
    exit 1
fi

panels=$(grep -c "ui panel #" "$LOG" 2> /dev/null)
[ -n "$panels" ] || panels=0

if [ "$panels" -lt 1 ]; then
    echo "clap: FAILED -- the UI decoded no panel updates in a whole run."
    echo "clap: the DSP -> UI channel is dead, which looks like a working plugin with a"
    echo "clap: blank LCD. See patches/0003-clap-dsp-to-ui-state.patch."
    exit 1
fi

echo "clap: the UI decoded $panels panel updates -- the DSP -> UI channel is open"
