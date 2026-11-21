#!/usr/bin/env bash
# Build the Linux host program (no game code) in a Debian 11 container: glibc 2.31 floor.
#   scripts/release/build_linux_host.sh [x86_64|arm64]   -> build-linux-<arch>/phobia2 + libSDL2-2.0.so.0
# SDL2 is built from the pinned source (scripts/release/toolchain.json) with all video/audio backends
# loaded at run time, so the bundled library depends on libc only and works on any desktop distro.
# Needs Docker. x86_64 on an arm64 Mac runs emulated (slower).
set -euo pipefail
ARCH="${1:-$(uname -m | sed 's/aarch64/arm64/')}"
case "$ARCH" in x86_64|amd64) ARCH=x86_64; PLAT=linux/amd64 ;; arm64|aarch64) ARCH=arm64; PLAT=linux/arm64 ;; *) echo "arch?"; exit 1 ;; esac
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE="$ROOT/build/tools/cache"
read -r SDL_URL SDL_FILE SDL_SHA < <(python3 -c "
import json; d=json.load(open('$ROOT/scripts/release/toolchain.json'))['sdl2']
print(d['url'].format(file=d['file'][0]), d['file'][0], d['file'][1])")
mkdir -p "$CACHE"
[ -f "$CACHE/$SDL_FILE" ] || curl -fsSL -o "$CACHE/$SDL_FILE" "$SDL_URL"
echo "$SDL_SHA  $CACHE/$SDL_FILE" | shasum -a 256 -c - >/dev/null || { echo "SDL2 checksum mismatch"; exit 1; }

docker run --rm --platform "$PLAT" -v "$ROOT":/src -w /src debian:bullseye bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  # Debian 11 is archived: use archive.debian.org (keeps the glibc 2.31 floor)
  echo 'deb http://archive.debian.org/debian bullseye main' > /etc/apt/sources.list
  echo 'deb http://archive.debian.org/debian-security bullseye-security main' >> /etc/apt/sources.list
  echo 'Acquire::Check-Valid-Until \"false\";' > /etc/apt/apt.conf.d/99archive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends cmake make gcc g++ libc6-dev pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxinerama-dev libxfixes-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dev \
    libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev >/dev/null
  mkdir -p /tmp/sdl && tar xzf /src/build/tools/cache/$SDL_FILE -C /tmp/sdl --strip-components=1
  cmake -S /tmp/sdl -B /tmp/sdl/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/sdl2 \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF >/dev/null
  cmake --build /tmp/sdl/build -j\$(nproc) >/dev/null && cmake --install /tmp/sdl/build >/dev/null
  cmake -S /src -B /src/build-linux-$ARCH -DCMAKE_BUILD_TYPE=Release -DPHOBIA_GAME_MODULE=ON \
    -DGAME_DIR=/nonexistent -DCMAKE_PREFIX_PATH=/opt/sdl2 >/dev/null
  cmake --build /src/build-linux-$ARCH -j\$(nproc) >/dev/null
  cp -L /opt/sdl2/lib/libSDL2-2.0.so.0 /src/build-linux-$ARCH/
  echo built /src/build-linux-$ARCH/phobia2
  objdump -p /src/build-linux-$ARCH/libSDL2-2.0.so.0 | grep NEEDED
"
