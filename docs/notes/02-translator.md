# 02 — Translator

`recompiler/lift.py` turns `Game.exe` into C in one pass (~1 s, ~1 730 functions, ~88 000 instructions, ~500 000 lines of C).

## Discovery
Recursive descent from: the entry point, `extra_entries.txt`, direct call targets, relocated immediates that point into code (`push offset fn`, callbacks), and relocated code pointers stored outside decoded instructions (vtables, tables in DGROUP). Relocated *displacements* of memory operands into code are data references and are not followed. Jump tables: `jmp [disp + reg*4]` whose table entries are relocated code addresses.

Spurious entries (strings that happen to be pointed at) are harmless: they compile to dead functions; undecodable instructions become `rt_bad()`/`rt_unimpl()` calls that only fire if ever reached.

## Functions and control flow
A function is the set of instructions reachable from its entry through intra-procedural edges. A direct `jmp` to another entry is a tail call; shared code reached by jumps is duplicated into each function. Instructions are emitted in address order with `goto` labels only where needed.

## Flags
CF, ZF, SF, OF, PF are bytes in the CPU struct. A backward liveness pass per function computes which flags each instruction must produce; `ret`, tail calls and indirect exits keep all flags live, calls kill them. Most `add`/`mov`/`cmp` sequences produce only the one or two flags their `jcc` reads.

## x87
Eight doubles with a top-of-stack index; `fnstsw`/`sahf` comparisons go through a status word with C0/C2/C3; `fistp`/`frndint` honour the rounding-control bits of the control word (Watcom's float-to-int helper sets truncation with `fldcw`). 80-bit memory operands are converted.

## Gotchas found
- capstone drops the `F2` prefix on `F2 A5` (it is ambiguous with SSE2 `movsd`), so the repeat prefix is taken from the raw bytes.
- `push`/`pop` of a segment register move 4 bytes in 32-bit code.
- `call [esp+N]`: the target must be read before the return address is pushed.
- capstone lists `fxch st(i)` with two operands `(st(0), st(i))`, unlike other one-operand x87 forms.
