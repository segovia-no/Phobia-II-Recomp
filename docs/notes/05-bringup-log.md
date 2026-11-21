# 05 — Bring-up log

Bugs met in order, with the symptom that exposed each.

| Symptom | Cause | Fix |
|---|---|---|
| "Corrupted data file error" on every TGA | `F2 A5` decoded by capstone as plain `movsd` (prefix dropped) → iostream `read` copied 4 bytes instead of 18 | repeat prefix from raw bytes |
| New thread crashed with misaligned stack | `push ds` emitted as a 2-byte push | segment push/pop move 4 bytes |
| Deadlock: main thread in `_beginthread`, worker in `EnterCriticalSection(0xf6f85589)` | `.bss` filled from file offset 0 (PE header bytes) instead of zeros → garbage semaphore table | zero-fill uninitialised sections |
| Worker called address `0x00000008` | `call [esp+0x14]` evaluated after pushing the return address | read target first |
| Worker called heap address `0x10811718` | mixer routines copied to heap and patched | SMC mode ([03](03-self-modifying-code.md)) |
| Crackling sound | DirectSound play/write cursors coarse (1024-frame chunks; write cursor guessed) and stderr spam slowing the mixer thread | cursor model from consumed position, 512-frame device buffer, GUI std output discarded |
| Light effects drawn as striped rectangles; ~44 000 "Overflow range error in pow" per run | capstone lists `fxch st(1)` as `(st(0), st(1))`; the lifter took the first operand, so `fxch` did nothing and the light-map code computed `pow(2, dx)` instead of `pow(dx, 2)` | use the last operand |
| Crash in the mixer thread after the high-score table (guest fault at `0x1ec` = `ebx*4` with an unpatched table base) | heap-copy recogniser cached address → routine; each screen's sound thread re-copies a mixer routine into the same heap buffer, so a different routine there ran with the old routine's layout | re-verify the cached match on every call, per-thread cache, longest match wins |
| Random crashes mid-game and after the high-score table (corrupt game objects, overwritten mixer copy, bad COM objects) | **original game bug**: the death "melt" effect grows int16 column offsets every frame without a cap; after ~2000 frames (e.g. waiting on name entry) they wrap to −32768 and the column copy writes up to 32k rows before the surface. Found with headless stress runs (`scripts/dev/stress.sh`) and guard pages around surfaces (`PHOBIA_GUARD_SURF=1`) | saturate the offset store (`recompiler/patches.py`); surfaces also get 64 rows of slack on each side for the game's harmless one-row edge accesses |
