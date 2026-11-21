# Contributing

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting a problem or an idea
Open an issue and pick **Bug report** or **Feature request**. For bugs: the commit, your platform and what happened; screenshots and, for crashes, the terminal output (FATAL lines carry the guest registers and a host backtrace with `f_<address>` names) help a lot. Never attach game files (`Game.exe`, `pics/`, `sounds/`, `dat/`) or anything built from them.

## Pull requests
1. Fork and clone, put your game in `game/`, run `./setup.sh` ([docs/dev/building.md](docs/dev/building.md)). How the pieces fit: [docs/dev/architecture.md](docs/dev/architecture.md).
2. Make your change on a branch, test it (below), open a pull request against `main`.

Small, focused pull requests are easiest to review. For larger changes open an issue first.

### Testing
- Play the part you changed; say where you tested.
- Scripted runs with injected keys and frame dumps: `scripts/dev/e2e.sh OUTDIR [seconds] [keys]`.
- Translator changes: build the trace variant (`-DPHOBIA_RECOMP_TRACE=ON`) and compare register traces around the code you touched (`PHOBIA_TR=lo-hi`); `-DPHOBIA_RECOMP_CHECK=ON` verifies every guest return address.

### Rules
- **No game content in the repository**: no game files, no generated C, no binaries made from `Game.exe`.
- Runtime shims follow the Win32/DirectX contracts the game relies on; comments explain *why*.
- Update the docs when behaviour, settings or build steps change.

By contributing you agree that your contributions are released under the project's [MIT License](LICENSE).
