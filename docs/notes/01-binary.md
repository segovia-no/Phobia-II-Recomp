# 01 — Target binary

`Game.exe`, 357 888 bytes, linked 1998-09-01. PE32 GUI, i386, image base `0x400000`, entry `0x4263bc`.

| Section | VA | Size | Notes |
|---|---|---|---|
| `AUTO` | 0x401000 | 0x48000 | code (+ some constants); Watcom naming |
| `.idata` | 0x449000 | 0x1000 | imports |
| `DGROUP` | 0x44a000 | 0xa000 | data, C++ vtables, the mixer routine table |
| `.bss` | 0x454000 | 0x2000 | uninitialised — **PointerToRawData is 0; must be zero-filled, not copied from file offset 0** |
| `.reloc` | 0x456000 | 0x5000 | 8 422 HIGHLOW relocations — every absolute address is known |
| `.rsrc` | 0x45b000 | 0x1000 | icon only |

Compiler: Watcom C/C++32 (runtime string "WATCOM C/C++32 Run-Time system ... 1988-1995"). Consequences: register calling convention (eax, edx, ebx, ecx), C++ iostreams for file loading, Watcom CRT semaphores on Win32 critical sections, `F2`-prefixed string moves (`repne movsd` used as `rep movsd`).

Imports: KERNEL32 (86), USER32 (19), GDI32 (9), WINMM (11: joystick, MCI, waveOut), DDRAW `DirectDrawCreate`, DSOUND `DirectSoundCreate`, DINPUT `DirectInputCreateA`. The game also `LoadLibrary`/`GetProcAddress`es DDRAW/DINPUT/DSOUND (DirectX SDK version-detection code) and queries `IDirectDraw2`.

## Game data
| Files | Format |
|---|---|
| `pics/*.pl`, `*.bl`, `*.lib` | Truevision TGA, type 10 (RLE true colour), 24 bpp; loaded with iostreams, colour-matched to 8-bit or converted to 16-bit |
| `sounds/*.sl` | sound samples (8/16-bit PCM in DirectSound buffers) |
| `sounds/*.it` | Impulse Tracker modules, played by the game's own software mixer |
| `sounds/*.mid` | MIDI, played through MCI (`open %s type sequencer alias MidiMusic`, `play MidiMusic from 0 notify`) |
| `dat/*.dat` | text, key bindings, config, high scores |

Display: asks for 640×480 at 8 or 16 bpp (menu "Select Color-Bit Depth"); 8-bit mode uses a DirectDraw palette.
