#!/usr/bin/env python3
"""Find instructions that touch a module-relative global via ADRP+offset.

    python tools/find_global.py data/libgame.so 0x6e4000 0x680
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF  # noqa: E402

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, CS_AC_WRITE  # noqa: E402
from capstone.arm64 import ARM64_OP_MEM  # noqa: E402


def nearest(e, addr):
    best = None
    for s in e.exports:
        if s['value'] <= addr and (best is None or s['value'] > best['value']):
            best = s
    return best


def main():
    e = ELF(sys.argv[1])
    page = int(sys.argv[2], 0)
    disp = int(sys.argv[3], 0)
    text = next(sh for sh in e.shdrs if e.sec_name(sh) == '.text')
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    adrp = {}
    for ins in md.disasm(e.data[text['offset']:text['offset'] + text['size']],
                         text['addr']):
        if ins.mnemonic == 'adrp':
            try:
                adrp[ins.operands[0].reg] = ins.operands[1].imm
            except Exception:
                pass
            continue
        if ins.mnemonic in ('add', 'sub') and len(ins.operands) == 3:
            try:
                if ins.operands[0].reg == ins.operands[1].reg and ins.operands[2].type == 2:
                    adrp[ins.operands[0].reg] = adrp.get(ins.operands[0].reg, 0) + \
                        (ins.operands[2].imm if ins.mnemonic == 'add' else -ins.operands[2].imm)
            except Exception:
                pass
            continue
        # any instruction with a memory operand based on a register we know
        for op in ins.operands:
            if op.type == ARM64_OP_MEM:
                base = op.mem.base
                if base in adrp and adrp[base] + op.mem.disp == page + disp:
                    s = nearest(e, ins.address)
                    print('  %x  %-8s %-40s  ; %s+0x%x' %
                          (ins.address, ins.mnemonic, ins.op_str,
                           s['name'] if s else '?', ins.address - s['value'] if s else 0))
        # adrp value itself may be clobbered by writes to the reg
        try:
            for op in ins.operands:
                if op.type == ARM64_OP_REG and op.access and (op.access & CS_AC_WRITE) and op.reg in adrp:
                    del adrp[op.reg]
        except Exception:
            adrp.clear()
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
