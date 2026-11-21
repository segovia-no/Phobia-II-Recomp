# Phobia II Recomp

![Platforms](https://img.shields.io/badge/platforms-macOS-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Play **Phobia II** (WAH-Software, 1998), a top-down alien shooter for Windows 95, natively on modern Macs.

This is a *static recompilation*: the original Windows executable (`Game.exe`, Watcom C/C++ 32-bit) is translated to C ahead of time and compiled into a native program, linked against a small runtime that re-implements the Windows, DirectDraw, DirectSound, DirectInput and WinMM parts the game uses on top of SDL2. No emulator, no Wine.

> **You need your own copy of the game.** This repository contains no game code or assets. Everything game-specific (the generated C and the binary) is made from your `Game.exe` on your own computer.

## Status
| Area | State |
|---|---|
| Boot, Watcom C runtime, WinMain, threads | working |
| Color-depth menu, title screen, gameplay rendering (8-bit palette and 16-bit) | working |
| Keyboard (DirectInput + window messages) | working |
| Sound effects and music mixer (DirectSound streaming, self-modifying mixer) | working, being tuned |
| MIDI music (MCI sequencer) | via macOS AudioToolbox |
| Gamepad (WinMM joystick) | via SDL GameController |

## Quick start (macOS)
```sh
brew install sdl2 cmake
git clone <this repo> PhobiaIIRecomp && cd PhobiaIIRecomp
cp -R /path/to/your/Phobia2 game      # folder with Game.exe, pics/, sounds/, dat/
./setup.sh                            # makes .venv, recompiles Game.exe, builds build/phobia2
./build/phobia2
```
`Cmd+Ctrl+F` or `Alt+Enter` toggles fullscreen. More: **[Playing](docs/guides/playing.md)** · **[Troubleshooting](docs/guides/troubleshooting.md)**.

## Documentation
| | |
|---|---|
| [Building from source](docs/dev/building.md) | setup script, CMake by hand, debug builds and developer aids |
| [Architecture](docs/dev/architecture.md) | how the recompiler and runtime work, repository layout |
| [Technical notes](docs/README.md) | target analysis, translator, self-modifying mixer, runtime notes |

## Contributing
Bug reports and pull requests are welcome: see **[CONTRIBUTING.md](CONTRIBUTING.md)** (above all: no game files in the repository).

## Distribution and legal
The recompiler, runtime and documentation are released under the [MIT License](LICENSE); the license does not cover Phobia II itself or anything generated from it. What the build produces — the generated C in `build/gen/` and the `phobia2` binary — contains code translated from your `Game.exe` and must not be redistributed.

This is an unofficial fan project for preservation and interoperability. It is not affiliated with or endorsed by WAH-Software. "Phobia II" is a trademark of its respective owners.
