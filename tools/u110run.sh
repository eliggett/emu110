#!/bin/bash
# Run the U-110 driver headlessly -- no window at all.
#
#   tools/u110run.sh [-p PATCH] [-t SECONDS] [-m MIDIFILE] [-w WAVOUT] [extra mame args...]
#
# SDL_VIDEODRIVER=dummy is the part that actually suppresses the window: MAME's
# "-video none" still asks SDL for a window, which pops a black rectangle onto the
# desktop.  The dummy driver gives it one that is never mapped.
#
# Each run gets a scratch NVRAM directory, so the patch number always starts at P-01
# and patch selection is reproducible (see tools/select_patch.lua).

set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PATCHNO=1; SECS=20; MIDI=""; WAV=""; NV="$(mktemp -d)"
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PATCHNO="$2"; shift 2 ;;
    -t) SECS="$2";    shift 2 ;;
    -m) MIDI="$2";    shift 2 ;;
    -w) WAV="$2";     shift 2 ;;
    *)  break ;;
  esac
done
ARGS=(u110 -seconds_to_run "$SECS" -nothrottle -video none
      -nvram_directory "$NV" -autoboot_script "$HERE/tools/select_patch.lua")
[ -n "$MIDI" ] && ARGS+=(-min "$MIDI")
if [ -n "$WAV" ]; then ARGS+=(-wavwrite "$WAV"); else ARGS+=(-sound none); fi
cd "$HERE/mame"
SDL_VIDEODRIVER=dummy U110_PATCH="$PATCHNO" ./u110 "${ARGS[@]}" "$@"
rc=$?
rm -rf "$NV"
exit $rc
