#!/usr/bin/env python3
"""Find callers of an exported (internal) function through its local PLT thunk.

Exported engine functions are called via a thunk that loads a GOT slot filled by
a RELATIVE relocation, not a JMP_SLOT import relocation.  So:

    export addr -> RELATIVE relocation addend -> GOT slot
                -> scan .text for "adrp/ldr x17 ; add x16 ; br x17"
                -> scan .text for BL to that thunk

    python tools/find_callers_sym.py data/libgame.so jRequestRender
    python tools/find_callers_sym.py data/libgame.so 0x255524   # or raw vaddr
"""
import sys
import os
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def sec_by_name(e, name):
    for sh in e.shdrs:
        if e.sec_name(sh) == name:
            return sh
    return None


def main():
    e = ELF(sys.argv[1])
    arg = sys.argv[2]
    if arg.startswith('0x'):
        target = int(arg, 16)
        tname = arg
    else:
        hits = [s for s in e.exports if s['name'] == arg]
        if not hits:
            print('no such export:', arg)
            return 1
        target = hits[0]['value']
        tname = arg
    print('%s = 0x%x' % (tname, target))

    # Slots that end up holding the function address: a RELATIVE relocation
    # whose addend is the target, or a GLOB_DAT/JUMP_SLOT naming the export.
    dynsym = sec_by_name(e, '.dynsym')
    dynstr = e.shdrs[dynsym['link']]

    def sym_value(idx):
        return struct.unpack_from('<Q', e.data, dynsym['offset'] + idx * 24 + 8)[0]

    slots = set()
    for name in ('.rela.dyn', '.rela.plt'):
        s = sec_by_name(e, name)
        if not s:
            continue
        for i in range(s['size'] // 24):
            off, info, addend = struct.unpack_from(
                '<QQq', e.data, s['offset'] + i * 24)
            typ = info & 0xffffffff
            if typ == 1027 and addend == target:
                slots.add(off)
            elif typ in (1026, 1025) and sym_value(info >> 32) == target:
                slots.add(off)
    if not slots:
        print('no RELATIVE relocation points at it')
    print('got slots:', ', '.join('0x%x' % s for s in sorted(slots)))

    text = sec_by_name(e, '.text')
    plt = sec_by_name(e, '.plt')
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(e.data[text['offset']:text['offset'] + text['size']],
                           text['addr']))
    thunks = set()
    # stubs live in .plt, callers in .text
    for sec in (plt, text):
        if not sec:
            continue
        stub_ins = list(md.disasm(e.data[sec['offset']:sec['offset'] + sec['size']],
                                  sec['addr'])) if sec is not text else insns
        for i, ins in enumerate(stub_ins):
            if ins.mnemonic != 'adrp' or i + 3 >= len(stub_ins):
                continue
            page = ins.operands[1].imm
            ldr, add, br = stub_ins[i + 1], stub_ins[i + 2], stub_ins[i + 3]
            if ldr.mnemonic != 'ldr' or add.mnemonic != 'add' or br.mnemonic != 'br':
                continue
            try:
                slot = page + ldr.operands[1].mem.disp
            except Exception:
                continue
            if slot in slots:
                thunks.add(ins.address)
    print('thunks:', ', '.join('0x%x' % t for t in sorted(thunks)))

    callers = []
    for ins in insns:
        if ins.mnemonic in ('bl', 'b') and ins.operands:
            try:
                t = ins.operands[-1].imm
            except Exception:
                continue
            if t in thunks:
                callers.append((ins.address, t))
    print('callers (%d):' % len(callers))
    for a, t in callers:
        print('  0x%x -> thunk 0x%x' % (a, t))
    return 0


if __name__ == '__main__':
    sys.exit(main())
