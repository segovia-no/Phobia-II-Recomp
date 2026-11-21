# Building from source

## Requirements
- macOS (Apple Silicon or Intel), Xcode command line tools (`clang`)
- `brew install sdl2 cmake`
- Python 3.10+ (the setup script creates `.venv` with `capstone` and `pefile`)
- Your copy of Phobia II v2.0: a folder with `Game.exe`, `pics/`, `sounds/`, `dat/`

## Setup script
```sh
./setup.sh [path/to/game]     # default ./game
```
It creates `.venv`, configures CMake into `build/`, recompiles `Game.exe` into `build/gen/` and builds `build/phobia2`.

## By hand
```sh
python3 -m venv .venv && .venv/bin/pip install -r recompiler/requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGAME_DIR="$PWD/game"
cmake --build build -j
```
The translation step is a custom command: editing `recompiler/lift.py` or `recompiler/extra_entries.txt` re-runs it.

Running: `build/phobia2` finds the game folder in this order: `$PHOBIA_DATA`, next to the binary (`.`, `game/`, `../game`, `../../game`), the current directory. Writes (high scores, config) go to the game folder, as the original did.

## Debug builds and developer aids
| CMake option | Effect |
|---|---|
| `-DPHOBIA_RECOMP_TRACE=ON` | per-instruction hook; `PHOBIA_TR=413dc3-413e02` prints registers and x87 stack for that guest range, `PHOBIA_TR_BT=1` adds a host backtrace |
| `-DPHOBIA_RECOMP_CHECK=ON` | checks every guest `ret` lands on its call site's next instruction |

| Environment variable | Effect |
|---|---|
| `PHOBIA_TRACE=1` | logs every Win32 import call with its first four arguments, COM method calls, file opens |
| `PHOBIA_KEYS="3000:Down,4000:Return:100"` | injects key presses (ms since start : SDL key name [: hold ms]) |
| `PHOBIA_SHOTS="5000:/tmp/a.bmp,..."` | saves presented frames at given times |
| `PHOBIA_WAV=/tmp/out.f32` | dumps the mixed audio (raw float32 stereo 44.1 kHz) |
| `PHOBIA_AUDIO_STATS=1` | logs sound buffer creation and mixer statistics |
| `PHOBIA_CONSOLE=1` | shows the game's stdout/stderr (a GUI app on Windows has none; Watcom's `matherr` prints there) |
| `PHOBIA_BPP=8` / `16` | forces the display depth offered to the game |
| `PHOBIA_GUARD_SURF=1` | write-protected guard pages around surface memory (outside the slack): a stray write faults at the writer |
| `PHOBIA_GUARD_COM=1` | every COM object on its own read-only page |
| `PHOBIA_NOFREE=1` | never reuse freed guest heap blocks (use-after-free hunting) |
| `PHOBIA_HIDDEN=1` | hidden window (headless runs; combine with `SDL_AUDIODRIVER=dummy`) |
| `PHOBIA_FPS=1` | log presents per second |
| `PHOBIA_NOSOUND`, `PHOBIA_NOMIDI`, `PHOBIA_NO_MSGBOX`, `PHOBIA_FULLSCREEN` | as named |

`scripts/dev/e2e.sh OUTDIR [seconds] [keys]` wraps keys + frame dumps; `PHOBIA_DATA=/tmp/gamecopy scripts/dev/stress.sh OUTDIR [seconds] [runs]` runs parallel headless games with scripted movement and fire (use a copy of the game folder: scripted play writes high scores); `scripts/dev/disas.py game/Game.exe 0xADDR [n]` disassembles the original.

When the runtime stops with `dispatch: no compiled entry at XXXXXXXX`, add the address to `recompiler/extra_entries.txt` and rebuild.

## Game-bug patches
`recompiler/patches.py` maps a guest instruction address to replacement C. It is reserved for bugs in the original game that the faithful translation exposes (on 1998 hardware they were hidden by spare video memory or never hit). Each entry documents the bug.
