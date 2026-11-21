# 06 — Roadmap

## Done (confirmed by play-testing on macOS, 2026-11-21)
- Boot, menus, title, gameplay in 8-bit and 16-bit, light effects, death effect, high-score entry.
- Sound effects and music clean (DirectSound cursor model, 512-frame device buffer).
- Long sessions stable: the original game's melt-effect overflow is patched (`recompiler/patches.py`);
  4 × 5-minute headless stress runs without a fault.
- Keyboard fire on Ctrl via `scripts/setup/set_keys.py`.

## Next
- **Linux port** (x86-64 and arm64; Steam Deck): portable runtime, Docker-tested headless.
- **Builder / packaging** like MotoRacerRecomp: host program without game code + `gamecode` module built on
  the player's machine by `recompiler/build_gamecode.py` (bundled `zig cc` + standalone Python), per-platform
  release kits.
- MIDI on Linux (the MCI sequencer path; unused by the default settings).
- Two-player mode and the in-game key setup menu: play-test.
- Performance: keep guest registers in C locals across instructions (every access goes through `c->` today).
- `.app` bundle with icon; data in `~/Library/Application Support/PhobiaIIRecomp`.
- Scaling options (integer / aspect), gamepad mapping for both players.
- Windows build of the runtime.
