# Miyoo Mini / Mini Plus build toolchain

Everything needed to cross-compile Butterscotch for Onion v2 on an ARMv7
hard-float handheld. Previously these pieces lived in `/tmp` and had to be
recreated by hand after every reboot.

| Path | What it is |
| --- | --- |
| `zigcc`, `zigar`, `zigranlib` | Compiler/archiver wrappers: `zig cc -target arm-linux-gnueabihf.2.17 -mcpu=cortex_a7` |
| `toolchain.cmake` | CMake toolchain file pointing at the wrappers above |
| `include/SDL/` | SDL 1.2 headers (the target has SDL 1.2, not SDL2) |
| `lib/libSDL-1.2.so.0` | ARM SDL 1.2 the binary links against (also shipped in the release zip) |
| `fetch-zig.sh` | Downloads Zig 0.17.0 into `.zig/` (ignored by git) |
| `build.sh` | Configures and builds `build-arm-zig/butterscotch` |

## Why Zig

Onion ships glibc 2.17. Distro and ARM-vendor cross toolchains nowadays target
glibc 2.3x+, so their binaries die with a `GLIBC_2.28 not found` error on the
device. Zig can target an arbitrary glibc version and supplies matching stubs,
which is why it is used here.

## First time on a fresh machine

```sh
tools/miyoo-arm/fetch-zig.sh     # ~55 MB download, 400 MB unpacked, into .zig/
tools/ffmpeg/build-arm.sh        # one-off: static H.264/AAC ffmpeg, see below
tools/miyoo-arm/build.sh         # -> build-arm-zig/butterscotch
```

## Video playback (optional but recommended)

Deltarune Chapter 3 and 5 open `vid/*.mp4` cutscenes through GameMaker's
`video_*` API. Decoding those needs ffmpeg, so `tools/ffmpeg/build-arm.sh`
builds a minimal static copy (mov demuxer, H.264 + AAC decoders, swscale) with
the same zig toolchain and installs it into `tools/ffmpeg/arm/`. Neither the
sources nor the build output are committed; the script re-downloads them.

`build.sh` points pkg-config only at that prefix, so a host ffmpeg can never be
picked up by accident. If the directory is missing, the binary is built without
video support: the `video_*` built-ins then report "no video", which makes the
game take its own fallback path instead of hanging on a black screen.

`ZIG=/path/to/zig` overrides the compiler if you keep Zig elsewhere
(`fetch-zig.sh` only handles x86_64 Linux hosts).

## Verify before shipping

```sh
arm-none-linux-gnueabihf-strip build-arm-zig/butterscotch
readelf --version-info build-arm-zig/butterscotch | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
readelf -A build-arm-zig/butterscotch | grep -E 'CPU_name|FP_arch|SIMD'
readelf -d build-arm-zig/butterscotch | grep NEEDED
```

Expected: newest symbol version `GLIBC_2.17`, `Tag_CPU_name "cortex-a7"`,
`VFPv4` + `NEONv1`, and `NEEDED` limited to `libSDL-1.2.so.0`, `libm`, `libc`,
`libpthread`, `libdl`.

## Provenance

`include/SDL/` is the stock SDL 1.2 header set (LGPL 2.1) and
`lib/libSDL-1.2.so.0` is the ARM SDL 1.2 library that was already shipped in
the release zip, kept here so the linker has something to link against.
