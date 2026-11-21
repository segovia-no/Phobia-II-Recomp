# 06 — Roadmap

- Confirm the audio is clean in long sessions (gameplay with many effects); compare against the original on Windows.
- Play through all menus, options (key setup, colour select), one- and two-player games, high-score entry.
- Performance: keep guest registers in C locals across instructions (now every access goes through `c->`), measure with Instruments.
- `.app` bundle with icon; game folder in `~/Library/Application Support/PhobiaIIRecomp`.
- Widescreen/integer scaling options, gamepad mapping for both players.
- Windows and Linux builds of the runtime (SDL2 already abstracts most of it; `AudioToolbox` MIDI needs a fallback such as SDL_mixer/FluidSynth).
