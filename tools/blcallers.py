#!/usr/bin/env python3
"""Find BL call sites that target one address inside an ELF .so.

    python blcallers.py <so> <hex-target> [before] [after]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def main():
    path = sys.argv[1]
    target = int(sys.argv[2], 0)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    e = ELF(path)
    d = e.data
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    hits = []
    for p in e.phdrs:
        if p['type'] != 1 or not (p['flags'] & 1):
            continue
        start = p['vaddr']
        blob = d[p['offset']:p['offset'] + p['filesz']]
        for ins in md.disasm(blob, start):
            if ins.mnemonic == 'bl' and ins.op_str.startswith('#'):
                try:
                    dst = int(ins.op_str[1:], 16)
                except ValueError:
                    continue
                if dst == target:
                    hits.append(ins.address)
    print('target 0x%x: %d BL call site(s)' % (target, len(hits)))
    for h in hits:
        print('---- call at 0x%x' % h)
        off = None
        for p in e.phdrs:
            if p['type'] == 1 and p['vaddr'] <= h < p['vaddr'] + p['filesz']:
                off = h - p['vaddr'] + p['offset']
                break
        base = h - before * 4
        blob = d[off - before * 4: off + after * 4]
        for ins in md.disasm(blob, base):
            mark = '  <== ' if ins.address == h else '       '
            print('  0x%x%s%s %s' % (ins.address, mark, ins.mnemonic, ins.op_str))


if __name__ == '__main__':
    main()
