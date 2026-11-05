#!/bin/sh
# Builds jkrelay_server as fully static Linux binaries (musl libc, baseline
# CPU) for x86-64 and ARM64. They run on any 64-bit Linux of that
# architecture: no glibc version or newer-CPU requirements.
#
# Needs Zig as the cross compiler: your distro's "zig" package, or
#   pip install ziglang     (then ZIG="python3 -m ziglang")
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
mkdir -p dist
for arch in x86_64 aarch64; do
    $ZIG cc -target $arch-linux-musl -mcpu=baseline -static -O2 -s \
        -o dist/jkrelay_server-linux-$arch jkrelay_server.c -I../../src/Platform/Networking/DPlay
    echo "built dist/jkrelay_server-linux-$arch"
done
