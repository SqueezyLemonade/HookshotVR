#!/usr/bin/env bash
# The Linux launcher as the release ships it: build.sh with the Hookshot builder's own clang and glibc 2.35 sysroot (the one
# the game is built against), and libstdc++ linked in, so it starts on Ubuntu 22.04, Debian 12 and newer; a build with this
# PC's g++ asks for this PC's glibc (2.38 and up). libpng stays the system's (libpng16.so.16, on every desktop distribution).
#
#   HOOKSHOT_BUILDER=<unpacked Hookshot Linux builder> vr/launcher/build-release-linux.sh [test]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"   # (-ffile-prefix-map: the asserts' file names without the builder's folders)
: "${HOOKSHOT_BUILDER:?set HOOKSHOT_BUILDER to an unpacked Hookshot Linux builder (usr/lib/hookshot/emsdk, sysroot)}"
LIB="$HOOKSHOT_BUILDER/usr/lib/hookshot"
PNG="$(pkg-config --variable=libdir libpng 2>/dev/null || echo /usr/lib/x86_64-linux-gnu)/libpng16.so.16"
[[ -f "$PNG" ]] || { echo "no $PNG: install libpng (libpng-dev)"; exit 1; }
T="$HERE/build/release-linux"
mkdir -p "$T/lib"
ln -sf "$PNG" "$T/lib/libpng16.so"   # (only libpng from the system: the C library is the sysroot's)
cat > "$T/cxx" <<EOF
#!/bin/sh
exec "$LIB/emsdk/upstream/bin/clang++" --sysroot="$LIB/sysroot" -fuse-ld=lld -static-libstdc++ -static-libgcc -L"$T/lib" -ffile-prefix-map="$REPO"=. "\$@"
EOF
chmod +x "$T/cxx"
rm -rf "$HERE/build/obj" "$HERE/build/hookshotvr-launcher"   # (no objects of another compiler in the link)
CXX="$T/cxx" "$HERE/build.sh" "$@"
need="$(objdump -T "$HERE/build/hookshotvr-launcher" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
echo "needs $need"
[[ "$need" == "GLIBC_2.35" || "$(printf '%s\n' "$need" GLIBC_2.35 | sort -V | tail -1)" == "GLIBC_2.35" ]] || { echo "needs more than glibc 2.35"; exit 1; }
