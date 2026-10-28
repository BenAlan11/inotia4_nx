"""Dump a class's methods from a dex, with the bytecode of the ones that matter.

The engine's Java layer decides the native call order and the arguments (a
command name for CWrapper.nativeInitialize, the buffers for CEventHandler, ...).
Guessing that from the .so is what cost three hardware runs. This reads it out
of the dex instead.

usage: dex_class.py <apk> <com.example.Class> [method-filter]

Only the instructions that carry an index (const-string, invoke-*, sget/sput,
new-instance, iget/iput, const-class, check-cast) are annotated; the rest are
printed as offset + mnemonic so the control flow stays readable.
"""
import struct
import sys
import zipfile

# opcode -> instruction length in 16-bit units (the dalvik format table)
LEN = [1] * 0x100


def _set(rng, n):
    for o in rng:
        LEN[o] = n


_set(range(0x02, 0x03), 2)
_set([0x03], 3)
_set([0x05], 2)
_set([0x06], 3)
_set([0x08], 2)
_set([0x09], 3)
_set([0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x1F, 0x20, 0x22, 0x23,
      0x29, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
      0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D], 2)
_set([0x14, 0x17, 0x1B, 0x24, 0x25, 0x26, 0x2A, 0x2B, 0x2C], 3)
_set([0x18], 5)
_set(range(0x2D, 0x32), 2)
_set(range(0x44, 0x52), 2)
_set(range(0x52, 0x6E), 2)
_set(range(0x6E, 0x73), 3)
_set(range(0x74, 0x79), 3)
_set(range(0x90, 0xB0), 2)
_set(range(0xD0, 0xE3), 2)
_set(range(0xE3, 0xFA), 2)
_set([0xFA, 0xFB], 4)
_set([0xFC, 0xFD], 3)
_set([0xFE, 0xFF], 2)

ACC_STATIC = 0x0008
ACC_NATIVE = 0x0100
ACC_ABSTRACT = 0x0400
ACC_CONSTRUCTOR = 0x10000

IDX_OPS = {
    0x1A: "const-string", 0x1B: "const-string/jumbo", 0x1C: "const-class",
    0x1F: "check-cast", 0x20: "instance-of", 0x22: "new-instance",
    0x23: "new-array", 0x24: "filled-new-array", 0x25: "filled-new-array/range",
    0x26: "fill-array-data", 0x52: "iget", 0x53: "iget-wide", 0x54: "iget-object",
    0x55: "iget-boolean", 0x56: "iget-byte", 0x57: "iget-char", 0x58: "iget-short",
    0x59: "iput", 0x5A: "iput-wide", 0x5B: "iput-object", 0x5C: "iput-boolean",
    0x5D: "iput-byte", 0x5E: "iput-char", 0x5F: "iput-short",
    0x60: "sget", 0x61: "sget-wide", 0x62: "sget-object", 0x63: "sget-boolean",
    0x64: "sget-byte", 0x65: "sget-char", 0x66: "sget-short",
    0x67: "sput", 0x68: "sput-wide", 0x69: "sput-object", 0x6A: "sput-boolean",
    0x6B: "sput-byte", 0x6C: "sput-char", 0x6D: "sput-short",
    0x6E: "invoke-virtual", 0x6F: "invoke-super", 0x70: "invoke-direct",
    0x71: "invoke-static", 0x72: "invoke-interface",
    0x74: "invoke-virtual/range", 0x75: "invoke-super/range",
    0x76: "invoke-direct/range", 0x77: "invoke-static/range",
    0x78: "invoke-interface/range",
}


def uleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, o


class Dex:
    def __init__(self, b):
        self.b = b
        (self.str_n, self.str_off, self.type_n, self.type_off,
         self.proto_n, self.proto_off, self.field_n, self.field_off,
         self.meth_n, self.meth_off, self.cls_n, self.cls_off) = struct.unpack_from(
            "<12I", b, 0x38)
        self._str, self._type = {}, {}

    def u4(self, o):
        return struct.unpack_from("<I", self.b, o)[0]

    def string(self, i):
        if i not in self._str:
            o = self.u4(self.str_off + i * 4)
            _, o = uleb(self.b, o)
            self._str[i] = self.b[o:self.b.index(b"\0", o)].decode("utf-8", "replace")
        return self._str[i]

    def type(self, i):
        if i not in self._type:
            self._type[i] = self.string(self.u4(self.type_off + i * 4))
        return self._type[i]

    def proto(self, i):
        o = self.proto_off + i * 12
        ret = self.type(self.u4(o + 4))
        po = self.u4(o + 8)
        params = []
        if po:
            n = self.u4(po)
            params = [self.type(struct.unpack_from("<H", self.b, po + 4 + k * 2)[0])
                      for k in range(n)]
        return params, ret

    def method(self, i):
        o = self.meth_off + i * 8
        cls = self.type(struct.unpack_from("<H", self.b, o)[0])
        pr = struct.unpack_from("<H", self.b, o + 2)[0]
        return cls, self.string(self.u4(o + 4)), pr

    def field(self, i):
        o = self.field_off + i * 8
        cls = self.type(struct.unpack_from("<H", self.b, o)[0])
        typ = self.type(struct.unpack_from("<H", self.b, o + 2)[0])
        return cls, self.string(self.u4(o + 4)), typ

    def class_data(self, name):
        for c in range(self.cls_n):
            o = self.cls_off + c * 32
            if self.type(self.u4(o)) != name:
                continue
            return self.u4(o + 24)
        return None

    def methods(self, name):
        cd = self.class_data(name)
        if not cd:
            return []
        o = cd
        sf, o = uleb(self.b, o)
        inf, o = uleb(self.b, o)
        dm, o = uleb(self.b, o)
        vm, o = uleb(self.b, o)
        for _ in range(sf + inf):
            _, o = uleb(self.b, o)
            _, o = uleb(self.b, o)
        out = []
        for n in (dm, vm):
            midx = 0
            for _ in range(n):
                d, o = uleb(self.b, o)
                flags, o = uleb(self.b, o)
                code, o = uleb(self.b, o)
                midx += d
                out.append((midx, flags, code))
        return out

    def disasm(self, code_off):
        b = self.b
        regs, ins, outs, tries, dbg, n = struct.unpack_from("<HHHHII", b, code_off)
        base = code_off + 16
        lines = []
        i = 0
        while i < n:
            u = struct.unpack_from("<H", b, base + i * 2)[0]
            op = u & 0xFF
            if op == 0 and (u >> 8) in (1, 2, 3):
                kind = u >> 8
                if kind == 1:
                    size = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    i += size * 2 + 4
                elif kind == 2:
                    size = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    i += size * 4 + 2
                else:
                    w = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    el = struct.unpack_from("<H", b, base + (i + 2) * 2)[0]
                    cnt = struct.unpack_from("<I", b, base + (i + 2) * 2 + 2)[0]
                    i += 4 + (w * el * cnt + 1) // 2
                continue
            length = LEN[op]
            txt = IDX_OPS.get(op)
            if txt:
                if op in (0x24, 0x6E, 0x6F, 0x70, 0x71, 0x72):
                    idx = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    c, nm, pr = self.method(idx)
                    p, r = self.proto(pr)
                    sig = "(" + "".join(x.strip("L;").replace("/", ".") for x in p) + ")" + \
                          r.strip("L;").replace("/", ".")
                    txt += "  " + c.strip("L;").replace("/", ".") + "." + nm + sig
                elif op == 0x1A:
                    txt += "  \"" + self.string(struct.unpack_from("<H", b, base + (i + 1) * 2)[0]) + "\""
                elif op == 0x1B:
                    txt += "  \"" + self.string(struct.unpack_from(
                        "<I", b, base + (i + 1) * 2)[0]) + "\""
                elif op in (0x1C, 0x1F, 0x20, 0x22, 0x23):
                    idx = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    txt += "  " + self.type(idx).strip("L;").replace("/", ".")
                elif op in (0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
                            0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63,
                            0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D):
                    idx = struct.unpack_from("<H", b, base + (i + 1) * 2)[0]
                    c, nm, t = self.field(idx)
                    txt += "  " + c.strip("L;").replace("/", ".") + "." + nm + " : " + \
                           t.strip("L;").replace("/", ".")
            else:
                txt = "0x%02x" % op
                b1 = b[base + i * 2 + 1]
                if op in (0x12,):                       # const/4 vA, #+B
                    txt = "const/4 v%d, #%d" % (b1 & 0xF, (b1 >> 4) - 16 if b1 >> 4 > 7 else b1 >> 4)
                elif op in (0x01, 0x04, 0x07, 0x21, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
                            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
                            0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0xB0, 0xB1,
                            0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
                            0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3,
                            0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC,
                            0xCD, 0xCE, 0xCF):
                    txt = "0x%02x v%d, v%d" % (op, b1 & 0xF, b1 >> 4)
                elif op in (0x0A, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1D, 0x1E,
                            0x27):
                    txt = "0x%02x v%d" % (op, b1)
                elif op in (0x28, 0x29, 0x2A):          # goto +AA / +AAAA
                    off = b1 if op == 0x28 else struct.unpack_from(
                        "<h" if op == 0x29 else "<i", b, base + i * 2 + 2)[0]
                    txt = "goto %+d -> %04x" % (off, i + off)
                elif op in (0x32, 0x33, 0x34, 0x35, 0x36, 0x37):   # if-* vA, vB, +CCCC
                    off = struct.unpack_from("<h", b, base + i * 2 + 2)[0]
                    txt = "if%02x v%d, v%d -> %04x" % (op, b1 & 0xF, b1 >> 4, i + off)
                elif op in (0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D):   # if-*z vAA, +BBBB
                    off = struct.unpack_from("<h", b, base + i * 2 + 2)[0]
                    txt = "if%02xz v%d -> %04x" % (op, b1, i + off)
                elif op in (0x13, 0x16):                # const/16 vAA, #+BBBB
                    txt = "const/16 v%d, #%d" % (b1, struct.unpack_from(
                        "<h", b, base + i * 2 + 2)[0])
                elif op == 0x0E:
                    txt = "return-void"
            lines.append("%04x  %s" % (i, txt))
            i += length
        return lines


def main():
    apk, want = sys.argv[1], sys.argv[2]
    flt = sys.argv[3] if len(sys.argv) > 3 else ""
    name = "L" + want.strip("L;").replace(".", "/") + ";"
    z = zipfile.ZipFile(apk)
    for dn in sorted(x for x in z.namelist() if x.endswith(".dex")):
        d = Dex(z.read(dn))
        ms = d.methods(name)
        if not ms:
            continue
        print("== %s  (%s)" % (want, dn))
        for midx, flags, code in ms:
            _, nm, pr = d.method(midx)
            p, r = d.proto(pr)
            attrs = ("static " if flags & ACC_STATIC else "") + \
                    ("native " if flags & ACC_NATIVE else "") + \
                    ("abstract " if flags & ACC_ABSTRACT else "")
            sig = "(" + ",".join(x.strip("L;").replace("/", ".") for x in p) + ")" + \
                  r.strip("L;").replace("/", ".")
            print("\n  %s%s %s%s" % (attrs, nm, sig, "   [no code]" if not code else ""))
            if not code or (flt and flt not in nm):
                continue
            for ln in d.disasm(code):
                print("      " + ln)
        return
    print("class not found: " + want, file=sys.stderr)


if __name__ == "__main__":
    main()
