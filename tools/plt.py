"""Resolve a PLT stub address in libgame.so to the imported symbol it jumps to."""
import struct, sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from elfinfo import ELF
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

class Resolver:
    def __init__(self, path):
        self.e = ELF(path)
        d = self.d = self.e.data
        md = self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        shoff, = struct.unpack_from('<Q', d, 0x28)
        se, shn, ss = struct.unpack_from('<HHH', d, 0x3A)
        secs = []
        for i in range(shn):
            nm, t, f, a, o, sz, l, inf, al, en = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * se)
            secs.append(dict(name=nm, type=t, off=o, size=sz, link=l))
        shstr = secs[ss]['off']
        def sn(i):
            p = shstr + i
            return d[p:d.index(b'\0', p)].decode()
        bn = {sn(s['name']): s for s in secs}
        dynstr = secs[bn['.dynsym']['link']]
        def sa(o):
            p = dynstr['off'] + o
            return d[p:d.index(b'\0', p)].decode('utf-8', 'replace')
        self.got = {}
        for sec in ('.rela.plt', '.rela.dyn'):
            s = bn.get(sec)
            if not s:
                continue
            for i in range(s['size'] // 24):
                o, info, _ = struct.unpack_from('<QQq', d, s['off'] + i * 24)
                if (info & 0xffffffff) != 1026:
                    continue
                st = struct.unpack_from('<I', d, secs[s['link']]['off'] + (info >> 32) * 24)[0]
                self.got[o] = sa(st)
        self.syms = sorted({s['value']: s['name'] for s in self.e.exports if s['name']}.items())

    def off_of(self, va):
        for p in self.e.phdrs:
            if p['type'] == 1 and p['vaddr'] <= va < p['vaddr'] + p['filesz']:
                return va - p['vaddr'] + p['offset']
        return None

    def stub(self, va):
        page = slot = None
        o = self.off_of(va)
        for ins in self.md.disasm(self.d[o:o + 16], va):
            if ins.mnemonic == 'adrp':
                page = int(ins.op_str.split('#')[1], 16)
            elif ins.mnemonic == 'ldr':
                try:
                    slot = int(ins.op_str.split('#')[-1].rstrip(']'), 16)
                except Exception:
                    pass
        if page is None or slot is None:
            return None
        return self.got.get(page + slot)

    def near(self, a):
        prev = (0, '?')
        for v, n in self.syms:
            if v <= a:
                prev = (v, n)
            else:
                break
        return prev


if __name__ == '__main__':
    r = Resolver(sys.argv[1] if len(sys.argv) > 1 else 'data/libgame.so')
    for a in sys.argv[2:]:
        va = int(a, 16)
        print('0x%06x -> %s' % (va, r.stub(va) or '(not a PLT stub)'))
