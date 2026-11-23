#!/usr/bin/env bash
# Cross-builds the Windows HookshotVR Launcher on Linux with llvm-mingw (no Windows PC, no CMake): vr/launcher/build/windows/
# hookshotvr-launcher.exe, with its fonts, art and changelog beside it as build.sh does.
#
#   HOOKSHOT_MINGW=<llvm-mingw's Linux release> vr/launcher/build-windows.sh     (DEBUG=1: -O0 -g and a console window)
#   ... ICON=<256 px png>    the exe's icon (default: the Linux launcher's --icon, when vr/launcher/build has one; else none)
#
# The same llvm-mingw release Hookshot's Windows build uses (scripts/make_setup_parts.py LLVM; its Linux twin in
# .github/workflows/windows-builder.yml). The C and C++ runtimes are linked in: the exe needs only Windows' own DLLs, and at
# run time SDL2.dll (with SDL3.dll, which Hookshot's sdl2-compat SDL2.dll loads) and openxr_loader.dll beside it. PNGs are
# read and written with Windows' own codec (WIC), so no libpng. A UTF-8 manifest is linked in (nx2/win_manifest.py), as the
# game's: argv and the C library's paths are UTF-8.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build/windows"
: "${HOOKSHOT_MINGW:?set HOOKSHOT_MINGW to the Linux release of llvm-mingw (github.com/mstorsjo/llvm-mingw, ucrt-ubuntu-22.04-x86_64)}"
CXX="$HOOKSHOT_MINGW/bin/x86_64-w64-mingw32-clang++"
[[ -x "$CXX" ]] || { echo "no $CXX"; exit 1; }
IMGUI="$REPO/third_party/imgui"
mkdir -p "$OUT/obj"
OPT=(-O2)
SUBSYSTEM=(-mwindows)   # (no console window when the player double-clicks it)
[[ -n "${DEBUG:-}" ]] && OPT=(-O0 -g) && SUBSYSTEM=()
FLAGS=(-std=c++17 "${OPT[@]}" -Wall -Wno-unused-function -I"$IMGUI" -I"$REPO/vr/third_party"
       -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
       -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00)
LIBS=(-static -lole32 -lshell32 -ladvapi32 -luuid -lwindowscodecs)

objs=()
compile() {   # <source> <object>: again only when the source, a header or the flags changed
  local src="$1" obj="$2"
  objs+=("$obj")
  if [[ -f "$obj" && "$obj" -nt "$src" && "$obj" -nt "$HERE/build-windows.sh" ]]; then
    local newer
    newer="$(find "$HERE/src" "$IMGUI" -name '*.h' -newer "$obj" -print -quit)"
    [[ -z "$newer" ]] && return 0
  fi
  echo "  $(basename "$src")"
  "$CXX" "${FLAGS[@]}" -c "$src" -o "$obj"
}
for f in imgui imgui_draw imgui_tables imgui_widgets; do compile "$IMGUI/$f.cpp" "$OUT/obj/$f.o"; done
for f in "$HERE"/src/*.cpp; do
  case "$f" in *_posix.cpp) continue;; esac   # (platform_posix.cpp: Linux's half, src/platform.h)
  compile "$f" "$OUT/obj/$(basename "${f%.cpp}").o"
done

# (the manifest and the icon, as a .res the link takes)
ICON="${ICON:-}"
if [[ -z "$ICON" && -x "$HERE/build/hookshotvr-launcher" ]]; then
  ICON="$OUT/obj/icon.png"
  "$HERE/build/hookshotvr-launcher" --icon "$ICON" >/dev/null 2>&1 || ICON=""
fi
python3 -I "$HERE/tools/win_res.py" "$OUT/obj/launcher.res" ${ICON:+"$ICON"}
[[ -n "$ICON" ]] || echo "(no icon: build the Linux launcher first, vr/launcher/build.sh, or set ICON)"

"$CXX" "${objs[@]}" "$OUT/obj/launcher.res" -o "$OUT/hookshotvr-launcher.exe" "${SUBSYSTEM[@]}" "${LIBS[@]}"

mkdir -p "$OUT/fonts" "$OUT/art"
cp -f "$REPO/third_party/fonts/libertinus/LibertinusSerif-Semibold.ttf" "$OUT/fonts/"   # (the fallback)
cp -f "$REPO/third_party/fonts/libertinus/OFL.txt" "$OUT/fonts/Libertinus-OFL.txt"
shopt -s nullglob
for f in "$HERE"/fonts/*.ttf "$HERE"/fonts/*.otf "$HERE"/fonts/*.txt "$HERE"/fonts/*.md; do cp -f "$f" "$OUT/fonts/"; done
for f in "$HERE"/art/*.png; do cp -f "$f" "$OUT/art/"; done
cp -f "$HERE/CHANGELOG.txt" "$OUT/"
echo "$OUT/hookshotvr-launcher.exe"
