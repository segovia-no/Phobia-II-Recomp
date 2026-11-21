# Architecture

```
game/Game.exe ──► recompiler/lift.py ──► build/gen/*.c ──┐
                                                         ├─► clang ─► build/phobia2
                   runtime/*.c (Win32/DirectX on SDL2) ──┘
```

## Repository layout
| Path | |
|---|---|
| `recompiler/lift.py` | the static recompiler (Python, capstone + pefile) |
| `recompiler/build_gamecode.py` | builds the game-code module for a target (system cc or zig) |
| `recompiler/patches.py` | fixes for original-game bugs at single instructions |
| `recompiler/extra_entries.txt` | extra code entry points the analysis cannot find |
| `runtime/cpu.h` | guest CPU state, memory accessors, x87 macros used by generated code |
| `runtime/core.c` | guest address space, image loading, dispatcher, host thunks, x87 helpers, threads, faults |
| `runtime/kernel32.c` | files, memory, threads, events, critical sections, TLS, time |
| `runtime/user32.c` | SDL window, message queue, keyboard mapping, video presentation, GDI bits |
| `runtime/ddraw.c` | DirectDraw: surfaces in guest memory, blits, palettes, flips |
| `runtime/audio.c` | DirectSound buffers + waveOut stream mixed in an SDL audio callback |
| `runtime/dinput.c` | DirectInput keyboard |
| `runtime/winmm.c` | joystick (SDL GameController), MCI MIDI sequencer (AudioToolbox MusicPlayer) |
| `runtime/module.c` | finds the game code: linked in, or loads `gamecode.so`/`.dylib` and fills its import slots |
| `runtime/main.c` | host entry |
| `scripts/dev/` | e2e runner, headless stress runner, disassembler |
| `scripts/release/` | tool pins + fetcher, Linux/macOS host builds, kit packager |
| `scripts/setup/set_keys.py` | rebind keys in the game's key files |
| `docs/` | guides and notes |

## Execution model
- **Guest state**: one `CPU` struct per guest thread (8 GPRs, flags as bytes, x87 stack of doubles) and one flat 4 GiB guest address space reserved with `mmap` (`M[addr]`). The PE image sits at its preferred base `0x400000`; heap, stacks and COM objects are allocated in guest memory so guest pointers stay 32-bit. The first 64 KiB is unmapped to catch NULL dereferences.
- **Functions**: every code entry becomes `void f_<addr>(CPU *c)`. All state lives in `c` and `M`, so *any* address can be an entry; the dispatcher maps a guest address to its C function through a table indexed by address.
- **Calls** push the real guest return address and call the C function; `ret` pops it and returns. Direct `jmp` to another entry is a tail call. Indirect calls/jumps go through `dispatch()`; switch tables (`jmp [table+reg*4]`) become C `switch` statements.
- **Imports**: IAT slots are filled with magic addresses (`0xFFF00000+`); `call [IAT]` is emitted as a direct call to the shim `IMP_<DLL>_<Name>`. Shims are stdcall: they read arguments from the guest stack and pop them.
- **COM**: DirectX objects are guest-memory structs whose vtables hold magic thunk addresses mapped to host functions.
- **Callbacks** (window procedure, enumeration callbacks, thread entry) run guest code from the host with `call_guest()`.
- **Threads**: `CreateThread` starts a host pthread with its own `CPU`, guest stack and TIB (`fs:` base).
- **Self-modifying code**: see [notes/03](../notes/03-self-modifying-code.md).
