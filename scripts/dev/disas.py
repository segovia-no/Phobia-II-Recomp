#!/usr/bin/env python3
"""Disassemble Game.exe at a virtual address: scripts/dev/disas.py game/Game.exe 0x413ce0 [count]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1]); va = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i, ins in enumerate(md.disasm(img[va - base:va - base + n * 15], va)):
    if i >= n: break
    print(f'{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')
