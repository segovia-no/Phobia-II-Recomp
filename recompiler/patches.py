"""Fixes for bugs in the original game, applied at single guest instructions.

Each entry replaces the C emitted for one instruction. Keep them minimal and explain the original bug;
the translation stays faithful everywhere else.
"""

PATCHES = {
    # Death "melt" effect (f_408de0): every frame each column offset grows by rand()%(n/2)+n/2 and is
    # stored as int16 with no cap. Offsets >= height are skipped when drawing, so growth is harmless
    # until ~2000 frames later (e.g. waiting on the high-score name entry) the value wraps to -32768 and
    # the column copy (f_409310 / f_4091d0) writes up to 32k rows *before* the surface, trashing the heap.
    # Saturate instead of wrapping: identical picture, no wrap.
    0x408e36: 'WR16(c->ecx, (uint16_t)((int32_t)c->edx > 0x7fff ? 0x7fff : (int32_t)c->edx));',
}
