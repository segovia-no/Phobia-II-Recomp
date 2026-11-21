#!/usr/bin/env bash
# Headless gameplay stress: hidden window, silent audio, scripted movement + fire for N seconds.
#   scripts/dev/stress.sh OUTDIR [SECONDS] [RUNS]
# Each run's log goes to OUTDIR/run_<i>.log; FATAL lines are summarised at the end.
set -uo pipefail
# Run on a copy of the game folder (PHOBIA_DATA) so scripted play does not touch your high scores.
OUT="${1:?output dir}"; SECS="${2:-180}"; RUNS="${3:-4}"
BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/phobia2"
mkdir -p "$OUT"
keys_for() {  # pseudo-random play: menu -> start, then turns/moves while holding fire
  python3 - "$1" "$SECS" <<'PY'
import random, sys
r = random.Random(int(sys.argv[1])); end = int(sys.argv[2]) * 1000
ev = ['3000:Down', '4000:Return', '9000:Return', '11000:Return']
t = 13000
while t < end:
    k = r.choice(['Left', 'Right', 'Up', 'Up', 'Down', 'Left', 'Right'])
    ev.append(f'{t}:{k}:{r.randint(150, 900)}')
    ev.append(f'{t + 50}:Left Ctrl:{r.randint(200, 1500)}')
    t += r.randint(300, 1200)
print(','.join(ev))
PY
}
for ((i = 1; i <= RUNS; i++)); do
  SHOTS=""; for ((t = 15000; t < SECS * 1000; t += 15000)); do SHOTS+="${SHOTS:+,}$t:$OUT/run_${i}_$t.bmp"; done
  PHOBIA_HIDDEN=1 SDL_AUDIODRIVER=dummy PHOBIA_DATA="${PHOBIA_DATA:-}" PHOBIA_NO_MSGBOX=1 PHOBIA_KEYS="$(keys_for $i)" PHOBIA_SHOTS="$SHOTS" \
    timeout -s KILL "$SECS" "$BIN" > "$OUT/run_$i.log" 2>&1 &
done
wait
grep -l FATAL "$OUT"/run_*.log 2>/dev/null | while read -r f; do echo "== $f"; grep -A4 FATAL "$f"; done
echo "done: $RUNS runs x ${SECS}s"
