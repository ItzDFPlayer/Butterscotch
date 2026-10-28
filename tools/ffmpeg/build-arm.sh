#!/bin/sh
# Builds a minimal static ffmpeg (demuxer + H.264/AAC decoders + swscale) for the
# Miyoo Mini / Mini Plus, using the same zig toolchain as the main binary.
#
# Only what video playback needs is enabled, because every extra decoder is tens of
# kilobytes and the device has 128 MB of RAM in total:
#   - mov/mp4 demuxing (Deltarune ships vid/*.mp4)
#   - H.264 video + AAC audio decoding
#   - swscale, for YUV420 -> RGB555
#
# The result is installed into tools/ffmpeg/arm, and tools/miyoo-arm/build.sh points
# pkg-config at it, so the runner picks the libraries up automatically.
#
# Usage: sh tools/ffmpeg/build-arm.sh
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
here="$root/tools/ffmpeg"
src="$here/ffmpeg-7.1"
prefix="$here/arm"

if [ ! -d "$src" ]; then
    echo "fetching ffmpeg 7.1 sources..."
    ( cd "$here" && \
      curl -fL -o ffmpeg-7.1.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz && \
      tar xf ffmpeg-7.1.tar.xz ) || {
        echo "could not fetch ffmpeg sources into $here" >&2
        exit 1
    }
fi

cd "$src"

# The compiler wrappers live next to the toolchain and pick zig from
# tools/miyoo-arm/.zig automatically.
export PATH="$root/tools/miyoo-arm:$PATH"

./configure \
    --prefix="$prefix" \
    --target-os=linux \
    --arch=arm \
    --enable-cross-compile \
    --cc=zigcc \
    --ar=zigar \
    --ranlib=zigranlib \
    --disable-everything \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --disable-autodetect \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-encoders \
    --disable-muxers \
    --disable-bsfs \
    --disable-indevs \
    --disable-outdevs \
    --enable-decoder=h264 \
    --enable-decoder=aac \
    --enable-parser=h264 \
    --enable-parser=aac \
    --enable-demuxer=mov \
    --enable-protocol=file \
    --enable-swscale \
    --enable-swresample \
    --enable-static \
    --disable-shared \
    --disable-asm \
    --extra-cflags="-O3 -fPIC" \
    --pkg-config=false

# glibc 2.17 as shipped for this target has no <sys/sysctl.h>, but configure's
# sysctl() probe still succeeds (the symbol lingers in libc), so libavutil would try
# to include a header that does not exist. The only user is libavutil's CPU-count
# query, which has a sysconf() fallback.
sed -i 's/^#define HAVE_SYSCTL 1$/#define HAVE_SYSCTL 0/' config.h

make -j"$(nproc)"
make install

echo
echo "ffmpeg installed into $prefix"
du -sh "$prefix/lib"/*.a 2>/dev/null || true
