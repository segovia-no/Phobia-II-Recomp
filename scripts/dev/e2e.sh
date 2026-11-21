#!/usr/bin/env bash
# Scripted run: inject key presses and dump frames, then kill the game.
#   scripts/dev/e2e.sh OUTDIR [SECONDS] [KEYS]
# KEYS uses PHOBIA_KEYS syntax: "ms:Key[:hold_ms],..." (SDL key names), frames every 2 s go to OUTDIR.
set -euo pipefail
OUT="${1:?output dir}"; SECS="${2:-20}"; KEYS="${3:-3000:Return,6000:Return}"
mkdir -p "$OUT"
BIN="$(dirname "$0")/../../build/phobia2"
SHOTS=""
for ((t = 2000; t < SECS * 1000; t += 2000)); do SHOTS+="${SHOTS:+,}$t:$OUT/frame_$t.bmp"; done
PHOBIA_NO_MSGBOX=1 PHOBIA_KEYS="$KEYS" PHOBIA_SHOTS="$SHOTS" timeout -s KILL "$SECS" "$BIN" > "$OUT/run.log" 2>&1 || true
echo "log: $OUT/run.log"; ls "$OUT"
