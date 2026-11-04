#!/usr/bin/env python3
"""Find the callers of an imported function in libgame.so by resolving its PLT
stub (via JMP_SLOT relocations) and scanning .text for BLs to that stub.

    python tools/find_callers.py data/libgame.so TouchHandle_Event
"""
import sys
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

so_path, want = sys.argv[1], sys.argv[2]
d = open(so_path, 'rb').read()

e_shoff, = struct.unpack_from('<Q', d, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
sections = []
for i in range(e_shnum):
    so = e_shoff + i * e_shentsize
    nameoff, typ, flags, addr, offset, size, link = struct.unpack_from('<IIQQQQI', d, so)
    sections.append((nameoff, typ, addr, offset, size, link))
shstr = sections[e_shstrndx][3]

def shname(no):
    end = d.index(b'\0', shstr + no)
    return d[shstr + no:end].decode()

sec = {shname(n): (a, o, s) for n, t, a, o, s, l in sections}
dynsym = sec['.dynsym']
dynstr = sec['.dynstr']

def symname(idx):
    _, off, size = dynsym
    _, doff, dsize = dynstr
    nameoff = struct.unpack_from('<I', d, off + idx * 24)[0]
    end = d.index(b'\0', doff + nameoff)
    return d[doff + nameoff:end].decode()

# JMP_SLOT relocations: got address -> symbol name
relsec = sec.get('.rela.plt')
got2sym = {}
if relsec:
    roff, rsize, rlink = relsec
    for i in range(rsize // 24):
        r_off, r_info, r_addend = struct.unpack_from('<QQq', d, roff + i * 24)
        got2sym[r_off] = symname(r_info >> 32)

cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
pa, po, ps = sec['.plt']
stub2sym = {}
insns = list(cs.disasm(d[po:po + ps], pa))
for j in range(0, len(insns) - 3, 4):
    base = imm = None
    for ins in insns[j:j + 4]:
        if ins.mnemonic == 'adrp':
            base = ins.operands[1].imm
        if ins.mnemonic == 'ldr' and base is not None:
            try:
                imm = ins.operands[1].mem.disp
            except Exception:
                pass
    if base is not None and imm is not None and base + imm in got2sym:
        stub2sym[pa + j * 4] = got2sym[base + imm]

stub = None
for a, n in stub2sym.items():
    if n == want:
        stub = a
        break
print('%s@plt = %s' % (want, hex(stub) if stub else 'NOT FOUND'))
if stub is None:
    sys.exit(1)

ta, to, ts = sec['.text']
callers = []
for ins in cs.disasm(d[to:to + ts], ta):
    if ins.mnemonic == 'bl' and ins.operands[0].imm == stub:
        callers.append(ins.address)
print('callers (%d):' % len(callers))
for c in callers:
    print('  %x' % c)
