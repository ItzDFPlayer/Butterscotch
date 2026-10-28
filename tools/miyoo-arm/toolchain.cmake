# CMake toolchain for the Miyoo Mini / Mini Plus (ARMv7 hard-float, Onion v2).
#
# Onion provides glibc 2.17, which no current cross toolchain targets any more, so
# the C compiler here is Zig (bundled clang plus glibc 2.17 stubs) wrapped by
# tools/miyoo-arm/zigcc. CMAKE_SYSTEM_NAME is deliberately not set: it would match
# the host value anyway, and setting it makes CMake try to execute target binaries
# during configuration.
#
# Example (tools/miyoo-arm/build.sh does exactly this):
#   cmake -B build-arm-zig -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=tools/miyoo-arm/toolchain.cmake \
#     -DBACKEND=sdl1 -DAUDIO_BACKEND=sdl1 -DENABLE_SW_RENDERER=ON \
#     -DENABLE_LEGACY_GL=OFF -DENABLE_MODERN_GL=OFF -DCMAKE_BUILD_TYPE=Release

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zigcc")
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zigar")
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zigranlib")
set(CMAKE_C_FLAGS_INIT "-I${CMAKE_CURRENT_LIST_DIR}/include")

# libSDL-1.2.so.0 is linked by absolute path, which would otherwise bake a
# RUNPATH with this machine's directory layout into the binary. The device finds
# the library through the port folder / Onion's LD_LIBRARY_PATH instead.
set(CMAKE_SKIP_RPATH ON)
