#!/usr/bin/env python3
"""Find code that references a string/address in libgame.so and disassemble
around the reference.

    python tools/xref.py data/libgame.so game_res [--ctx 30]

AArch64 position-independent code reaches rodata either as ADRP+ADD (address
of the string) or ADRP+LDR (address of a pointer to it).  Both forms are
scanned.  Everything is printed with real string operands resolved so the
call being set up can be read off directly.
"""
import argparse
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import (ARM64_OP_REG, ARM64_OP_IMM, ARM64_INS_ADRP,
                            ARM64_INS_ADD, ARM64_INS_LDR, ARM64_INS_BL)
import struct


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        assert d[:4] == b'\x7fELF' and d[4] == 2
        e_phoff, = struct.unpack_from('<Q', d, 0x20)
        e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x36)
        e_shoff, = struct.unpack_from('<Q', d, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
        self.segs = []
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            p_type, p_flags = struct.unpack_from('<II', d, o)
            p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<QQQQ', d, o + 8)
            if p_type == 1:
                self.segs.append((p_vaddr, p_offset, p_filesz, p_flags))
        shstr = e_shoff + e_shstrndx * e_shentsize
        shstr_off, = struct.unpack_from('<Q', d, shstr + 0x18)
        self.sections = {}
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            name, = struct.unpack_from('<I', d, o)
            addr, off, size = struct.unpack_from('<QQQ', d, o + 0x10)
            end = d.index(b'\0', shstr_off + name)
            self.sections[d[shstr_off + name:end].decode()] = (addr, off, size)

    def read(self, vaddr, n):
        for va, off, sz, fl in self.segs:
            if va <= vaddr < va + sz:
                o = off + (vaddr - va)
                return self.d[o:o + n]
        return b''

    def v2o(self, vaddr):
        for va, off, sz, fl in self.segs:
            if va <= vaddr < va + sz:
                return off + (vaddr - va)
        return None

    def cstr(self, vaddr, limit=200):
        raw = self.read(vaddr, limit)
        z = raw.find(b'\0')
        if z >= 0:
            raw = raw[:z]
        return raw.decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('needle')
    ap.add_argument('--ctx', type=int, default=24)
    ap.add_argument('--max', type=int, default=6)
    a = ap.parse_args()

    e = Elf(a.elf)
    needle = a.needle.encode()
    # locate every rodata occurrence
    targets = []
    i = e.d.find(needle)
    while i >= 0:
        # convert file offset back to vaddr
        for va, off, sz, fl in e.segs:
            if off <= i < off + sz:
                targets.append(va + (i - off))
                break
        i = e.d.find(needle, i + 1)
    if not targets:
        print('needle not found')
        return 1
    # a reference usually points at the start of the literal, not at the needle
    # inside it, so also register the enclosing NUL-terminated run's first byte.
    for t in list(targets):
        o = e.v2o(t)
        if o is None:
            continue
        s = o
        while s > 0 and e.d[s - 1] >= 0x20:
            s -= 1
        targets.append(t - (o - s))
    print('needle at vaddrs:', [hex(t) for t in targets])

    tgt = set(targets)
    # also allow references through a pointer stored in .data.rel.ro
    for t in list(targets):
        for va, off, sz, fl in e.segs:
            blob = e.d[off:off + sz]
            p = struct.pack('<Q', t)
            j = blob.find(p)
            while j >= 0:
                tgt.add(va + j)
                j = blob.find(p, j + 1)

    text_va, text_off, text_sz = e.sections['.text']
    code = e.d[text_off:text_off + text_sz]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    # decode whole .text once, grouping into pages for ADRP resolution
    insns = list(md.disasm(code, text_va))
    hits = []
    for idx, ins in enumerate(insns):
        if ins.id not in (ARM64_INS_ADD, ARM64_INS_LDR):
            continue
        ops = ins.operands
        if len(ops) < 3:
            continue
        if ops[1].type != ARM64_OP_REG or ops[2].type != ARM64_OP_IMM:
            continue
        # walk back for the matching ADRP on the same dest reg
        for k in range(idx - 1, max(-1, idx - 6), -1):
            prev = insns[k]
            if prev.id == ARM64_INS_ADRP and prev.operands[1].type == ARM64_OP_IMM:
                pg = prev.operands[1].imm
                addr = pg + ops[2].imm
                if addr in tgt:
                    hits.append((idx, addr))
                break
    print('references: %d' % len(hits))
    seen = set()
    for idx, addr in hits[:a.max]:
        lo = max(0, idx - a.ctx)
        hi = min(len(insns), idx + a.ctx)
        if lo in seen:
            continue
        seen.add(lo)
        print('\n=== ref to %s (0x%x) at 0x%x ===' %
              (e.cstr(addr, 120), addr, insns[idx].address))
        for k in range(lo, hi):
            ins = insns[k]
            mark = '>>' if k == idx else '  '
            com = ''
            # annotate ADRP/ADD pairs that land in rodata
            if ins.id == ARM64_INS_ADD and len(ins.operands) >= 3 and \
                    ins.operands[2].type == ARM64_OP_IMM:
                for j in range(k - 1, max(-1, k - 6), -1):
                    if insns[j].id == ARM64_INS_ADRP:
                        t = insns[j].operands[1].imm + ins.operands[2].imm
                        s = e.cstr(t, 60)
                        if s and s.isprintable():
                            com = '   ; "%s"' % s
                        break
            if ins.id == ARM64_INS_BL:
                tgt2 = ins.operands[0].imm
                com = '   ; sub_%x' % tgt2
            print('%s %08x  %-8s %s%s' % (mark, ins.address, ins.mnemonic,
                                          ins.op_str, com))
    return 0


if __name__ == '__main__':
    sys.exit(main())
