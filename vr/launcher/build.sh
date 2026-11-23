#!/usr/bin/env bash
# Builds the HookshotVR Launcher on its own (no game build, no CMake): vr/launcher/build/hookshotvr-launcher.
#
#   vr/launcher/build.sh            (CXX=clang++ to use clang; DEBUG=1 for -O0 -g)
#   vr/launcher/build.sh test       and its self-test: the build's lines, Cancel, the VR runtime choice, two launches and an
#                                   update, on stand-ins (tools/selftest.cpp)
#
# Needs a C++17 compiler and, at run time, SDL2 (libSDL2-2.0.so.0, loaded when the launcher starts: no SDL headers are needed
# to build) and OpenGL 3.3. libpng is used when pkg-config finds it: the art slots and --screenshot; without it the art
# stays drawn.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
CXX="${CXX:-g++}"
IMGUI="$REPO/third_party/imgui"
mkdir -p "$OUT/obj"
OPT=(-O2)
[[ -n "${DEBUG:-}" ]] && OPT=(-O0 -g)
FLAGS=(-std=c++17 "${OPT[@]}" -Wall -Wno-unused-function -I"$IMGUI" -I"$REPO/vr/third_party"
       -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
LIBS=(-ldl -lpthread)
if pkg-config --exists libpng 2>/dev/null; then
  read -r -a PNG_CFLAGS <<<"$(pkg-config --cflags libpng)"
  read -r -a PNG_LIBS <<<"$(pkg-config --libs libpng)"
  FLAGS+=(-DHSL_PNG "${PNG_CFLAGS[@]}")
  LIBS+=("${PNG_LIBS[@]}")
else
  echo "(no libpng: the art slots stay drawn and --screenshot is off)"
fi

objs=()
compile() {   # <source> <object>: again only when the source, a header or the flags changed
  local src="$1" obj="$2"
  objs+=("$obj")
  if [[ -f "$obj" && "$obj" -nt "$src" && "$obj" -nt "$HERE/build.sh" ]]; then
    local newer
    newer="$(find "$HERE/src" "$IMGUI" -name '*.h' -newer "$obj" -print -quit)"
    [[ -z "$newer" ]] && return 0
  fi
  echo "  $(basename "$src")"
  "$CXX" "${FLAGS[@]}" -c "$src" -o "$obj"
}
for f in imgui imgui_draw imgui_tables imgui_widgets; do compile "$IMGUI/$f.cpp" "$OUT/obj/$f.o"; done
for f in "$HERE"/src/*.cpp; do
  case "$f" in *_win32.cpp) continue;; esac   # (platform_win32.cpp: the Windows build's, src/platform.h)
  compile "$f" "$OUT/obj/$(basename "${f%.cpp}").o"
done
"$CXX" "${objs[@]}" -o "$OUT/hookshotvr-launcher" "${LIBS[@]}"

# (beside it: the fonts, the changelog and the art slots, so the build folder runs on its own)
mkdir -p "$OUT/fonts" "$OUT/art"
cp -f "$REPO/third_party/fonts/libertinus/LibertinusSerif-Semibold.ttf" "$OUT/fonts/"   # (the fallback)
cp -f "$REPO/third_party/fonts/libertinus/OFL.txt" "$OUT/fonts/Libertinus-OFL.txt"
shopt -s nullglob
for f in "$HERE"/fonts/*.ttf "$HERE"/fonts/*.otf "$HERE"/fonts/*.txt "$HERE"/fonts/*.md; do cp -f "$f" "$OUT/fonts/"; done
for f in "$HERE"/art/*.png; do cp -f "$f" "$OUT/art/"; done
cp -f "$HERE/CHANGELOG.txt" "$OUT/"
echo "$OUT/hookshotvr-launcher"

if [[ "${1:-}" == "test" ]]; then
  "$CXX" "${FLAGS[@]}" "$HERE/tools/selftest.cpp" "$OUT"/obj/{system,runtime,update,platform_posix}.o -o "$OUT/selftest" -lpthread -ldl
  T="$(mktemp -d "${TMPDIR:-/tmp}/hookshotvr-launcher-test.XXXXXX")"
  trap 'rm -rf "$T"' EXIT
  env -u XR_RUNTIME_JSON "$OUT/selftest" "$T" "$HERE/tools/fake_make_game.py"   # (System's runtime: as the launcher was started)
fi
