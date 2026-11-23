#!/usr/bin/env bash
# Cross-builds the Windows game with Hookshot VR on Linux (RX_TARGET=windows, docs/vr-knowledge's windows-port notes): the
# same make_game.py run the Windows launcher starts on a player's PC, with llvm-mingw's Linux release in place of its Windows
# one. Proves the Windows hookshot.exe and hookshot_vk.dll compile, link and export what the host looks up; run it under Wine
# or Proton, or copy it to a Windows PC. Shaders are not prepared (the game can't run here: it prepares them at first start).
#
#   HOOKSHOT_BUILDER=<unpacked Hookshot Linux builder> HOOKSHOT_MINGW=<llvm-mingw, ucrt-ubuntu-22.04-x86_64> \
#   RX_CPM=<Hookshot's Windows CPM part: tools/cpm, with ffmpeg and dav1d> \
#     scripts/hookshotvr_build_windows.sh <game folder> <work folder> --game <file> | --romfs <romfs> --exefs <exefs> | --update
#
# HOOKSHOT_TOOLS (default: this repository) is the source built: a release's HookshotVR folder (scripts/hookshotvr_release.py
# --keep-staging) builds what a player's build builds, with the third_party part's prebuilt files beside it. The Linux builder
# gives the Python and the host's compiler (the build's own tools, a64emit and zbic, run on this PC). Never a RAM folder: the
# work folder holds gigabytes. On the shared PC, inside flock on the game lock and the job queue (AGENTS.md).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:?usage: scripts/hookshotvr_build_windows.sh <game folder> <work folder> <make_game.py input options>}"
WORK="${2:?usage: scripts/hookshotvr_build_windows.sh <game folder> <work folder> <make_game.py input options>}"
shift 2
: "${HOOKSHOT_BUILDER:?set HOOKSHOT_BUILDER to an unpacked Hookshot Linux builder (usr/python, usr/lib/hookshot)}"
: "${HOOKSHOT_MINGW:?set HOOKSHOT_MINGW to the Linux release of llvm-mingw}"
: "${RX_CPM:?set RX_CPM to the Windows CPM packages (Hookshot Windows builder part cpm: tools/cpm)}"
LIB="$HOOKSHOT_BUILDER/usr/lib/hookshot"
export HOOKSHOT_TOOLS="${HOOKSHOT_TOOLS:-$REPO}"
export RX_TARGET=windows EMSDK="$LIB/emsdk" HOOKSHOT_SYSROOT="$LIB/sysroot"
export HOOKSHOT_NO_UPDATE_CHECK=1 HOOKSHOT_KEEP_BUILD_FILES=1
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$(dirname "$GAME")/cache}"
unset HOOKSHOT_FFMPEG HOOKSHOT_SDL2   # (the Linux builder's: a Windows build takes FFmpeg from RX_CPM)
in=("$@")
[[ "${1:-}" == "--update" ]] && in=(--update "$GAME")
[[ "${1:-}" == "--update" ]] || in+=(--out "$GAME")
exec "$HOOKSHOT_BUILDER/usr/python/bin/python3" -I "$HOOKSHOT_TOOLS/nx2/make_game.py" "${in[@]}" \
  --work "$WORK" --jobs "${HOOKSHOT_JOBS:-8}" --no-prepare-shaders --log "$(dirname "$GAME")/$(basename "$GAME")-build.log"
