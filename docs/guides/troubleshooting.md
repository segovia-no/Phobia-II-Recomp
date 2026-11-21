# Troubleshooting

- **"Cannot find game data directory"**: put the game in `game/` next to the repository root or set `PHOBIA_DATA=/path/to/game`.
- **The program stops with `FATAL:`**: the terminal shows the reason, guest registers, guest stack and a host backtrace (`f_<address>` is the guest function). Please open an issue with that output.
- **`dispatch: no compiled entry at XXXXXXXX`**: add the address to `recompiler/extra_entries.txt`, rebuild, and report it.
- **No sound**: check the macOS output device; `PHOBIA_AUDIO_STATS=1` logs the sound buffers the game creates.
- **High scores/config not saved**: the game writes into its own folder (`dat/`); it must be writable.
