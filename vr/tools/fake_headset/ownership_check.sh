#!/usr/bin/env bash
# The headset layers' failure handling, in the game, without a headset (vr/headset/eye_rendering.inc: swap_take,
# headset_submit, headset_gpu_free). A windowed run with the stand-in headset and its faults: every 2000th acquire and every
# 3000th wait fail (one each 10-20 s at 90 Hz), and every 12 s the headset's queue is held 1.5 s, so the game's next
# headset submit waits past its 1 s.
# Passes when the game logged each kind of failure, held and then let go the timed-out submit, showed headset frames again
# after it, and never lost the GPU.
#
#   vr/tools/fake_headset/ownership_check.sh <game folder> <stand-in folder> <out folder (made)>
#
# The stand-in folder: vr/tools/fake_headset/build.sh <folder>. Needs a desktop (the window shows) and wmctrl; the caller
# takes the PC's game lock and job queue as for any game run. Uses the builder's Python when HOOKSHOT_BUILDER is set.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="${1:?usage: ownership_check.sh <game folder> <stand-in folder> <out folder>}"
FAKE="${2:?stand-in folder}"
OUT="${3:?out folder}"
PY="${HOOKSHOT_BUILDER:+$HOOKSHOT_BUILDER/usr/python/bin/python3}"
PY="${PY:-python3}"
"$PY" -I -B "$HERE/window_test.py" --game "$GAME" --fake "$FAKE" --out "$OUT" --resizes 6 --every 8 \
  --set threaded_rendering=1 --env FAKE_XR_FAIL_ACQUIRE=2000 --env FAKE_XR_FAIL_WAIT=3000 --env FAKE_XR_STALL_S=12
rc=$?
LOG="$OUT/run.log"
[ -f "$LOG" ] || { echo "ownership_check: FAIL: no log"; exit 1; }
bad=0
need() {   # need <what> <pattern>: at least one line
    local n
    n=$(grep -ac -- "$2" "$LOG")
    printf 'ownership_check: %-44s %5d\n' "$1" "$n"
    [ "$n" -gt 0 ] || bad=1
}
none() {
    local n
    n=$(grep -ac -- "$2" "$LOG")
    printf 'ownership_check: %-44s %5d\n' "$1 (none wanted)" "$n"
    [ "$n" -eq 0 ] || bad=1
}
need "stalls the stand-in made" "the headset's queue held"
need "acquires the stand-in failed" "FAKE_XR_FAIL_ACQUIRE"
need "the game: an acquire failed, layer left out" "swapchain image's acquire failed"
need "the game: a wait failed, layer left out" "swapchain image's wait failed"
need "the game: a submit's wait gave up, held" "submit did not finish in 1 s"
need "the game: that submit let go once done" "submit finished .* s after it was given"
none "device lost" "device lost\|DEVICE_LOST\|FATAL"
# headset frames shown after the last let-go line
last=$(grep -an "submit finished .* s after it was given" "$LOG" | tail -1 | cut -d: -f1)
shown=0
[ -n "$last" ] && shown=$(tail -n +"$last" "$LOG" | grep -ao "vr: [1-9][0-9]* headset frames shown" | wc -l)
printf 'ownership_check: %-44s %5d\n' "headset frame lines with frames after it" "$shown"
[ "$shown" -gt 0 ] || bad=1
[ "$rc" -eq 0 ] || { echo "ownership_check: window_test exit $rc"; bad=1; }
[ "$bad" -eq 0 ] && echo "ownership_check: PASS" || echo "ownership_check: FAIL (see $LOG)"
exit "$bad"
