# 06 — Roadmap

## Done (confirmed by play-testing on macOS, 2026-11-21)
- Boot, menus, title, gameplay in 8-bit and 16-bit, light effects, death effect, high-score entry.
- Sound effects and music clean (DirectSound cursor model, 512-frame device buffer).
- Long sessions stable: the original game's melt-effect overflow is patched (`recompiler/patches.py`);
  4 × 5-minute headless stress runs without a fault.
- Keyboard fire on Ctrl via `scripts/setup/set_keys.py`.

- Linux port (x86-64, arm64) and the builder / release kits ([07](07-builder.md)): host without game code,
  `recompiler/build_gamecode.py`, kits tested as a player gets them (bare Debian container, macOS 11 target).

## Next
- Play-test the Linux build on real hardware (desktop, Steam Deck desktop mode).
- CI (GitHub Actions): build kits on tags, attach to Releases; first-run UI inside the program.
- MIDI on Linux (the MCI sequencer path; unused by the default settings).
- Two-player mode and the in-game key setup menu: play-test.
- Performance: keep guest registers in C locals across instructions (every access goes through `c->` today).
- `.app` bundle with icon; data in `~/Library/Application Support/PhobiaIIRecomp`.
- Scaling options (integer / aspect), gamepad mapping for both players.
- Windows build of the runtime.
