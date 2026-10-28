"""Find every call site of a method across all classes in an APK's dex files.

The engine's Java layer decides when the natives are called; the natives
themselves do not. This answers "who calls CWrapperKernel.nativeStart, and with
what" without a full decompiler.

usage: dex_grep.py <apk> <com.example.Class> <methodName>
"""
import sys
import zipfile

from dex_class import Dex, IDX_OPS, LEN


def callers(d, target_cls, target_name):
    """yield (class, method, [(offset, invoke text)]) for methods that call it"""
    for c in range(d.cls_n):
        o = d.cls_off + c * 32
        cls_name = d.type(d.u4(o))
        cd = d.u4(o + 24)
        if not cd:
            continue
        for midx, flags, code in d.methods(cls_name):
            if not code:
                continue
            b = d.b
            regs, ins, outs, tries, dbg, n = struct_unpack(b, code)
            base = code + 16
            i = 0
            hits = []
            while i < n:
                u = b[base + i * 2] | (b[base + i * 2 + 1] << 8)
                op = u & 0xFF
                if op == 0 and (u >> 8) in (1, 2, 3):
                    kind = u >> 8
                    if kind == 1:
                        size = b[base + (i + 1) * 2] | (b[base + (i + 1) * 2 + 1] << 8)
                        i += size * 2 + 4
                    elif kind == 2:
                        size = b[base + (i + 1) * 2] | (b[base + (i + 1) * 2 + 1] << 8)
                        i += size * 4 + 2
                    else:
                        w = b[base + (i + 1) * 2] | (b[base + (i + 1) * 2 + 1] << 8)
                        el = b[base + (i + 2) * 2] | (b[base + (i + 2) * 2 + 1] << 8)
                        cnt = int.from_bytes(b[base + (i + 2) * 2 + 2:base + (i + 2) * 2 + 6], "little")
                        i += 4 + (w * el * cnt + 1) // 2
                    continue
                if op in (0x24, 0x6E, 0x6F, 0x70, 0x71, 0x72):
                    idx = b[base + (i + 1) * 2] | (b[base + (i + 1) * 2 + 1] << 8)
                    kcls, knm, _ = d.method(idx)
                    if kcls == target_cls and knm == target_name:
                        hits.append((i, IDX_OPS[op]))
                i += LEN[op]
            if hits:
                _, nm, _ = d.method(midx)
                yield cls_name, nm, hits


def struct_unpack(b, code):
    import struct
    return struct.unpack_from("<HHHHII", b, code)


def main():
    apk, cls, name = sys.argv[1], sys.argv[2], sys.argv[3]
    want = "L" + cls.strip("L;").replace(".", "/") + ";"
    z = zipfile.ZipFile(apk)
    for dn in sorted(x for x in z.namelist() if x.endswith(".dex")):
        d = Dex(z.read(dn))
        for c, m, hits in callers(d, want, name):
            print("%s.%s  <- %d call(s) at %s" % (
                c.strip("L;").replace("/", "."), m, len(hits),
                ", ".join("0x%04x %s" % h for h in hits)))


if __name__ == "__main__":
    main()
