#!/usr/bin/env bash
# Build Phobia II Recomp from your own copy of the game.
#   ./setup.sh [path/to/game-folder]     (default: ./game)
# Needs: python3, cmake, a C compiler, SDL2 (macOS: brew install sdl2 cmake;
# Debian/Ubuntu: sudo apt install build-essential cmake libsdl2-dev python3-venv).
set -euo pipefail
cd "$(dirname "$0")"
GAME_DIR="${1:-$PWD/game}"
if [ ! -f "$GAME_DIR/Game.exe" ]; then
  echo "Game.exe not found in $GAME_DIR — pass the folder holding Game.exe, pics/, sounds/ and dat/." >&2
  exit 1
fi
if [ ! -x .venv/bin/python ]; then
  python3 -m venv .venv
  .venv/bin/pip install -q --upgrade pip
fi
.venv/bin/pip install -q -r recompiler/requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGAME_DIR="$GAME_DIR"
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo
echo "Built build/phobia2. Run it with:  PHOBIA_DATA=\"$GAME_DIR\" ./build/phobia2"
