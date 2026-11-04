#!/usr/bin/env python3
"""Find BL/B callers of an arbitrary virtual address in an AArch64 ELF.

    python tools/find_callers_at.py data/libgame.so 0x25569c
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def nearest(e, addr):
    best = None
    for s in e.exports:
        if s['value'] <= addr and (best is None or s['value'] > best['value']):
            best = s
    return best


def main():
    e = ELF(sys.argv[1])
    target = int(sys.argv[2], 0)
    text = None
    for sh in e.shdrs:
        if e.sec_name(sh) == '.text':
            text = sh
            break
    if not text:
        print('no .text')
        return 1
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    hits = []
    for ins in md.disasm(e.data[text['offset']:text['offset'] + text['size']],
                         text['addr']):
        if ins.mnemonic in ('bl', 'b') and ins.operands:
            try:
                if ins.operands[-1].imm == target:
                    hits.append(ins.address)
            except Exception:
                pass
    print('%d references to 0x%x' % (len(hits), target))
    for h in hits:
        s = nearest(e, h)
        print('  %x  %s+0x%x' % (h, s['name'] if s else '?',
                                 h - s['value'] if s else 0))
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
