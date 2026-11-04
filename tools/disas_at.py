#!/usr/bin/env python3
"""Disassemble libgame.so around an arbitrary virtual address.

    python tools/disas_at.py data/libgame.so 0x255714 [before] [after]

Prints the nearest exported symbol and a window of instructions, with BL
targets annotated by the nearest export. Handy when a log line names a raw
return address (caller 0xe179a0714 -> base 0xe1774b000 -> 0x255714).
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def vaddr_to_off(e, addr):
    for p in e.phdrs:
        if p['type'] == 1 and p['vaddr'] <= addr < p['vaddr'] + p['filesz']:
            return p['offset'] + (addr - p['vaddr'])
    return None


def nearest(e, addr):
    best = None
    for s in e.exports:
        if s['value'] <= addr and (best is None or s['value'] > best['value']):
            best = s
    return best


def main():
    path = sys.argv[1]
    want = int(sys.argv[2], 0)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 80
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 160

    e = ELF(path)
    off = vaddr_to_off(e, want)
    if off is None:
        print('not in a loadable segment')
        return 1

    sym = nearest(e, want)
    if sym:
        print('nearest export: %s + 0x%x (size %d)' %
              (sym['name'], want - sym['value'], sym['size']))

    start = want - before * 4
    start_off = vaddr_to_off(e, start)
    data = e.data[start_off:start_off + (before + after) * 4]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for ins in md.disasm(data, start):
        note = ''
        if ins.mnemonic in ('bl', 'b', 'b.eq', 'b.ne', 'b.lt', 'b.gt', 'cbz', 'cbnz'):
            try:
                tgt = ins.operands[-1].imm if hasattr(ins.operands[-1], 'imm') else None
            except Exception:
                tgt = None
            if tgt is not None:
                s = nearest(e, tgt)
                if s:
                    note = '  ; %s+0x%x' % (s['name'], tgt - s['value'])
        print('  0x%06x  %-8s %-40s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
