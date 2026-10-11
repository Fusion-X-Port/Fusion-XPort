#!/bin/sh
# Fusion X-Port — build SDL3 for the Nintendo 3DS
#
# devkitPro does not package SDL3 for the 3DS yet (the 3ds portlibs repo
# still ships SDL 1.2 only), but SDL3 supports the 3DS natively.  These
# are the official build instructions from SDL's docs/README-n3ds.md:
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   cmake --install build
#
# This script performs them and installs into the portlibs prefix so the
# project Makefile can pick SDL3 up through $(PORTLIBS).
set -e

: "${DEVKITPRO:?Please export DEVKITPRO=<path to>devkitpro}"
PORTLIBS_PREFIX="${PORTLIBS_PREFIX:-$DEVKITPRO/portlibs/3ds}"

SDL3_REPO="https://github.com/libsdl-org/SDL"
SDL3_DIR="${SDL3_DIR:-$DEVKITPRO/SDL3-src}"

if [ ! -d "$SDL3_DIR" ]; then
    echo "==> Cloning SDL3 into $SDL3_DIR"
    git clone --depth 1 "$SDL3_REPO" "$SDL3_DIR"
fi

echo "==> Configuring SDL3 for 3DS (toolchain: $DEVKITPRO/cmake/3DS.cmake)"
cmake -S "$SDL3_DIR" -B "$SDL3_DIR/build-3ds" \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PORTLIBS_PREFIX"

echo "==> Building"
cmake --build "$SDL3_DIR/build-3ds" -j"$(nproc 2>/dev/null || echo 4)"

echo "==> Installing into $PORTLIBS_PREFIX"
cmake --install "$SDL3_DIR/build-3ds"

echo "==> Done.  SDL3 for 3DS is now in $PORTLIBS_PREFIX"
echo "    Build Fusion X-Port with:  make"
