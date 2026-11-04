"""Turn a crash address into a function name.

Two kinds of input:

    python tools/where.py logs/.../crash_report_0_*.log
    python tools/where.py --pc 0x1a2b3c [--lr 0x...]

Atmosphere's crash report lists a stack trace as "module + 0xOFFSET", and this
maps such an offset (or a bare address, with --base) straight onto the symbols in
build/inotia4_nx.elf. That is the difference between "it crashed somewhere" and
"it crashed in so_finalize".
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, 'build', 'inotia4_nx.elf')


def symbols(path):
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)

    secs = []
    for i in range(shnum):
        (name, typ, _flags, addr, off, size, link, _info, _align,
         entsize) = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize)
        secs.append({'name': name, 'type': typ, 'addr': addr, 'off': off,
                     'size': size, 'link': link, 'entsize': entsize})

    strtab_off = secs[shstrndx]['off']

    def shname(idx):
        p = strtab_off + idx
        return d[p:d.index(b'\0', p)].decode()

    out = []
    for s in secs:
        if s['type'] not in (2, 11) or not s['entsize']:   # SYMTAB, DYNSYM
            continue
        strtab = secs[s['link']]['off']
        for i in range(s['size'] // s['entsize']):
            name, info, _other, shndx, value, size = struct.unpack_from(
                '<IBBHQQ', d, s['off'] + i * s['entsize'])
            if not name or not value:
                continue
            kind = info & 0xF
            if kind not in (0, 2):          # NOTYPE, FUNC
                continue
            p = strtab + name
            out.append((value, size, d[p:d.index(b'\0', p)].decode('utf-8', 'replace')))
    out.sort()
    return out


def lookup(syms, addr):
    best = None
    for value, size, name in syms:
        if value <= addr and (best is None or value >= best[0]):
            best = (value, size, name)
    if not best:
        return None
    value, size, name = best
    delta = addr - value
    if size and delta >= size:
        return '%s + 0x%x (past the end of the symbol)' % (name, delta)
    return '%s + 0x%x' % (name, delta)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report', nargs='?', help='crash report text file')
    ap.add_argument('--pc', help='faulting pc')
    ap.add_argument('--lr', help='link register')
    ap.add_argument('--base', help='load base, if the report gives absolute addresses')
    ap.add_argument('--elf', default=ELF)
    a = ap.parse_args()

    if not os.path.isfile(a.elf):
        raise SystemExit('%s not found; build first' % a.elf)
    syms = symbols(a.elf)
    print('%s: %d symbols' % (os.path.basename(a.elf), len(syms)))

    base = int(a.base, 0) if a.base else 0
    targets = []
    if a.pc:
        targets.append(('pc', int(a.pc, 0)))
    if a.lr:
        targets.append(('lr', int(a.lr, 0)))

    if a.report:
        text = open(a.report, encoding='utf-8', errors='replace').read()
        # "module + 0x1234" style entries in the stack trace
        for m in re.finditer(r'\+ ?0x([0-9a-fA-F]{1,16})', text):
            targets.append(('trace', int(m.group(1), 16)))
        for tag in ('pc', 'lr'):
            m = re.search(r'^\s*%s:?\s*(0x)?([0-9a-fA-F]{6,16})' % tag, text, re.M)
            if m:
                targets.append((tag, int(m.group(2), 16)))
        if not targets:
            print('no addresses recognised in %s; first 20 lines:' % a.report)
            for line in text.splitlines()[:20]:
                print('  ' + line)

    if not targets:
        raise SystemExit('nothing to look up; pass --pc or a crash report')

    seen = set()
    for tag, addr in targets:
        rel = addr - base
        if rel in seen:
            continue
        seen.add(rel)
        where = lookup(syms, rel)
        print('%-6s 0x%08x  %s' % (tag, rel, where or 'no symbol covers it'))


if __name__ == '__main__':
    sys.exit(main())
