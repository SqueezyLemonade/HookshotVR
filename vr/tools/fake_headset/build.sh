#!/usr/bin/env bash
# Builds the stand-in OpenXR runtime (fake_openxr.cpp) as <out folder>/libopenxr_loader.so.1. A run then finds it first with
#   LD_LIBRARY_PATH=<out folder> HOOKSHOT_VR=1 HOS_HEADLESS=1 ...
# (the game dlopens libopenxr_loader.so.1: vr/headset/runtime_loader.inc). Needs the Vulkan headers (RX_CPM's, or the system's).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?usage: vr/tools/fake_headset/build.sh <out folder>}"
mkdir -p "$OUT"
VKINC=""
if [ -n "${RX_CPM:-}" ]; then VKINC="$(ls -d "$RX_CPM"/vulkanheaders/*/include 2>/dev/null | sort | tail -1)"; fi
"${CXX:-clang++}" -std=c++20 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -I"$HERE/../../third_party" ${VKINC:+-I"$VKINC"} -o "$OUT/libopenxr_loader.so.1" "$HERE/fake_openxr.cpp" -ldl -lpthread
echo "fake headset: $OUT/libopenxr_loader.so.1"
