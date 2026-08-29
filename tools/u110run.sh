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
#
# It also gets a scratch CFG directory.  MAME persists configuration ports (BOOTKEYS,
# AUTOTEST) to cfg/u110.cfg on exit, so a single run that enables a service test would
# otherwise silently put EVERY later run into the test menu -- notes stop sounding and
# the cause is not obvious.

set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PATCHNO=1; SECS=20; MIDI=""; WAV=""; NV="$(mktemp -d)"; CFG="$(mktemp -d)"
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
      -nvram_directory "$NV" -cfg_directory "$CFG"
      -autoboot_script "$HERE/tools/select_patch.lua")
[ -n "$MIDI" ] && ARGS+=(-min "$MIDI")
# Generated audio belongs in listen/renders/.  A bare filename goes there; the script
# cds into mame/ before launching, so a relative path would otherwise land in mame/.
if [ -n "$WAV" ]; then
  case "$WAV" in
    /*) ;;
    */*) WAV="$(cd "$(dirname "$WAV")" && pwd)/$(basename "$WAV")" ;;
    *)  mkdir -p "$HERE/listen/renders"; WAV="$HERE/listen/renders/$WAV" ;;
  esac
  ARGS+=(-wavwrite "$WAV")
else
  ARGS+=(-sound none)
fi
# Say plainly which mode this run is in.  The driver announces it too, but check the
# persistent config as well: running MAME by hand (without this script) picks that up.
PERSIST="$HERE/mame/cfg/u110.cfg"
if [ -f "$PERSIST" ] && grep -qE '(AUTOTEST|BOOTKEYS)"[^/]*value="[^0]' "$PERSIST"; then
  echo "u110run: WARNING - $PERSIST has a service test enabled:" >&2
  grep -E '(AUTOTEST|BOOTKEYS)' "$PERSIST" | sed 's/^/           /' >&2
  echo "           this run is unaffected (it uses a scratch -cfg_directory)," >&2
  echo "           but running ./u110 by hand WILL boot into the test menu." >&2
fi
# `make SUBTARGET=roland` links mame/roland; u110run.sh runs mame/u110, which is a COPY.
# Forgetting to refresh it means a rebuild silently has no effect and the render measures
# the old code -- which has happened.  Refresh it here instead of warning about it.
if [ -x "$HERE/mame/roland" ] && [ "$HERE/mame/roland" -nt "$HERE/mame/u110" ]; then
  echo "u110run: mame/roland is newer than mame/u110 - refreshing the copy." >&2
  if ! cp "$HERE/mame/roland" "$HERE/mame/u110"; then
    # "Text file busy" means another MAME still has the copy open -- a run that hung, or
    # one left behind by a debugger session.  Carrying on would measure the OLD build,
    # which is the exact trap this refresh exists to close, so stop instead.
    echo "u110run: could not refresh mame/u110 - it is still in use.  Running processes:" >&2
    pgrep -af "mame/u110|[.]/u110" >&2 || true
    echo "u110run: kill those and re-run; refusing to measure a stale binary." >&2
    exit 1
  fi
fi
printf "u110run: patch P-%02d, %ss%s\n" "$PATCHNO" "$SECS" "${MIDI:+, midi $(basename "$MIDI")}"

cd "$HERE/mame"
SDL_VIDEODRIVER=dummy U110_PATCH="$PATCHNO" ./u110 "${ARGS[@]}" "$@"
rc=$?
rm -rf "$NV" "$CFG"
exit $rc
