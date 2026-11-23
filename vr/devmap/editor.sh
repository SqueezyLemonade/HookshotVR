#!/usr/bin/env bash
# Opens the dev map editor (vr/devmap/editor) on YOUR game files.
#
#   vr/devmap/editor.sh <game folder> [port]
#
# Uses the Python of an installed Hookshot builder (HOOKSHOT_BUILDER: the unpacked builder's root, as vr/tools/build.sh), which
# has what the editor reads models with. Its Build button writes the map's mod into <game folder>/mods/hookshot-vr-devmap;
# restart the game after building, then Insert menu > Teleport > VR test map. Any OS: the same with
#   <builder python> vr/devmap/editor/server.py <game folder>/romfs
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:?usage: vr/devmap/editor.sh <game folder> [port]}"
PORT="${2:-8765}"
: "${HOOKSHOT_BUILDER:?set HOOKSHOT_BUILDER to an unpacked Hookshot builder (the folder holding usr/python)}"
PY="$HOOKSHOT_BUILDER/usr/python/bin/python3"
( sleep 2; command -v xdg-open >/dev/null && xdg-open "http://127.0.0.1:$PORT" >/dev/null 2>&1 ) &
exec "$PY" -I -B "$REPO/vr/devmap/editor/server.py" "$GAME/romfs" --port "$PORT"
