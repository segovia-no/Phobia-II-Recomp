#!/usr/bin/env python3
"""
Static recompiler: Win32 x86 PE (Watcom C/C++ 1998, Phobia II Game.exe) -> portable C.

Model
-----
* All guest state lives in a CPU struct (registers, flags, x87 stack) and a flat
  32-bit guest address space (M[]).  Generated C functions take `CPU *c`.
* Every discovered code entry (call targets, relocated code pointers, jump-table
  targets, program entry) becomes a C function `f_<addr>`.  Because all state is
  in guest memory/CPU, *any* address can be an entry, so the runtime dispatcher
  can enter any compiled address for indirect calls/jumps.
* `call` pushes the real guest return address and calls the C function;
  `ret` pops it and returns from the C function.  `jmp` to another entry is a
  tail call (`f(c); return;`).
* Flags are computed per-flag only where live (intra-function liveness;
  ret/tail exits conservatively keep all flags live; calls kill all flags).
* Imports: IAT slots hold magic addresses; `call [IAT]` is emitted as a direct
  call to the host shim when the slot is known.
"""
import sys, os, struct, collections, json
import pefile, capstone
from capstone import x86 as X

import argparse
_ap = argparse.ArgumentParser(description='Phobia II static recompiler: Game.exe -> C')
_ap.add_argument('exe', nargs='?', default='game/Game.exe')
_ap.add_argument('out', nargs='?', default='build/gen')
_ap.add_argument('--extra', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_entries.txt'))
_ap.add_argument('--files', type=int, default=16, help='number of generated code_NNN.c files')
_args = _ap.parse_args()
GAME, OUT, EXTRA, NFILES = _args.exe, _args.out, _args.extra, _args.files

pe = pefile.PE(GAME)
BASE = pe.OPTIONAL_HEADER.ImageBase
SIZE = pe.OPTIONAL_HEADER.SizeOfImage
img = bytearray(SIZE)
img[0:pe.OPTIONAL_HEADER.SizeOfHeaders] = pe.__data__[0:pe.OPTIONAL_HEADER.SizeOfHeaders]
sections = []
for s in pe.sections:
    va = s.VirtualAddress
    if s.PointerToRawData and not (s.Characteristics & 0x80):   # skip uninitialised (.bss) -> zero
        raw = pe.__data__[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        img[va:va + len(raw)] = raw
    sections.append((s.Name.rstrip(b'\0').decode(), BASE + va, max(s.Misc_VirtualSize, s.SizeOfRawData)))
CODE_LO = BASE + 0x1000
CODE_HI = CODE_LO + 0x48000

def in_code(a): return CODE_LO <= a < CODE_HI
def rd32(a): return struct.unpack_from('<I', img, a - BASE)[0]

# ---------------------------------------------------------------- relocations
relocs = set()
for b in pe.DIRECTORY_ENTRY_BASERELOC:
    for e in b.entries:
        if e.type == 3:
            relocs.add(BASE + e.rva)

# ---------------------------------------------------------------- imports
imports = {}   # iat slot addr -> (dll, name)
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for i in d.imports:
        imports[i.address] = (d.dll.decode().split('.')[0].upper(), i.name.decode())

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

# Self-modifying mixer routines: table of (start, end) pairs in DGROUP; routines are copied to heap and patched.
SMC_TABLE = (0x4511c8, 0x4518a8)
smc_routines = []
for r in sorted(relocs):
    if SMC_TABLE[0] <= r < SMC_TABLE[1]:
        pass
_pairs = {}
for r in sorted(relocs):
    if SMC_TABLE[0] <= r < SMC_TABLE[1] and (r + 4) in relocs:
        a0, a1 = struct.unpack_from('<II', img, r - BASE)
        if 0x447000 <= a0 < a1 <= 0x449000 and a1 - a0 < 0x400 and a0 not in _pairs:
            _pairs[a0] = a1
smc_routines = sorted(_pairs.items())
SMC_LO = min(a for a, _ in smc_routines) if smc_routines else 0
SMC_HI = max(b for _, b in smc_routines) if smc_routines else 0
def in_smc(a): return SMC_LO <= a < SMC_HI

insn_cache = {}
def decode(a):
    if a in insn_cache: return insn_cache[a]
    if not in_code(a):
        insn_cache[a] = None; return None
    off = a - BASE
    ins = next(md.disasm(bytes(img[off:off + 16]), a, 1), None)
    insn_cache[a] = ins
    return ins

COND_J = {'jo','jno','jb','jae','je','jne','jbe','ja','js','jns','jp','jnp','jl','jge','jle','jg','jecxz','jcxz','loop','loope','loopne'}

def jump_table(ins):
    """jmp dword ptr [disp + idx*4] with relocated entries -> list of targets"""
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM: return None
    m = op.mem
    if m.base != 0 or m.index == 0 or m.scale != 4: return None
    t = m.disp & 0xffffffff
    out = []
    while t in relocs and in_code(rd32(t)):
        out.append(rd32(t)); t += 4
        if len(out) > 1024: break
    return out or None

# ---------------------------------------------------------------- discovery
entries = set()
entry_point = BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint
entries.add(entry_point)
if os.path.exists(EXTRA):
    for line in open(EXTRA):
        line = line.split('#')[0].strip()
        if line: entries.add(int(line, 16))

# relocated code pointers stored in data (or in code outside decoded instructions -> filtered later)
data_ptr_cands = set()
for r in relocs:
    v = rd32(r)
    if in_code(v):
        data_ptr_cands.add((r, v))

bad_insn = set()
decoded = {}            # addr -> ins (all reached instructions)
insn_cover = {}         # byte addr -> insn start
jtables = {}            # jmp insn addr -> targets
imm_code_refs = set()

def explore(start):
    work = [start]
    while work:
        a = work.pop()
        while True:
            if a in decoded: break
            ins = decode(a)
            if ins is None:
                bad_insn.add(a); break
            decoded[a] = ins
            for k in range(ins.size): insn_cover.setdefault(a + k, a)
            mn = ins.mnemonic
            # immediates that are relocated code addresses -> candidate entries
            for k in range(ins.size - 3):
                if (a + k) in relocs:
                    v = rd32(a + k)
                    if in_code(v):
                        # displacement of a mem operand = data reference; immediates = pointers
                        is_mem_disp = any(o.type == X.X86_OP_MEM and (o.mem.disp & 0xffffffff) == v for o in ins.operands)
                        if not is_mem_disp:
                            imm_code_refs.add(v)
            nxt = a + ins.size
            if mn in ('ret', 'retf', 'hlt', 'int3', 'ljmp', 'iretd'):
                break
            if mn == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    work.append(op.imm & 0xffffffff)
                else:
                    jt = jump_table(ins)
                    if jt:
                        jtables[a] = jt
                        work.extend(jt)
                break
            if mn in COND_J:
                work.append(ins.operands[0].imm & 0xffffffff)
            if mn == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    entries.add(t); work.append(t)
            a = nxt

def discover():
    done = set()
    while True:
        todo = (entries | imm_code_refs) - done
        # data pointers: only those whose reloc lies outside decoded instructions
        for (r, v) in data_ptr_cands:
            if r not in insn_cover and v not in done:
                todo.add(v)
        todo -= done
        if not todo: break
        for e in sorted(todo):
            done.add(e)
            entries.add(e)
            explore(e)
discover()
for jt in jtables.values():
    pass
print(f'entries={len(entries)} insns={len(decoded)} jtables={len(jtables)} bad={len(bad_insn)}', file=sys.stderr)

# ---------------------------------------------------------------- per-function CFG
def succs(ins):
    """intra-procedural successors (excluding tail calls), and kind"""
    mn = ins.mnemonic; a = ins.address; nxt = a + ins.size
    if mn in ('ret', 'retf', 'hlt', 'int3', 'ljmp', 'iretd'): return []
    if mn == 'jmp':
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM: return [op.imm & 0xffffffff]
        if a in jtables: return list(jtables[a])
        return []
    if mn in COND_J: return [ins.operands[0].imm & 0xffffffff, nxt]
    return [nxt]

def func_body(entry):
    """collect instruction addresses of function starting at entry.
    jmp (direct) to another entry => tail call (not followed)."""
    body = set(); work = [entry]; labels = {entry}
    if in_smc(entry):
        for (r0, r1) in smc_routines:
            if r0 <= entry < r1:
                a = r0
                while a < r1 and a in decoded:
                    body.add(a); a += decoded[a].size
    
    while work:
        a = work.pop()
        while True:
            if a in body: break
            ins = decoded.get(a)
            if ins is None: body.add(a); break   # bad -> stub
            body.add(a)
            mn = ins.mnemonic
            ss = succs(ins)
            if mn == 'jmp' and ins.operands[0].type == X.X86_OP_IMM:
                t = ss[0]
                if t in entries and t != entry:
                    break       # tail call
                labels.add(t); work.append(t); break
            if mn == 'jmp':
                for t in ss: labels.add(t); work.append(t)
                break
            if not ss: break
            if mn in COND_J:
                labels.add(ss[0]); work.append(ss[0])
            a = a + ins.size
    return body, labels

# ---------------------------------------------------------------- flags model
ALLF = frozenset('czsop')
def regname(ins, r): return ins.reg_name(r)

COND_FLAGS = {
 'jo':'o','jno':'o','jb':'c','jae':'c','je':'z','jne':'z','jbe':'cz','ja':'cz','js':'s','jns':'s',
 'jp':'p','jnp':'p','jl':'so','jge':'so','jle':'zso','jg':'zso',
}
def base_cc(mn):
    for p in ('set', 'cmov'):
        if mn.startswith(p): return 'j' + mn[len(p):]
    return mn
CC_ALIASES = {'jc':'jb','jnc':'jae','jnae':'jb','jnb':'jae','jz':'je','jnz':'jne','jna':'jbe','jnbe':'ja',
              'jnge':'jl','jnl':'jge','jng':'jle','jnle':'jg','jpe':'jp','jpo':'jnp'}
def norm_cc(j): return CC_ALIASES.get(j, j)

def flags_rw(ins):
    mn = ins.mnemonic.replace('rep ', '').replace('repne ', '').replace('repe ', '')
    if mn in ('add','sub','cmp','neg','and','or','xor','test','sar','shl','shr','sal','imul','mul','scasb','scasw','scasd','cmpsb','cmpsw','cmpsd'):
        if mn in ('sar','shl','shr','sal'):
            # count may be zero -> flags unchanged; we treat as read+write when count not const
            op = ins.operands[1] if len(ins.operands) > 1 else None
            if op is not None and op.type == X.X86_OP_IMM and (op.imm & 31) != 0:
                return set(), set(ALLF)
            return set(ALLF), set(ALLF)
        return set(), set(ALLF)
    if mn in ('adc', 'sbb'): return {'c'}, set(ALLF)
    if mn in ('inc', 'dec'): return set(), set('zsop')
    if mn in ('rol', 'ror'):
        return {'c','o'}, {'c','o'}
    if mn in ('rcl','rcr'): return {'c','o'}, {'c','o'}
    if mn in ('div','idiv'): return set(), set(ALLF)
    if mn == 'sahf': return set(), set('czsp')
    if mn == 'lahf': return set('czsp'), set()
    if mn in ('pushfd','pushf'): return set(ALLF), set()
    if mn in ('popfd','popf'): return set(), set(ALLF)
    if mn in ('stc','clc','cmc'): return ({'c'} if mn=='cmc' else set()), {'c'}
    if mn in ('bt','bts','btr','btc'): return set(), {'c'}
    if mn in ('aas','aaa','daa','das','aam','aad'): return set(ALLF), set(ALLF)
    if mn == 'call': return set(), set(ALLF)
    j = norm_cc(base_cc(mn))
    if j in COND_FLAGS: return set(COND_FLAGS[j]), set()
    if mn in ('fcomi','fucomi','fcomip','fucomip'): return set(), set(ALLF)
    return set(), set()

def liveness(body, entry):
    """returns dict insn_addr -> live flags after insn"""
    live_after = {}
    live_in = {a: set() for a in body}
    order = sorted(body, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            ins = decoded.get(a)
            if ins is None:
                la = set(ALLF)
            else:
                mn = ins.mnemonic
                ss = succs(ins)
                la = set()
                exit_ = False
                if mn == 'jmp' and ins.operands[0].type == X.X86_OP_IMM and ss[0] not in body:
                    exit_ = True
                if mn == 'jmp' and ins.operands[0].type != X.X86_OP_IMM and a not in jtables:
                    exit_ = True
                if mn in ('ret','retf','int3','hlt','ljmp','iretd'):
                    exit_ = True
                if exit_: la = set(ALLF)
                for s in ss:
                    if s in body: la |= live_in[s]
                    elif mn != 'jmp': la |= set(ALLF)
                if a in jtables: la = set(ALLF)
                r, w = flags_rw(ins)
                li = (la - w) | r
            if ins is None: li = set(ALLF)
            if live_after.get(a) != la or live_in[a] != li:
                live_after[a] = la; live_in[a] = li; changed = True
    return live_after

# ---------------------------------------------------------------- C emission
R32 = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
R16 = {'ax':'eax','cx':'ecx','dx':'edx','bx':'ebx','sp':'esp','bp':'ebp','si':'esi','di':'edi'}
R8L = {'al':'eax','cl':'ecx','dl':'edx','bl':'ebx'}
R8H = {'ah':'eax','ch':'ecx','dh':'edx','bh':'ebx'}
TY = {1:'uint8_t', 2:'uint16_t', 4:'uint32_t', 8:'uint64_t'}
STY = {1:'int8_t', 2:'int16_t', 4:'int32_t', 8:'int64_t'}
SIGN = {1:'0x80u', 2:'0x8000u', 4:'0x80000000u'}
BITS = {1:8, 2:16, 4:32}

class Unsupported(Exception): pass

SEGV = {'cs':'0x1Bu','ds':'0x23u','es':'0x23u','ss':'0x23u','fs':'0x3Bu','gs':'0x0u'}
def reg_rd(n):
    if n in SEGV: return SEGV[n]
    if n in R32: return f'c->{n}'
    if n in R16: return f'(uint16_t)c->{R16[n]}'
    if n in R8L: return f'(uint8_t)c->{R8L[n]}'
    if n in R8H: return f'(uint8_t)(c->{R8H[n]} >> 8)'
    raise Unsupported('reg ' + n)

def reg_wr(n, v):
    if n in SEGV: return f'(void)({v}); /* segment load ignored */'
    if n in R32: return f'c->{n} = (uint32_t)({v});'
    if n in R16: r = R16[n]; return f'c->{r} = (c->{r} & 0xFFFF0000u) | (uint16_t)({v});'
    if n in R8L: r = R8L[n]; return f'c->{r} = (c->{r} & ~0xFFu) | (uint8_t)({v});'
    if n in R8H: r = R8H[n]; return f'c->{r} = (c->{r} & ~0xFF00u) | ((uint32_t)(uint8_t)({v}) << 8);'
    raise Unsupported('reg ' + n)

def smc_field(ins, off, size):
    sx = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}[size]
    return f'(uint32_t)(int32_t)({sx})RD{size*8}(0x{ins.address + off:x}u + D)'

def ea(ins, op):
    m = op.mem
    parts = []
    if in_smc(ins.address) and ins.disp_size:
        if m.base: parts.append(reg_rd(ins.reg_name(m.base)))
        if m.index:
            ix = reg_rd(ins.reg_name(m.index))
            parts.append(ix if m.scale == 1 else f'{ix}*{m.scale}')
        parts.append(smc_field(ins, ins.disp_offset, ins.disp_size))
        e = ' + '.join(parts)
        if m.segment and ins.reg_name(m.segment) == 'fs': e = f'c->fs_base + {e}'
        return f'(uint32_t)({e})'
    if m.base: parts.append(reg_rd(ins.reg_name(m.base)))
    if m.index:
        ix = reg_rd(ins.reg_name(m.index))
        parts.append(ix if m.scale == 1 else f'{ix}*{m.scale}')
    d = m.disp & 0xffffffff
    if d or not parts: parts.append(f'0x{d:x}u')
    e = ' + '.join(parts)
    if m.segment:
        sn = ins.reg_name(m.segment)
        if sn == 'fs': e = f'c->fs_base + {e}'
        elif sn == 'gs': e = f'c->gs_base + {e}'
        # cs/ds/es/ss: flat
    return f'(uint32_t)({e})'

def rd(ins, op, size=None):
    size = size or op.size
    if op.type == X.X86_OP_REG: return reg_rd(ins.reg_name(op.reg))
    if op.type == X.X86_OP_IMM:
        if in_smc(ins.address) and ins.imm_size and ins.mnemonic not in ('jmp', 'call') and not ins.mnemonic.startswith('j') and not ins.mnemonic.startswith('loop'):
            return f'({TY[size]})({smc_field(ins, ins.imm_offset, ins.imm_size)})'
        v = op.imm & ((1 << (8 * size)) - 1)
        return f'({TY[size]})0x{v:x}u'
    if op.type == X.X86_OP_MEM: return f'RD{BITS[size]}({ea(ins, op)})'
    raise Unsupported('op')

def wr(ins, op, v, size=None):
    size = size or op.size
    if op.type == X.X86_OP_REG: return reg_wr(ins.reg_name(op.reg), v)
    if op.type == X.X86_OP_MEM: return f'WR{BITS[size]}({ea(ins, op)}, {v});'
    raise Unsupported('wr op')

def cond_expr(j):
    j = norm_cc(j)
    return {
        'jo':'c->of','jno':'!c->of','jb':'c->cf','jae':'!c->cf','je':'c->zf','jne':'!c->zf',
        'jbe':'(c->cf|c->zf)','ja':'!(c->cf|c->zf)','js':'c->sf','jns':'!c->sf','jp':'c->pf','jnp':'!c->pf',
        'jl':'(c->sf!=c->of)','jge':'(c->sf==c->of)','jle':'(c->zf|(c->sf!=c->of))','jg':'(!c->zf&&(c->sf==c->of))',
    }[j]

def szf(live, r, sz):
    o = []
    if 'z' in live: o.append(f'c->zf = ({r}) == 0;')
    if 's' in live: o.append(f'c->sf = (({r}) & {SIGN[sz]}) != 0;')
    if 'p' in live: o.append(f'c->pf = PARITY({r});')
    return o

def fmem(ins, op):
    """x87 memory operand read as double"""
    a = ea(ins, op)
    mnem = ins.mnemonic
    if mnem.startswith('fi'):
        return {2: f'(double)(int16_t)RD16({a})', 4: f'(double)(int32_t)RD32({a})', 8: f'(double)(int64_t)RD64({a})'}[op.size]
    return {4: f'(double)RDF32({a})', 8: f'RDF64({a})', 10: f'RDF80({a})'}[op.size]

def fstore(ins, op, v):
    a = ea(ins, op)
    if ins.mnemonic.startswith('fi'):
        return {2: f'WR16({a}, (uint16_t)fpu_to_int16(c, {v}));', 4: f'WR32({a}, (uint32_t)fpu_to_int32(c, {v}));',
                8: f'WR64({a}, (uint64_t)fpu_to_int64(c, {v}));'}[op.size]
    return {4: f'WRF32({a}, {v});', 8: f'WRF64({a}, {v});', 10: f'WRF80({a}, {v});'}[op.size]

def st(i): return f'ST({i})'

def emit_insn(ins, live, body, entry, out, fname):
    a = ins.address; nxt = a + ins.size
    mn = ins.mnemonic; ops = ins.operands
    E = out.append
    prefix = ''
    for p in ('rep ', 'repne ', 'repe '):
        if mn.startswith(p): prefix = p.strip(); mn = mn[len(p):]
    STRS = ('movsb','movsw','movsd','stosb','stosw','stosd','lodsb','lodsw','lodsd','scasb','scasw','scasd','cmpsb','cmpsw','cmpsd')
    if mn in STRS and not prefix:
        # capstone drops F2/F3 on some string ops (e.g. F2 A5 'movsd'); use raw prefix bytes
        raw = None
        for bb in ins.bytes:
            if bb in (0xF2, 0xF3): raw = bb
            elif bb in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf0): continue
            else: break
        if raw == 0xF3: prefix = 'rep' if mn[:4] in ('movs','stos','lods') else 'repe'
        elif raw == 0xF2: prefix = 'rep' if mn[:4] in ('movs','stos','lods') else 'repne'
    if prefix == 'repne' and mn[:4] in ('movs','stos','lods'): prefix = 'rep'

    def alu_flags(kind, a_, b_, r_, sz, carry_in=None):
        o = []
        if 'c' in live:
            if kind == 'add': o.append(f'c->cf = ({r_}) < ({a_});' if carry_in is None else f'c->cf = (({r_}) < ({a_})) || (({r_}) == ({a_}) && {carry_in});')
            elif kind == 'sub': o.append(f'c->cf = ({a_}) < ({b_});' if carry_in is None else f'c->cf = (({a_}) < ({b_})) || (({a_}) == ({b_}) && {carry_in});')
            elif kind == 'logic': o.append('c->cf = 0;')
        if 'o' in live:
            if kind == 'add': o.append(f'c->of = ((({a_}) ^ ({r_})) & (({b_}) ^ ({r_})) & {SIGN[sz]}) != 0;')
            elif kind == 'sub': o.append(f'c->of = ((({a_}) ^ ({b_})) & (({a_}) ^ ({r_})) & {SIGN[sz]}) != 0;')
            elif kind == 'logic': o.append('c->of = 0;')
        o += szf(live, r_, sz)
        return o

    T = None
    if mn in ('mov', 'movsx', 'movzx', 'movsxd'):
        d, s = ops
        if mn == 'mov':
            E(wr(ins, d, rd(ins, s, d.size)))
        elif mn == 'movzx':
            E(wr(ins, d, f'(uint32_t){rd(ins, s)}'))
        else:
            E(wr(ins, d, f'(uint32_t)(int32_t)({STY[s.size]}){rd(ins, s)}'))
    elif mn == 'lea':
        d, s = ops
        E(wr(ins, d, ea(ins, s)))
    elif mn in ('add', 'sub', 'cmp', 'adc', 'sbb'):
        d, s = ops; sz = d.size; t = TY[sz]
        kind = 'add' if mn in ('add', 'adc') else 'sub'
        cin = 'c->cf' if mn in ('adc', 'sbb') else None
        opch = '+' if kind == 'add' else '-'
        E('{')
        E(f'{t} a_ = {rd(ins, d)}, b_ = {rd(ins, s, sz)};')
        if cin: E(f'uint8_t ci_ = c->cf; {t} r_ = ({t})(a_ {opch} b_ {opch} ci_);')
        else: E(f'{t} r_ = ({t})(a_ {opch} b_);')
        if mn != 'cmp': E(wr(ins, d, 'r_'))
        out.extend(alu_flags(kind, 'a_', 'b_', 'r_', sz, 'ci_' if cin else None))
        E('}')
    elif mn in ('and', 'or', 'xor', 'test'):
        d, s = ops; sz = d.size; t = TY[sz]
        opch = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
        if mn == 'xor' and s.type == X.X86_OP_REG and d.type == X.X86_OP_REG and s.reg == d.reg:
            E(wr(ins, d, '0'))
            E('{ ' + ' '.join(alu_flags('logic', '0', '0', '0', sz)) + ' }')
        else:
            E('{')
            E(f'{t} r_ = ({t})({rd(ins, d)} {opch} {rd(ins, s, sz)});')
            if mn != 'test': E(wr(ins, d, 'r_'))
            out.extend(alu_flags('logic', '0', '0', 'r_', sz))
            E('}')
    elif mn in ('inc', 'dec'):
        d = ops[0]; sz = d.size; t = TY[sz]
        E('{')
        E(f'{t} a_ = {rd(ins, d)}; {t} r_ = ({t})(a_ {"+" if mn == "inc" else "-"} 1);')
        E(wr(ins, d, 'r_'))
        if 'o' in live:
            E(f'c->of = r_ == {SIGN[sz]};' if mn == 'inc' else f'c->of = a_ == {SIGN[sz]};')
        out.extend(szf(live, 'r_', sz))
        E('}')
    elif mn == 'neg':
        d = ops[0]; sz = d.size; t = TY[sz]
        E('{')
        E(f'{t} a_ = {rd(ins, d)}; {t} r_ = ({t})(0 - a_);')
        E(wr(ins, d, 'r_'))
        if 'c' in live: E('c->cf = a_ != 0;')
        if 'o' in live: E(f'c->of = a_ == {SIGN[sz]};')
        out.extend(szf(live, 'r_', sz))
        E('}')
    elif mn == 'not':
        d = ops[0]; E(wr(ins, d, f'~{rd(ins, d)}'))
    elif mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
        d = ops[0]; sz = d.size; t = TY[sz]; nb = BITS[sz]
        cnt = rd(ins, ops[1], 1) if len(ops) > 1 else '1'
        E('{')
        E(f'unsigned n_ = ({cnt}) & 31u;')
        E(f'if (n_) {{ {t} a_ = {rd(ins, d)}; {t} r_;')
        if mn in ('shl', 'sal'):
            E(f'r_ = ({t})((uint64_t)a_ << n_);')
            if 'c' in live: E(f'c->cf = (n_ <= {nb}) ? ((a_ >> ({nb} - n_)) & 1) : 0;')
            if 'o' in live: E(f'c->of = ((r_ & {SIGN[sz]}) != 0) ^ ((n_ <= {nb}) ? ((a_ >> ({nb} - n_)) & 1) : 0);')
        elif mn == 'shr':
            E(f'r_ = ({t})((uint64_t)a_ >> n_);')
            if 'c' in live: E(f'c->cf = (n_ <= {nb}) ? ((a_ >> (n_ - 1)) & 1) : 0;')
            if 'o' in live: E(f'c->of = (a_ & {SIGN[sz]}) != 0;')
        elif mn == 'sar':
            E(f'r_ = ({t})(({STY[sz]})a_ >> (n_ < {nb} ? n_ : {nb - 1}));')
            if 'c' in live: E(f'c->cf = (({STY[sz]})a_ >> (n_ <= {nb} ? n_ - 1 : {nb - 1})) & 1;')
            if 'o' in live: E('c->of = 0;')
        elif mn == 'rol':
            E(f'unsigned k_ = n_ % {nb}; r_ = ({t})(k_ ? ((a_ << k_) | (a_ >> ({nb} - k_))) : a_);')
            if 'c' in live: E('c->cf = r_ & 1;')
            if 'o' in live: E(f'c->of = ((r_ & {SIGN[sz]}) != 0) ^ (r_ & 1);')
        elif mn == 'ror':
            E(f'unsigned k_ = n_ % {nb}; r_ = ({t})(k_ ? ((a_ >> k_) | (a_ << ({nb} - k_))) : a_);')
            if 'c' in live: E(f'c->cf = (r_ & {SIGN[sz]}) != 0;')
            if 'o' in live: E(f'c->of = ((r_ ^ (r_ << 1)) & {SIGN[sz]}) != 0;')
        elif mn in ('rcl', 'rcr'):
            E(f'r_ = a_; for (unsigned i_ = 0; i_ < n_ % {nb + 1}; i_++) {{')
            if mn == 'rcl':
                E(f'uint8_t nc_ = (r_ & {SIGN[sz]}) != 0; r_ = ({t})((r_ << 1) | c->cf); c->cf = nc_; }}')
            else:
                E(f'uint8_t nc_ = r_ & 1; r_ = ({t})((r_ >> 1) | ((uint32_t)c->cf << {nb - 1})); c->cf = nc_; }}')
        E(wr(ins, d, 'r_'))
        if mn in ('shl', 'sal', 'shr', 'sar'):
            out.extend(szf(live, 'r_', sz))
        E('} }')
    elif mn in ('shld', 'shrd'):
        d, s = ops[0], ops[1]
        cnt = rd(ins, ops[2], 1)
        E('{ unsigned n_ = (' + cnt + ') & 31u; if (n_) {')
        E(f'uint32_t a_ = {rd(ins, d)}, b_ = {rd(ins, s)}, r_;')
        if mn == 'shld':
            E('r_ = (a_ << n_) | (b_ >> (32 - n_)); c->cf = (a_ >> (32 - n_)) & 1;')
        else:
            E('r_ = (a_ >> n_) | (b_ << (32 - n_)); c->cf = (a_ >> (n_ - 1)) & 1;')
        E(wr(ins, d, 'r_'))
        out.extend(szf(live, 'r_', 4))
        E('} }')
    elif mn == 'imul':
        if len(ops) == 1:
            s = ops[0]; sz = s.size
            if sz == 4:
                E(f'{{ int64_t r_ = (int64_t)(int32_t)c->eax * (int64_t)(int32_t){rd(ins, s)}; c->eax = (uint32_t)r_; c->edx = (uint32_t)((uint64_t)r_ >> 32);')
                E('c->cf = c->of = r_ != (int64_t)(int32_t)r_; }')
            elif sz == 2:
                E(f'{{ int32_t r_ = (int32_t)(int16_t)c->eax * (int32_t)(int16_t){rd(ins, s)}; {reg_wr("ax", "r_")} {reg_wr("dx", "r_ >> 16")}')
                E('c->cf = c->of = r_ != (int32_t)(int16_t)r_; }')
            else:
                E(f'{{ int16_t r_ = (int16_t)((int8_t)c->eax * (int8_t){rd(ins, s)}); {reg_wr("ax", "r_")}')
                E('c->cf = c->of = r_ != (int16_t)(int8_t)r_; }')
        else:
            d = ops[0]; sz = d.size
            s1 = rd(ins, ops[1]) if len(ops) == 2 else rd(ins, ops[1])
            s2 = rd(ins, ops[0]) if len(ops) == 2 else rd(ins, ops[2], sz)
            E(f'{{ int64_t r_ = (int64_t)({STY[sz]}){s1} * (int64_t)({STY[sz]}){s2};')
            E(wr(ins, d, 'r_'))
            if 'c' in live or 'o' in live: E(f'c->cf = c->of = r_ != (int64_t)({STY[sz]})r_;')
            out.extend(szf(live, f'({TY[sz]})r_', sz))
            E('}')
    elif mn == 'mul':
        s = ops[0]; sz = s.size
        if sz == 4:
            E(f'{{ uint64_t r_ = (uint64_t)c->eax * (uint64_t){rd(ins, s)}; c->eax = (uint32_t)r_; c->edx = (uint32_t)(r_ >> 32); c->cf = c->of = c->edx != 0; }}')
        elif sz == 2:
            E(f'{{ uint32_t r_ = (uint32_t)(uint16_t)c->eax * {rd(ins, s)}; {reg_wr("ax", "r_")} {reg_wr("dx", "r_ >> 16")} c->cf = c->of = (r_ >> 16) != 0; }}')
        else:
            E(f'{{ uint16_t r_ = (uint16_t)((uint8_t)c->eax * {rd(ins, s)}); {reg_wr("ax", "r_")} c->cf = c->of = (r_ >> 8) != 0; }}')
    elif mn in ('div', 'idiv'):
        s = ops[0]; sz = s.size
        if sz == 4:
            if mn == 'div':
                E(f'{{ uint32_t d_ = {rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); uint64_t n_ = ((uint64_t)c->edx << 32) | c->eax; c->eax = (uint32_t)(n_ / d_); c->edx = (uint32_t)(n_ % d_); }}')
            else:
                E(f'{{ int32_t d_ = (int32_t){rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); int64_t n_ = (int64_t)(((uint64_t)c->edx << 32) | c->eax); c->eax = (uint32_t)(int32_t)(n_ / d_); c->edx = (uint32_t)(int32_t)(n_ % d_); }}')
        elif sz == 2:
            if mn == 'div':
                E(f'{{ uint32_t d_ = {rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); uint32_t n_ = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff); {reg_wr("ax", "n_ / d_")} {reg_wr("dx", "n_ % d_")} }}')
            else:
                E(f'{{ int32_t d_ = (int16_t){rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); int32_t n_ = (int32_t)(((c->edx & 0xffff) << 16) | (c->eax & 0xffff)); {reg_wr("ax", "n_ / d_")} {reg_wr("dx", "n_ % d_")} }}')
        else:
            if mn == 'div':
                E(f'{{ uint32_t d_ = {rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); uint32_t n_ = c->eax & 0xffff; {reg_wr("al", "n_ / d_")} {reg_wr("ah", "n_ % d_")} }}')
            else:
                E(f'{{ int32_t d_ = (int8_t){rd(ins, s)}; if (!d_) rt_divzero(c, 0x{a:x}); int32_t n_ = (int16_t)(c->eax & 0xffff); {reg_wr("al", "n_ / d_")} {reg_wr("ah", "n_ % d_")} }}')
    elif mn == 'cpuid':
        E('rt_cpuid(c);')
    elif mn == 'cdq':
        E('c->edx = ((int32_t)c->eax < 0) ? 0xFFFFFFFFu : 0;')
    elif mn == 'cwde':
        E('c->eax = (uint32_t)(int32_t)(int16_t)c->eax;')
    elif mn == 'cbw':
        E(reg_wr('ax', '(int16_t)(int8_t)c->eax'))
    elif mn == 'cwd':
        E(reg_wr('dx', '((int16_t)c->eax < 0) ? 0xFFFF : 0'))
    elif mn == 'xchg':
        d, s = ops
        if d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
            pass
        else:
            E(f'{{ {TY[d.size]} t_ = {rd(ins, d)}; {wr(ins, d, rd(ins, s))} {wr(ins, s, "t_")} }}')
    elif mn == 'bswap':
        d = ops[0]; E(wr(ins, d, f'__builtin_bswap32({rd(ins, d)})'))
    elif mn == 'push':
        s = ops[0]
        if s.type == X.X86_OP_REG and ins.reg_name(s.reg) in SEGV:
            E(f'{{ c->esp -= 4; WR32(c->esp, {SEGV[ins.reg_name(s.reg)]}); }}')
        elif s.size == 2:
            E(f'{{ uint16_t v_ = {rd(ins, s)}; c->esp -= 2; WR16(c->esp, v_); }}')
        else:
            v = rd(ins, s, 4)
            if s.type == X.X86_OP_IMM:
                v = f'0x{s.imm & 0xffffffff:x}u' if not in_smc(a) else smc_field(ins, ins.imm_offset, ins.imm_size)
            E(f'{{ uint32_t v_ = {v}; c->esp -= 4; WR32(c->esp, v_); }}')
    elif mn == 'pop':
        d = ops[0]
        if d.type == X.X86_OP_REG and ins.reg_name(d.reg) in SEGV:
            E('c->esp += 4; /* pop sreg ignored */')
        elif d.size == 2:
            E(f'{{ uint16_t v_ = RD16(c->esp); c->esp += 2; {wr(ins, d, "v_")} }}')
        else:
            # pop [esp+x] uses esp after increment
            E(f'{{ uint32_t v_ = RD32(c->esp); c->esp += 4; {wr(ins, d, "v_")} }}')
    elif mn == 'pushal':
        E('{ uint32_t t_ = c->esp; c->esp -= 32; WR32(c->esp+28,c->eax); WR32(c->esp+24,c->ecx); WR32(c->esp+20,c->edx); WR32(c->esp+16,c->ebx); WR32(c->esp+12,t_); WR32(c->esp+8,c->ebp); WR32(c->esp+4,c->esi); WR32(c->esp,c->edi); }')
    elif mn == 'popal':
        E('{ c->edi=RD32(c->esp); c->esi=RD32(c->esp+4); c->ebp=RD32(c->esp+8); c->ebx=RD32(c->esp+16); c->edx=RD32(c->esp+20); c->ecx=RD32(c->esp+24); c->eax=RD32(c->esp+28); c->esp += 32; }')
    elif mn in ('pushfd',):
        E('{ c->esp -= 4; WR32(c->esp, rt_getflags(c)); }')
    elif mn in ('popfd',):
        E('{ rt_setflags(c, RD32(c->esp)); c->esp += 4; }')
    elif mn == 'sahf':
        E('{ uint8_t h_ = (uint8_t)(c->eax >> 8); c->cf = h_ & 1; c->pf = (h_ >> 2) & 1; c->zf = (h_ >> 6) & 1; c->sf = (h_ >> 7) & 1; }')
    elif mn == 'lahf':
        E(reg_wr('ah', '(c->cf | (c->pf << 2) | (c->zf << 6) | (c->sf << 7) | 2)'))
    elif mn in ('clc', 'stc', 'cmc'):
        E({'clc': 'c->cf = 0;', 'stc': 'c->cf = 1;', 'cmc': 'c->cf ^= 1;'}[mn])
    elif mn in ('cld', 'std'):
        E('c->df = 0;' if mn == 'cld' else 'c->df = 1;')
    elif mn in ('cli', 'sti', 'nop', 'wait', 'fwait', 'fnop', 'fnclex', 'fclex'):
        pass
    elif mn.startswith('set'):
        j = norm_cc(base_cc(mn))
        E(wr(ins, ops[0], f'({cond_expr(j)}) ? 1 : 0'))
    elif mn.startswith('cmov'):
        j = norm_cc(base_cc(mn))
        E(f'if ({cond_expr(j)}) {{ {wr(ins, ops[0], rd(ins, ops[1]))} }}')
    elif mn in ('bt', 'bts', 'btr', 'btc'):
        d, s = ops
        if d.type == X.X86_OP_MEM and s.type == X.X86_OP_REG:
            E(f'{{ int32_t b_ = (int32_t){rd(ins, s)}; uint32_t ad_ = {ea(ins, d)} + (uint32_t)((b_ >> 5) * 4); uint32_t v_ = RD32(ad_); unsigned k_ = b_ & 31; c->cf = (v_ >> k_) & 1;')
            if mn != 'bt': E({'bts': 'v_ |= 1u << k_;', 'btr': 'v_ &= ~(1u << k_);', 'btc': 'v_ ^= 1u << k_;'}[mn] + ' WR32(ad_, v_);')
            E('}')
        else:
            nb = BITS[d.size]
            E(f'{{ {TY[d.size]} v_ = {rd(ins, d)}; unsigned k_ = {rd(ins, s, 1) if s.type == X.X86_OP_IMM else rd(ins, s)} & {nb - 1}; c->cf = (v_ >> k_) & 1;')
            if mn != 'bt': E({'bts': 'v_ |= 1u << k_;', 'btr': 'v_ &= ~(1u << k_);', 'btc': 'v_ ^= 1u << k_;'}[mn] + ' ' + wr(ins, d, 'v_'))
            E('}')
    elif mn in ('bsf', 'bsr'):
        d, s = ops
        E(f'{{ uint32_t v_ = {rd(ins, s)}; c->zf = v_ == 0; if (v_) {{ {wr(ins, d, "__builtin_ctz(v_)" if mn == "bsf" else "31 - __builtin_clz(v_)")} }} }}')
    # ---------------- string ops
    elif mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd', 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd') and not (mn == 'movsd' and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg).startswith('xmm')):
        sz = {'b': 1, 'w': 2, 'd': 4}[mn[-1]]
        kind = mn[:-1]
        nb = BITS[sz]; t = TY[sz]
        step = f'(c->df ? -{sz} : {sz})'
        if kind == 'movs':
            body_ = f'WR{nb}(c->edi, RD{nb}(c->esi)); c->esi += {step}; c->edi += {step};'
        elif kind == 'stos':
            body_ = f'WR{nb}(c->edi, ({t})c->eax); c->edi += {step};'
        elif kind == 'lods':
            body_ = reg_wr({1: 'al', 2: 'ax', 4: 'eax'}[sz], f'RD{nb}(c->esi)') + f' c->esi += {step};'
        elif kind == 'scas':
            body_ = f'{{ {t} a_ = ({t})c->eax, b_ = RD{nb}(c->edi), r_ = ({t})(a_ - b_); c->edi += {step}; c->cf = a_ < b_; c->zf = r_ == 0; c->sf = (r_ & {SIGN[sz]}) != 0; c->of = ((a_ ^ b_) & (a_ ^ r_) & {SIGN[sz]}) != 0; c->pf = PARITY(r_); }}'
        elif kind == 'cmps':
            body_ = f'{{ {t} a_ = RD{nb}(c->esi), b_ = RD{nb}(c->edi), r_ = ({t})(a_ - b_); c->esi += {step}; c->edi += {step}; c->cf = a_ < b_; c->zf = r_ == 0; c->sf = (r_ & {SIGN[sz]}) != 0; c->of = ((a_ ^ b_) & (a_ ^ r_) & {SIGN[sz]}) != 0; c->pf = PARITY(r_); }}'
        if not prefix:
            E(body_)
        elif kind in ('scas', 'cmps'):
            stop = 'if (!c->zf) break;' if prefix == 'repe' else 'if (c->zf) break;'
            E(f'while (c->ecx) {{ {body_} c->ecx--; {stop} }}')
        else:
            if kind == 'movs' and sz in (1, 4):
                E(f'if (!c->df) {{ rt_movs(c, {sz}); }} else while (c->ecx) {{ {body_} c->ecx--; }}')
            elif kind == 'stos' and sz in (1, 4):
                E(f'if (!c->df) {{ rt_stos(c, {sz}); }} else while (c->ecx) {{ {body_} c->ecx--; }}')
            else:
                E(f'while (c->ecx) {{ {body_} c->ecx--; }}')
    # ---------------- control flow
    elif in_smc(a) and (mn == 'jmp' or mn in COND_J) and ops[0].type == X.X86_OP_IMM:
        t = ops[0].imm & 0xffffffff
        T = f'(uint32_t)(0x{nxt:x}u + {smc_field(ins, ins.imm_offset, ins.imm_size)})'
        cases = ' '.join(f'case 0x{l:x}u: goto L_{l:x};' for l in sorted(body) if decoded.get(l) is not None)
        jump = f'{{ uint32_t T_ = {T}; if (T_ == 0x{t:x}u) goto L_{t:x}; switch (T_) {{ {cases} default: rt_bad(c, T_); }} }}'
        if mn == 'jmp': E(jump)
        else:
            if mn in ('jecxz', 'jcxz'): cnd = 'c->ecx == 0' if mn == 'jecxz' else '(uint16_t)c->ecx == 0'
            elif mn == 'loop': cnd = '--c->ecx != 0'
            elif mn == 'loope': cnd = '(--c->ecx != 0) && c->zf'
            elif mn == 'loopne': cnd = '(--c->ecx != 0) && !c->zf'
            else: cnd = cond_expr(mn)
            E(f'if ({cnd}) {jump}')
    elif mn == 'jmp':
        op = ops[0]
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xffffffff
            if t in body and not (t in entries and t != entry):
                E(f'goto L_{t:x};')
            else:
                E(f'f_{t:x}(c); return;')
        elif a in jtables:
            E(f'switch ({rd(ins, op)}) {{')
            for t in sorted(set(jtables[a])):
                E(f'case 0x{t:x}u: goto L_{t:x};')
            E(f'default: dispatch(c, {rd(ins, op)}); return; }}')
        else:
            if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in imports:
                dll, nm = imports[op.mem.disp & 0xffffffff]
                E(f'TRACE_IMP("{nm}"); IMP_{dll}_{nm}(c); return;')
            else:
                E(f'dispatch(c, {rd(ins, op)}); return;')
    elif mn in COND_J:
        t = ops[0].imm & 0xffffffff
        if mn in ('jecxz', 'jcxz'):
            cnd = 'c->ecx == 0' if mn == 'jecxz' else '(uint16_t)c->ecx == 0'
        elif mn == 'loop':
            cnd = '--c->ecx != 0'
        elif mn == 'loope':
            cnd = '(--c->ecx != 0) && c->zf'
        elif mn == 'loopne':
            cnd = '(--c->ecx != 0) && !c->zf'
        else:
            cnd = cond_expr(mn)
        E(f'if ({cnd}) goto L_{t:x};')
    elif mn == 'call':
        op = ops[0]
        chk = f'CHECK_RET(c, 0x{nxt:x}u);'
        push_ret = f'c->esp -= 4; WR32(c->esp, 0x{nxt:x}u);'
        if op.type == X.X86_OP_IMM or (op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in imports):
            E(push_ret)
        if op.type == X.X86_OP_IMM:
            E(f'f_{op.imm & 0xffffffff:x}(c); {chk}')
        elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in imports:
            dll, nm = imports[op.mem.disp & 0xffffffff]
            E(f'TRACE_IMP("{nm}"); IMP_{dll}_{nm}(c); {chk}')
        else:
            E(f'{{ uint32_t t_ = {rd(ins, op)}; {push_ret} dispatch(c, t_); }} {chk}')
    elif mn == 'ret':
        n = ops[0].imm if ops else 0
        E(f'RET_HOOK(c); c->esp += {4 + n}; return;')
    elif mn == 'int3':
        E(f'rt_trap(c, 0x{a:x}, "int3"); return;')
    elif mn == 'int':
        E(f'rt_int(c, 0x{a:x}, {ops[0].imm});')
    elif mn == 'hlt':
        E(f'rt_trap(c, 0x{a:x}, "hlt"); return;')
    # ---------------- x87
    elif mn.startswith('f'):
        emit_fpu(ins, mn, ops, out)
    else:
        raise Unsupported(mn)

def emit_fpu(ins, mn, ops, out):
    E = out.append
    def src():
        # returns expression for source operand (memory or st(i))
        if len(ops) == 0: return st(1)
        if len(ops) == 1:
            o = ops[0]
            if o.type == X.X86_OP_MEM: return fmem(ins, o)
            return st(stidx(o))
        return None
    def stidx(o):
        n = ins.reg_name(o.reg)
        return int(n[3:-1]) if n.startswith('st(') else 0
    if mn == 'fld':
        o = ops[0]
        if o.type == X.X86_OP_MEM: E(f'FPUSH({fmem(ins, o)});')
        else: E(f'{{ double v_ = {st(stidx(o))}; FPUSH(v_); }}')
    elif mn == 'fild':
        E(f'FPUSH({fmem(ins, ops[0])});')
    elif mn in ('fst', 'fstp'):
        o = ops[0]
        if o.type == X.X86_OP_MEM: E(fstore(ins, o, 'ST(0)'))
        else: E(f'{st(stidx(o))} = ST(0);')
        if mn == 'fstp': E('FPOP();')
    elif mn in ('fist', 'fistp'):
        E(fstore(ins, ops[0], 'ST(0)'))
        if mn == 'fistp': E('FPOP();')
    elif mn in ('fadd', 'fmul', 'fsub', 'fsubr', 'fdiv', 'fdivr', 'faddp', 'fmulp', 'fsubp', 'fsubrp', 'fdivp', 'fdivrp', 'fiadd', 'fimul', 'fisub', 'fisubr', 'fidiv', 'fidivr'):
        pop = mn.endswith('p') and mn not in ('fsubrp',) or mn == 'fsubrp'
        base = mn[1:].rstrip('p') if mn.endswith('p') else mn[1:]
        if base.startswith('i'): base = base[1:]
        opf = {'add': 'a_ + b_', 'mul': 'a_ * b_', 'sub': 'a_ - b_', 'subr': 'b_ - a_', 'div': 'a_ / b_', 'divr': 'b_ / a_'}[base]
        if len(ops) == 2:
            d = stidx(ops[0]); s = stidx(ops[1])
            E(f'{{ double a_ = {st(d)}, b_ = {st(s)}; {st(d)} = {opf}; }}')
        elif len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
            E(f'{{ double a_ = ST(0), b_ = {fmem(ins, ops[0])}; ST(0) = {opf}; }}')
        elif len(ops) == 1:
            s = stidx(ops[0])
            if mn.endswith('p'):
                E(f'{{ double a_ = {st(s)}, b_ = ST(0); {st(s)} = {opf}; }}')
            else:
                E(f'{{ double a_ = ST(0), b_ = {st(s)}; ST(0) = {opf}; }}')
        else:  # no operands: st(1) op= st(0), pop
            E(f'{{ double a_ = ST(1), b_ = ST(0); ST(1) = {opf}; }}')
        if mn.endswith('p'): E('FPOP();')
    elif mn == 'fxch':
        # capstone lists fxch as (st(0), st(i)); the exchanged register is the last operand
        s = stidx(ops[-1]) if ops else 1
        E(f'{{ double t_ = ST(0); ST(0) = {st(s)}; {st(s)} = t_; }}')
    elif mn == 'fchs': E('ST(0) = -ST(0);')
    elif mn == 'fabs': E('ST(0) = fabs(ST(0));')
    elif mn == 'fsqrt': E('ST(0) = sqrt(ST(0));')
    elif mn == 'fsin': E('ST(0) = sin(ST(0));')
    elif mn == 'fcos': E('ST(0) = cos(ST(0));')
    elif mn == 'fsincos': E('{ double v_ = ST(0); ST(0) = sin(v_); FPUSH(cos(v_)); }')
    elif mn == 'fptan': E('ST(0) = tan(ST(0)); FPUSH(1.0);')
    elif mn == 'fpatan': E('ST(1) = atan2(ST(1), ST(0)); FPOP();')
    elif mn == 'fyl2x': E('ST(1) = ST(1) * log2(ST(0)); FPOP();')
    elif mn == 'fyl2xp1': E('ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();')
    elif mn == 'f2xm1': E('ST(0) = exp2(ST(0)) - 1.0;')
    elif mn == 'fscale': E('ST(0) = ldexp(ST(0), (int)trunc(ST(1)));')
    elif mn == 'frndint': E('ST(0) = fpu_round(c, ST(0));')
    elif mn == 'fprem': E('{ double q_ = trunc(ST(0) / ST(1)); ST(0) = ST(0) - q_ * ST(1); c->fsw &= ~0x0400; }')
    elif mn == 'fprem1': E('ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x0400;')
    elif mn == 'fldz': E('FPUSH(0.0);')
    elif mn == 'fld1': E('FPUSH(1.0);')
    elif mn == 'fldpi': E('FPUSH(3.14159265358979323846);')
    elif mn == 'fldln2': E('FPUSH(0.69314718055994530942);')
    elif mn == 'fldl2e': E('FPUSH(1.44269504088896340736);')
    elif mn == 'fldl2t': E('FPUSH(3.32192809488736234787);')
    elif mn == 'fldlg2': E('FPUSH(0.30102999566398119521);')
    elif mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp'):
        s = src() if mn not in ('fcompp', 'fucompp') else st(1)
        E(f'fpu_compare(c, ST(0), {s});')
        if mn.endswith('pp'): E('FPOP(); FPOP();')
        elif mn.endswith('p'): E('FPOP();')
    elif mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip'):
        s = st(stidx(ops[1])) if len(ops) == 2 else st(stidx(ops[0]))
        E(f'{{ double a_ = ST(0), b_ = {s}; c->zf = a_ == b_ || a_ != a_ || b_ != b_; c->pf = a_ != a_ || b_ != b_; c->cf = a_ < b_ || c->pf; c->of = c->sf = 0; }}')
        if mn.endswith('p'): E('FPOP();')
    elif mn == 'ftst': E('fpu_compare(c, ST(0), 0.0);')
    elif mn == 'fxam': E('fpu_fxam(c);')
    elif mn in ('fnstsw', 'fstsw'):
        o = ops[0]
        E(wr(ins, o, 'fpu_status(c)', 2))
    elif mn in ('fnstcw', 'fstcw'):
        E(wr(ins, ops[0], 'c->fcw', 2))
    elif mn == 'fldcw':
        E(f'c->fcw = {rd(ins, ops[0], 2)};')
    elif mn in ('fninit', 'finit'):
        E('fpu_init(c);')
    elif mn in ('ffree',):
        pass
    elif mn in ('fincstp',): E('c->ftop = (c->ftop + 1) & 7;')
    elif mn in ('fdecstp',): E('c->ftop = (c->ftop - 1) & 7;')
    elif mn in ('fnsave', 'fsave'): E(f'fpu_save(c, {ea(ins, ops[0])});')
    elif mn in ('frstor',): E(f'fpu_restore(c, {ea(ins, ops[0])});')
    elif mn in ('fnstenv', 'fstenv', 'fldenv'): E(f'/* {mn} ignored */')
    else:
        raise Unsupported(mn)

# ---------------------------------------------------------------- generate
import re
GOTO_RE = re.compile(r'goto L_([0-9a-f]+);')
def pre_lines_fix(pre, chunks):
    # 'pre' gotos must come before the label of this chunk -> attach to previous chunk
    if pre:
        if chunks: chunks[-1][1].extend(pre)
        else: chunks.append((None, list(pre)))
    return []

os.makedirs(OUT, exist_ok=True)
unsupported = collections.Counter()
func_list = sorted(entries)
CHUNK = (len(func_list) + NFILES - 1) // NFILES
files = []
decls = []
for e in func_list:
    decls.append(f'void f_{e:x}(CPU *c);')

hdr = open(os.path.join(OUT, 'gen.h'), 'w')
hdr.write('#pragma once\n#include "cpu.h"\n')
hdr.write('\n'.join(decls) + '\n')
hdr.close()

for fi in range(NFILES):
    ci = fi * CHUNK
    fn = os.path.join(OUT, f'code_{fi:03d}.c')
    files.append(fn)
    with open(fn, 'w') as f:
        f.write('#include "gen.h"\n#include "imports.h"\n#include <math.h>\n\n')
        for e in func_list[ci:ci + CHUNK]:
            body, labels = func_body(e)
            live = liveness(body, e)
            f.write(f'void f_{e:x}(CPU *c) {{\n')
            if in_smc(e): f.write('  const uint32_t D = c->smc_delta; c->smc_delta = 0; (void)D;\n')
            elif any(in_smc(x) for x in body): f.write('  const uint32_t D = 0; (void)D;\n')
            if e not in decoded:
                f.write(f'  rt_bad(c, 0x{e:x}); return;\n}}\n\n')
                continue
            # emit in address order; entry first via goto if not lowest
            order = sorted(body)
            chunks = []   # list of (addr, [lines])
            if order[0] != e:
                chunks.append((None, [f'goto L_{e:x};']))
            prev_fall = None
            for idx, a in enumerate(order):
                ins = decoded.get(a)
                pre = []
                if prev_fall is not None and prev_fall != a:
                    pre.append(f'goto L_{prev_fall:x};')
                if ins is None:
                    chunks.append((a, pre + [f'rt_bad(c, 0x{a:x}); return;']))
                    prev_fall = None
                    continue
                lines = []
                try:
                    emit_insn(ins, live.get(a, ALLF), body, e, lines, f'f_{e:x}')
                except Unsupported as ex:
                    unsupported[str(ex)] += 1
                    lines = [f'rt_unimpl(c, 0x{a:x}, "{ins.mnemonic} {ins.op_str}"); return;']
                lines = [f'/* {a:08x}: {ins.mnemonic} {ins.op_str} */ TR(0x{a:x});'] + lines
                mn = ins.mnemonic
                if mn in ('ret', 'jmp', 'int3', 'hlt', 'ljmp', 'retf', 'iretd') or lines[-1].endswith('return;'):
                    prev_fall = None
                else:
                    prev_fall = a + ins.size
                    if prev_fall not in body:
                        lines.append(f'f_{prev_fall:x}(c); return;' if prev_fall in entries else f'rt_bad(c, 0x{prev_fall:x}); return;')
                        prev_fall = None
                chunks.append((a, pre_lines_fix(pre, chunks) + lines))
            if prev_fall is not None:
                chunks.append((None, [f'goto L_{prev_fall:x};']))
            text = '\n'.join(l for _, ls in chunks for l in ls)
            needed = set(int(x, 16) for x in GOTO_RE.findall(text))
            for a, ls in chunks:
                if a is not None and a in needed:
                    f.write(f'L_{a:x}:;\n')
                for l in ls: f.write('  ' + l + '\n')
            f.write('}\n\n')

# function table
with open(os.path.join(OUT, 'functable.c'), 'w') as f:
    f.write('#include "gen.h"\n#include "runtime.h"\nconst FuncEntry g_functable[] = {\n')
    for e in func_list: f.write(f'  {{0x{e:x}u, f_{e:x}}},\n')
    f.write('};\nconst unsigned g_functable_count = %d;\n' % len(func_list))

# SMC routine table: masks of fixed opcode bytes
with open(os.path.join(OUT, 'smctable.c'), 'w') as f:
    f.write('#include "gen.h"\n#include "runtime.h"\n')
    for (r0, r1) in smc_routines:
        mask = bytearray(r1 - r0)
        a = r0
        while a < r1 and a in decoded:
            ins = decoded[a]
            for k in range(ins.size):
                if a + k - r0 < len(mask): mask[a + k - r0] = 1
            for (o, sz) in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
                if sz:
                    for k in range(sz):
                        if a + o + k - r0 < len(mask): mask[a + o + k - r0] = 0
            a += ins.size
        f.write(f'static const unsigned char mask_{r0:x}[] = {{' + ','.join(str(x) for x in mask) + '};\n')
    f.write('const SmcRoutine g_smc[] = {\n')
    for (r0, r1) in smc_routines:
        f.write(f'  {{0x{r0:x}u, 0x{r1:x}u, mask_{r0:x}, f_{r0:x}}},\n')
    f.write('};\nconst unsigned g_smc_count = %d;\n' % len(smc_routines))

# imports header + list
imp_sorted = sorted(imports.items())
with open(os.path.join(OUT, 'imports.h'), 'w') as f:
    f.write('#pragma once\n#include "cpu.h"\n')
    for slot, (dll, nm) in imp_sorted:
        f.write(f'void IMP_{dll}_{nm}(CPU *c);\n')
with open(os.path.join(OUT, 'importtable.c'), 'w') as f:
    f.write('#include "imports.h"\n#include "runtime.h"\nconst ImportEntry g_imports[] = {\n')
    for slot, (dll, nm) in imp_sorted:
        f.write(f'  {{0x{slot:x}u, "{dll}", "{nm}", IMP_{dll}_{nm}}},\n')
    f.write('};\nconst unsigned g_imports_count = %d;\n' % len(imp_sorted))

with open(os.path.join(OUT, 'imports_weak.c'), 'w') as f:
    f.write('#include "imports.h"\n#include "runtime.h"\n')
    for slot, (dll, nm) in imp_sorted:
        f.write(f'__attribute__((weak)) void IMP_{dll}_{nm}(CPU *c) {{ rt_missing_import(c, "{dll}", "{nm}"); }}\n')
# image blob
with open(os.path.join(OUT, 'image.c'), 'w') as f:
    f.write('// PE image of Game.exe mapped at IMAGE_BASE (data + code bytes for data references)\n')
    f.write('const unsigned g_image_size = %d;\nconst unsigned char g_image[] = {\n' % len(img))
    b = bytes(img)
    for i in range(0, len(b), 32):
        f.write(','.join(str(x) for x in b[i:i+32]) + ',\n')
    f.write('};\n')
with open(os.path.join(OUT, 'meta.h'), 'w') as f:
    f.write(f'#pragma once\n#define IMAGE_BASE 0x{BASE:x}u\n#define IMAGE_SIZE 0x{SIZE:x}u\n#define ENTRY_POINT 0x{entry_point:x}u\n')
    f.write(f'#define CODE_LO 0x{CODE_LO:x}u\n#define CODE_HI 0x{CODE_HI:x}u\n')
    f.write(f'#define STACK_RESERVE 0x{pe.OPTIONAL_HEADER.SizeOfStackReserve:x}u\n')

print('unsupported:', dict(unsupported), file=sys.stderr)
print(f'functions={len(func_list)} files={len(files)}', file=sys.stderr)
