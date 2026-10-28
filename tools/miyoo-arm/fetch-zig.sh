#!/bin/sh
# Downloads the Zig release used as the Miyoo cross compiler into
# tools/miyoo-arm/.zig (ignored by git; only needed once per machine).
#
# Only x86_64 Linux hosts are handled here; on other hosts install Zig yourself
# and point the ZIG environment variable at it when building.
set -e

version=0.17.0
here=$(cd "$(dirname "$0")" && pwd)
url="https://ziglang.org/download/${version}/zig-x86_64-linux-${version}.tar.xz"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $url"
curl -fL --progress-bar "$url" -o "$tmp/zig.tar.xz"

rm -rf "$here/.zig"
mkdir -p "$here/.zig"
tar -xJf "$tmp/zig.tar.xz" -C "$here/.zig" --strip-components=1

"$here/.zig/zig" version
