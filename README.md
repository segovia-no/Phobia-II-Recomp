# Phobia II Recomp

![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Linux-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Play **Phobia II** (WAH-Software, 1998), a top-down alien shooter for Windows 95, natively on modern Macs and Linux PCs (x86-64 and arm64, Steam Deck desktop mode).

This is a *static recompilation*: the original Windows executable (`Game.exe`, Watcom C/C++ 32-bit) is translated to C ahead of time and compiled into a native program, linked against a small runtime that re-implements the Windows, DirectDraw, DirectSound, DirectInput and WinMM parts the game uses on top of SDL2. No emulator, no Wine.

> **You need your own copy of the game.** This repository contains no game code or assets. Everything game-specific (the generated C and the binary) is made from your `Game.exe` on your own computer.

## Status
| Area | State |
|---|---|
| Boot, Watcom C runtime, WinMain, threads | working |
| Colour-depth menu, title, gameplay, light effects, death effect, high scores | working |
| Keyboard (DirectInput + window messages) | working |
| Sound effects and music (DirectSound streaming, the game's self-modifying mixer) | working |
| MIDI music (MCI sequencer, not used by default) | macOS (AudioToolbox); silent on Linux |
| Linux x86-64 / arm64 | working (Docker-tested headless; glibc 2.31+) |
| Gamepad (WinMM joystick) | via SDL GameController |

## Release kits
A kit (`PhobiaIIRecomp-<version>-<platform>.tar.gz`: macOS arm64/x86-64, Linux x86-64/arm64) contains the program, SDL2 and the build tools (Python, zig) but no game code. Unpack it, copy your Phobia II folder into its `game/` folder and run `./phobia2.sh`: the first start translates your `Game.exe` and compiles it (~15 s, once), then the game starts. Making kits: [Building](docs/dev/building.md#release-kits).

## Build from source (macOS)
```sh
brew install sdl2 cmake
git clone https://github.com/segovia-no/Phobia-II-Recomp.git && cd Phobia-II-Recomp
cp -R /path/to/your/Phobia2 game      # folder with Game.exe, pics/, sounds/, dat/
./setup.sh                            # makes .venv, recompiles Game.exe, builds build/phobia2
./build/phobia2
```
Linux: install `build-essential cmake libsdl2-dev python3-venv` instead of the Homebrew packages; the rest is the same.
`Cmd+Ctrl+F` or `Alt+Enter` toggles fullscreen. More: **[Playing](docs/guides/playing.md)** · **[Troubleshooting](docs/guides/troubleshooting.md)**.

## Documentation
| | |
|---|---|
| [Building from source](docs/dev/building.md) | setup script, CMake by hand, debug builds and developer aids |
| [Architecture](docs/dev/architecture.md) | how the recompiler and runtime work, repository layout |
| [Technical notes](docs/README.md) | target analysis, translator, self-modifying mixer, runtime notes |

## Contributing
Bug reports and pull requests are welcome: [open an issue](https://github.com/segovia-no/Phobia-II-Recomp/issues/new/choose) or see **[CONTRIBUTING.md](CONTRIBUTING.md)** (above all: no game files in the repository).

## Distribution and legal
The recompiler, runtime and documentation are released under the [MIT License](LICENSE); the license does not cover Phobia II itself or anything generated from it. What the build produces — the generated C in `build/gen/`, the `phobia2` binary of a source build and the `gamecode` module a kit builds — contains code translated from your `Game.exe` and must not be redistributed.

This is an unofficial fan project for preservation and interoperability. It is not affiliated with or endorsed by WAH-Software. "Phobia II" is a trademark of its respective owners.
