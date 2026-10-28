#!/bin/sh
# Configures and builds the Miyoo Mini / Mini Plus binary into build-arm-zig/.
#
# The result is build-arm-zig/butterscotch; strip it before shipping:
#     arm-none-linux-gnueabihf-strip build-arm-zig/butterscotch
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

if [ -z "${ZIG:-}" ] && [ ! -x tools/miyoo-arm/.zig/zig ] && ! command -v zig >/dev/null 2>&1; then
    echo "zig not found: run tools/miyoo-arm/fetch-zig.sh or set ZIG=/path/to/zig" >&2
    exit 1
fi

cmake -B build-arm-zig -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/miyoo-arm/toolchain.cmake" \
    -DBACKEND=sdl1 \
    -DAUDIO_BACKEND=sdl1 \
    -DENABLE_SW_RENDERER=ON \
    -DENABLE_LEGACY_GL=OFF \
    -DENABLE_MODERN_GL=OFF \
    -DSDL_INCLUDE_DIR="$root/tools/miyoo-arm/include" \
    -DSDL_LIBRARY="$root/tools/miyoo-arm/lib/libSDL-1.2.so.0" \
    -DSDL_LIBRARY_TEMP="$root/tools/miyoo-arm/lib/libSDL-1.2.so.0" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-arm-zig -j"$(nproc)"

echo
echo "Built $root/build-arm-zig/butterscotch (not stripped)"
