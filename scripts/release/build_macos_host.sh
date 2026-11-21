#!/usr/bin/env bash
# Build the macOS host program (no game code) for macOS 11+, with SDL2 built from the pinned source.
#   scripts/release/build_macos_host.sh [arm64|x86_64]   -> build-macos-<arch>/phobia2 + libSDL2-2.0.0.dylib
# Needs Xcode command line tools and cmake.
set -euo pipefail
ARCH="${1:-$(uname -m)}"
case "$ARCH" in arm64|x86_64) ;; *) echo "arch: arm64 or x86_64"; exit 1 ;; esac
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE="$ROOT/build/tools/cache"
OUT="$ROOT/build-macos-$ARCH"
read -r SDL_URL SDL_FILE SDL_SHA < <(python3 -c "
import json; d=json.load(open('$ROOT/scripts/release/toolchain.json'))['sdl2']
print(d['url'].format(file=d['file'][0]), d['file'][0], d['file'][1])")
mkdir -p "$CACHE"
[ -f "$CACHE/$SDL_FILE" ] || curl -fsSL -o "$CACHE/$SDL_FILE" "$SDL_URL"
echo "$SDL_SHA  $CACHE/$SDL_FILE" | shasum -a 256 -c - >/dev/null || { echo "SDL2 checksum mismatch"; exit 1; }
export MACOSX_DEPLOYMENT_TARGET=11.0
SRC="$OUT/sdl-src"; PREFIX="$OUT/sdl2"
rm -rf "$SRC" && mkdir -p "$SRC" && tar xzf "$CACHE/$SDL_FILE" -C "$SRC" --strip-components=1
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
cmake --build "$SRC/build" -j"$(sysctl -n hw.ncpu)" >/dev/null && cmake --install "$SRC/build" >/dev/null
cmake -S "$ROOT" -B "$OUT" -DCMAKE_BUILD_TYPE=Release -DPHOBIA_GAME_MODULE=ON -DGAME_DIR=/nonexistent \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_FIND_FRAMEWORK=NEVER >/dev/null
cmake --build "$OUT" -j"$(sysctl -n hw.ncpu)" >/dev/null
cp -L "$PREFIX/lib/libSDL2-2.0.0.dylib" "$OUT/"
echo "built $OUT/phobia2"
