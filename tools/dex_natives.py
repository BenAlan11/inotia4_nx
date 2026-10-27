"""Dump the native method signatures of a dex file.

The port has been guessing how the engine's Java layer calls its natives. The
dex has the answer in the method prototypes -- no bytecode decoding needed, just
the id tables and each class's class_data (for the ACC_NATIVE flag).

usage: dex_natives.py <apk> [class-prefix]
"""
import struct
import sys
import zipfile

ACC_NATIVE = 0x0100


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
        self._str = {}
        self._type = {}

    def u4(self, o):
        return struct.unpack_from("<I", self.b, o)[0]

    def string(self, i):
        if i in self._str:
            return self._str[i]
        o = self.u4(self.str_off + i * 4)
        _, o = uleb(self.b, o)          # utf16 length, unused
        end = self.b.index(b"\0", o)
        s = self.b[o:end].decode("utf-8", "replace")
        self._str[i] = s
        return s

    def type(self, i):
        if i not in self._type:
            self._type[i] = self.string(self.u4(self.type_off + i * 4))
        return self._type[i]

    def proto(self, i):
        o = self.proto_off + i * 12
        ret = self.type(self.u4(o + 4))
        params_off = self.u4(o + 8)
        params = []
        if params_off:
            n = self.u4(params_off)
            for k in range(n):
                params.append(self.type(struct.unpack_from("<H", self.b, params_off + 4 + k * 2)[0]))
        return params, ret

    def method(self, i):
        o = self.meth_off + i * 8
        cls = self.type(struct.unpack_from("<H", self.b, o)[0])
        proto = struct.unpack_from("<H", self.b, o + 2)[0]
        name = self.string(self.u4(o + 4))
        return cls, name, proto

    def classes(self):
        for c in range(self.cls_n):
            o = self.cls_off + c * 32
            cls_idx = self.u4(o)
            class_data = self.u4(o + 24)
            yield self.type(cls_idx), class_data

    def natives(self):
        out = []
        for cls, cd in self.classes():
            if not cd:
                continue
            o = cd
            sf, o = uleb(self.b, o)
            inf, o = uleb(self.b, o)
            dm, o = uleb(self.b, o)
            vm, o = uleb(self.b, o)
            for _ in range(sf + inf):
                _, o = uleb(self.b, o)
                _, o = uleb(self.b, o)
            for n_methods in (dm, vm):
                midx = 0
                for _ in range(n_methods):
                    diff, o = uleb(self.b, o)
                    flags, o = uleb(self.b, o)
                    _, o = uleb(self.b, o)      # code_off
                    midx += diff
                    if flags & ACC_NATIVE:
                        mcls, name, proto = self.method(midx)
                        params, ret = self.proto(proto)
                        out.append((mcls, name, params, ret))
        return out


def main():
    apk = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "Lcom/com2us/"
    z = zipfile.ZipFile(apk)
    rows = []
    for n in sorted(x for x in z.namelist() if x.endswith(".dex")):
        d = Dex(z.read(n))
        rows += d.natives()
    seen = set()
    for cls, name, params, ret in sorted(rows):
        key = (cls, name, tuple(params), ret)
        if key in seen:
            continue
        seen.add(key)
        if not cls.startswith(prefix):
            continue
        ps = ",".join(p.strip("L;").replace("/", ".") for p in params)
        r = ret.strip("L;").replace("/", ".")
        print(f"{cls.strip('L;').replace('/', '.')}.{name}({ps}) -> {r}")
    print(f"\n# {len(seen)} distinct native methods in {apk}", file=sys.stderr)


if __name__ == "__main__":
    main()
