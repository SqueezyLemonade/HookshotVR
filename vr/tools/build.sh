#!/usr/bin/env bash
# Builds the game with Hookshot VR from this repository's sources.
#
#   vr/tools/build.sh <game folder> [<work folder>]
#
# Uses the toolchain of an installed Hookshot builder (HOOKSHOT_BUILDER: the unpacked builder's root, with usr/python,
# usr/lib/hookshot) and this repository as Hookshot's tools. <game folder> is where the built game goes (and where it was
# built before); <work folder> keeps the translation between builds (default: <game folder>/../work).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:?usage: vr/tools/build.sh <game folder> [<work folder>]}"
WORK="${2:-$(dirname "$GAME")/work}"
: "${HOOKSHOT_BUILDER:?set HOOKSHOT_BUILDER to an unpacked Hookshot builder (the folder holding usr/python and usr/lib/hookshot)}"
LIB="$HOOKSHOT_BUILDER/usr/lib/hookshot"
export HOOKSHOT_TOOLS="$REPO"
export EMSDK="$LIB/emsdk" HOOKSHOT_SYSROOT="$LIB/sysroot" HOOKSHOT_FFMPEG="$LIB/ffmpeg" HOOKSHOT_SDL2="$LIB/libSDL2-2.0.so.0"
export HOOKSHOT_KEEP_BUILD_FILES=1 HOOKSHOT_NO_UPDATE_CHECK=1
# (a developer's build: the probes, test modes and diagnostics switches compiled in, nx2/build.py VR_RESEARCH; a player's build
# leaves it unset. HOOKSHOT_VR_RESEARCH=0 vr/tools/build.sh ... builds as a player's)
export HOOKSHOT_VR_RESEARCH="${HOOKSHOT_VR_RESEARCH:-1}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$(dirname "$GAME")/cache}"
unset HOOKSHOT_VK_PREBUILT
exec "$HOOKSHOT_BUILDER/usr/python/bin/python3" -I "$REPO/nx2/make_game.py" \
  --update "$GAME" --work "$WORK" --jobs "${HOOKSHOT_JOBS:-4}" --no-prepare-shaders --log "$(dirname "$GAME")/build.log"
