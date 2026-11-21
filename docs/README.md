# Technical notes

| Note | |
|---|---|
| [01 — Target binary](notes/01-binary.md) | what Game.exe is: compiler, sections, imports, data formats |
| [02 — Translator](notes/02-translator.md) | how x86 becomes C; flags, x87, calls, indirect branches |
| [03 — Self-modifying mixer](notes/03-self-modifying-code.md) | the sound mixer that patches and copies its own code, and how it is lifted |
| [04 — Runtime](notes/04-runtime.md) | guest memory, threads, Win32 and DirectX shims |
| [05 — Bugs found while bringing it up](notes/05-bringup-log.md) | translator/runtime bugs and how each showed up |
| [06 — Roadmap](notes/06-roadmap.md) | open issues and next steps |
| [07 — Builder, Linux port, release kits](notes/07-builder.md) | host + game-code module, `build_gamecode.py`, kits |
