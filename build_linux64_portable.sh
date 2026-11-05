#!/bin/bash
# Builds a Linux binary for other people's machines: inside an Ubuntu 22.04
# container (Podman), so it needs only glibc 2.35 and a generic x86-64 CPU.
# Runs on Linux Mint 21+, LMDE 6, Ubuntu 22.04+, Debian 12+, Pop!_OS...
# A build on the desktop (CachyOS: newest glibc, x86-64-v3) runs only there.
#
# Output: build_linux64_portable/openjkdf2 (stripped Release build).
# Players need OpenAL, GTK 3 and libcurl, e.g. on Mint/Debian/Ubuntu:
#   sudo apt install libopenal1 libgtk-3-0 libcurl4
#
# The first run builds the container image (a few minutes); later runs reuse
# it, and the build folder, so they only recompile what changed.
# Rebuild the image after changing the package list: OPENJKDF2_REBUILD_IMAGE=1.
set -euo pipefail

IMAGE=localhost/openjkdf2-build:ubuntu22.04
BUILD_DIR=build_linux64_portable
GLIBC_MAX=2.35

cd "$(dirname "$0")"

if ! command -v podman >/dev/null; then
    echo "Needs Podman (Arch/CachyOS: sudo pacman -Syu podman)." >&2
    exit 1
fi

if [ -n "${OPENJKDF2_REBUILD_IMAGE:-}" ] || ! podman image exists "$IMAGE"; then
    # Same packages as upstream's .github/workflows/ubuntu.yml, plus git for
    # CMake's downloads (libdatachannel) and binutils to check the result.
    # CMake from pip: the build needs 3.25+ (OpenAL::OpenAL from the system
    # OpenAL), Ubuntu 22.04 has 3.22. Only the build tool; glibc stays 2.35.
    podman build -t "$IMAGE" -f - <<'EOF'
FROM docker.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
        build-essential make git ca-certificates python3 python3-pip python3-venv bison imagemagick zsh binutils \
        libgtk-3-dev libopenal-dev libglew-dev libssl-dev libpng-dev libcurl4-openssl-dev \
        libx11-dev libxext-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev \
        libxss-dev libxxf86vm-dev libxtst-dev libxfixes-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols libasound2-dev libpulse-dev libdbus-1-dev libudev-dev \
    && pip3 install --no-cache-dir cogapp cmake==3.31.6 \
    && rm -rf /var/lib/apt/lists/*
EOF
fi

COMMIT="$(git log -1 --format=%H)"
COMMIT_SHORT="$(git rev-parse --short=8 HEAD)"

# keep-id: the build runs as you, so the files it writes are yours.
# The repo is owned by the host user; tell git inside it's fine.
podman run --rm --userns=keep-id \
    -v "$PWD":/src -w /src \
    -e HOME=/tmp \
    -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -e OPENJKDF2_RELEASE_COMMIT="$COMMIT" -e OPENJKDF2_RELEASE_COMMIT_SHORT="$COMMIT_SHORT" \
    -e BUILD_DIR="$BUILD_DIR" -e GLIBC_MAX="$GLIBC_MAX" \
    "$IMAGE" bash -euo pipefail -c '
        cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
        make -C "$BUILD_DIR" -j "$(nproc)" openjkdf2

        bin="$BUILD_DIR/openjkdf2"
        need=$(objdump -T "$bin" | grep -o "GLIBC_[0-9.]*" | sed "s/GLIBC_//" | sort -Vu | tail -1)
        echo "Needs glibc $need or newer."
        if [ "$(printf "%s\n%s\n" "$need" "$GLIBC_MAX" | sort -V | tail -1)" != "$GLIBC_MAX" ]; then
            echo "ERROR: needs a newer glibc than $GLIBC_MAX" >&2
            exit 1
        fi
        echo "Shared libraries it loads:"
        objdump -p "$bin" | awk "/NEEDED/ {print \"  \" \$2}"
    '

ls -la "$BUILD_DIR/openjkdf2"
