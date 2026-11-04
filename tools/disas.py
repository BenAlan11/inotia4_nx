#!/usr/bin/env python3
"""Disassemble one function out of an Android .so by exported symbol name.

    python disas.py <so> <symbol> [max-insns]

Handy for recovering calling conventions of internal engine functions: the
Android binaries keep their dynamic symbols, so a name plus a hundred
instructions is usually enough to see how many arguments a helper takes and
whether it returns a pointer.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def main():
    path, sym = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 80

    e = ELF(path)
    target = None
    for s in e.exports:
        if s['name'] == sym:
            target = s
            break
    if not target:
        print('symbol not found:', sym)
        return 1

    addr, size = target['value'], target['size']
    print('%s  vaddr=0x%x size=%d' % (sym, addr, size))

    # map vaddr -> file offset
    off = None
    for p in e.phdrs:
        if p['type'] == 1 and p['vaddr'] <= addr < p['vaddr'] + p['filesz']:
            off = p['offset'] + (addr - p['vaddr'])
            break
    if off is None:
        print('not in a loadable segment')
        return 1

    n = size if size else limit * 4
    code = e.data[off:off + n]

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    count = 0
    for ins in md.disasm(code, addr):
        print('  0x%06x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
        count += 1
        if count >= limit:
            break
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
