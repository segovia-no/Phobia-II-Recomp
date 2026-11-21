# 07 — Builder, Linux port and release kits (2026-11-21)

Same design as MotoRacerRecomp (its notes/28-easy-install.md, phases E1–E3): nothing translated from
`Game.exe` is ever distributed. A release kit ships everything else prebuilt; the player's machine only
translates and compiles the game code, with tools that come in the download.

## Host program + game-code module
- **Host** `phobia2`: runtime + SDL2, built with `-DPHOBIA_GAME_MODULE=ON` and no `Game.exe` (CMake builds
  the program only). It exports its symbols (`ENABLE_EXPORTS`: `-rdynamic` on Linux, executables export by
  default on macOS).
- **Module** `gamecode.so` / `gamecode.dylib`: the translator's C only (`lift.py --module`), compiled with
  `-fPIC -fvisibility=hidden`. It exports one symbol, `phobia_module` (`PhobiaModule` in `runtime.h`):
  version, image bytes and layout, dispatch table, import table, SMC routine table, import slots, and a
  translator id (hash of `Game.exe` + recompiler).
- **Module → host**: `M`, `dispatch`, `rt_*`, `fpu_*`, x87 80-bit helpers, resolved by the dynamic linker
  when the host loads the module (`-undefined dynamic_lookup` on macOS; undefined symbols are allowed in
  Linux shared objects).
- **Host → module**: everything through `phobia_module`. Import shims go through slots: the module's
  `IMP_<DLL>_<Name>` are hidden thunks through `g_import_slots[i]`; `runtime/module.c` fills the slots by
  looking the shims up by name in the program (`dlsym` on `dlopen(NULL)`; the hidden thunks can never be
  found that way). A missing shim leaves the slot empty and the thunk reports the import by name.
- **Static builds** (the default `./setup.sh`) are unchanged in behaviour: the same `module.c` descriptor is
  linked in, the import glue is weak fallbacks replaced by the runtime's strong shims at link time.
- Module search: `$PHOBIA_GAME_MODULE`, next to the program, `<program>/gamecode/`, `<game folder>/gamecode/`.

## Builder: `recompiler/build_gamecode.py`
```
build_gamecode.py --exe Game.exe --out DIR [--cc auto|zig|cc|PATH] [--zig ZIG] [--zig-lib DIR]
                  [--target macos|linux] [--arch arm64|x86_64] [--jobs N] [--if-stale]
```
Translates (`lift.py --module`), compiles the 22 files in parallel and links `DIR/gamecode.<so|dylib>`.
- Compiler: a bundled `zig cc` (cross-compiles to every target with no SDK) or the system `cc` for native
  builds. zig targets: `aarch64/x86_64-macos.11.0`, `aarch64/x86_64-linux-gnu.2.28`.
- `--if-stale`: `DIR/gamecode.stamp` holds the SHA-256 of `Game.exe`, of the recompiler files and the
  target; when it matches, nothing is rebuilt (the launcher calls it on every start).
- A kit's zig keeps only its own platform's C headers; a cross build points `--zig-lib` at the target
  kit's `tools/zig/lib` (as MotoRacer does with `ZIG_LIB_DIR`).
- Measured (MacBook, 11 jobs): translate 3 s, compile 6–7 s, link 2–5 s.

## Release kits
| Script | |
|---|---|
| `scripts/release/build_kit.sh P [version]` | all of the below for one platform |
| `scripts/release/fetch_tools.py --platform P --out DIR` | python-build-standalone 3.12.15 + pefile/capstone wheels, zig 0.16.0 pruned to the platform; pins and SHA-256 in `toolchain.json` (checksums from the upstream SHA256SUMS / ziglang index / PyPI). Adapted from MotoRacerRecomp. |
| `scripts/release/build_linux_host.sh x86_64\|arm64` | host in a Debian 11 container (glibc 2.31 floor; archived repos), SDL2 2.30.12 built from the pinned source with every backend loaded at run time — the bundled `libSDL2-2.0.so.0` needs only libc; RPATH `$ORIGIN/lib` |
| `scripts/release/build_macos_host.sh arm64\|x86_64` | host + SDL2 from the same source, deployment target macOS 11.0 |
| `scripts/release/package.py --platform P --host H --tools T` | `build/release/PhobiaIIRecomp-<version>-<P>.tar.gz` |

Kit layout: `phobia2.sh` (launcher), `phobia2`, `lib/` (SDL2), `recompiler/`, `include/` (`cpu.h`,
`runtime.h`), `tools/` (python, zig, licenses), empty `game/`, `README.txt`, `LICENSE`. About 85–90 MB
compressed. The launcher runs the bundled Python on `build_gamecode.py --if-stale`, then starts the
program with `PHOBIA_DATA` and `PHOBIA_GAME_MODULE` set.

## Linux port
The runtime was already SDL2-based; the platform-specific parts were:
- executable path: `_NSGetExecutablePath` → `SDL_GetBasePath`;
- MIDI: AudioToolbox `MusicPlayer` on macOS only; elsewhere `mciSendString` succeeds silently (the game
  uses MCI only for its optional MIDI music; no completion notice is posted, which would make the game
  re-issue `play ... notify` in a loop);
- `_GNU_SOURCE` for `memmem`, `strcasestr`, `FNM_CASEFOLD`; `-ldl -lpthread` on glibc 2.31.

## Checks
- macOS: static build, module build (CMake), module from `build_gamecode.py` with system cc and with zig:
  all reach gameplay, 128 of 128 import shims.
- Linux arm64 (Docker, native): host from `build_linux_host.sh`, module cross-built by zig on the Mac;
  gameplay, death effect and high-score menu headless (`SDL_VIDEODRIVER=dummy`).
- Linux x86-64 (Docker, emulated): same, 20 presents/s (the game's own frame rate) even under emulation.
- **Kits as a player gets them** (all four): Linux kits in a bare `debian:bookworm` container (no Python,
  no compiler, no SDL): the first start builds the module with the bundled tools (arm64 11 s; x86-64 a few
  minutes because zig itself runs emulated on the Mac), then plays; the second start is a cache hit.
  macOS arm64 natively, macOS x86-64 under Rosetta (build 45 s): same.
- Source build (`./setup.sh`) on Ubuntu 24.04: plays. Found there: glibc's fortified `realpath()` aborts
  with a buffer smaller than `PATH_MAX`; path buffers are now `HOST_PATH_MAX` (4096).

## Not yet
- First-run UI inside the program (MotoRacer E4); the launcher is a shell script for now.
- CI (GitHub Actions) building the kits on tags; `.app` bundle; Steam Deck "add to Steam" helper.
- Windows.
