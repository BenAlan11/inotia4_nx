"""Dump the native method signatures of a classes.dex.

The port calls 17 JNI entry points by hand, and the argument list of each one is
fixed by the Java declaration, not by anything in libgame.so. Getting one wrong
is a silent register mismatch that usually shows up as a crash inside the engine,
so read them off the dex instead of guessing.
"""
import struct
import sys
import zipfile


class Dex:
    def __init__(self, data):
        self.d = data
        (self.string_ids_size, self.string_ids_off,
         self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off,
         _field_ids_size, _field_ids_off,
         self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = struct.unpack_from(
            '<12I', data, 0x38)

    def u32(self, off):
        return struct.unpack_from('<I', self.d, off)[0]

    def string(self, idx):
        off = self.u32(self.string_ids_off + idx * 4)
        p = off
        # uleb128 utf16 length, then MUTF-8 up to NUL
        while self.d[p] & 0x80:
            p += 1
        p += 1
        end = self.d.index(b'\0', p)
        return self.d[p:end].decode('utf-8', 'replace')

    def type_name(self, idx):
        return self.string(self.u32(self.type_ids_off + idx * 4))

    def proto(self, idx):
        ret_type, params_off = struct.unpack_from(
            '<2I', self.d, self.proto_ids_off + idx * 12 + 4)
        params = []
        if params_off:
            n = self.u32(params_off)
            for i in range(n):
                params.append(struct.unpack_from('<H', self.d, params_off + 4 + i * 2)[0])
        return self.type_name(ret_type), [self.type_name(t) for t in params]

    def method(self, idx):
        class_idx, proto_idx, name_idx = struct.unpack_from(
            '<HHI', self.d, self.method_ids_off + idx * 8)
        return (self.type_name(class_idx), self.string(name_idx), proto_idx)

    def classes(self):
        for i in range(self.class_defs_size):
            cd = self.class_defs_off + i * 32
            class_idx = self.u32(cd)
            class_data_off = self.u32(cd + 24)
            yield self.type_name(class_idx), class_data_off

    def native_methods(self):
        for cls, off in self.classes():
            if not off:
                continue
            p = off
            sf, inf, dm, vm = 0, 0, 0, 0
            vals = []
            for _ in range(4):
                v, p = uleb(self.d, p)
                vals.append(v)
            sf, inf, dm, vm = vals
            for _ in range(sf + inf):
                _, p = uleb(self.d, p)          # field_idx_diff
                _, p = uleb(self.d, p)          # access_flags
            # static natives live in the direct list, instance natives in the
            # virtual one; both start their differential index at zero
            midx = 0
            for _ in range(dm):
                diff, p = uleb(self.d, p)
                acc, p = uleb(self.d, p)
                _code, p = uleb(self.d, p)
                midx += diff
                if acc & 0x100:
                    cname, mname, pidx = self.method(midx)
                    ret, params = self.proto(pidx)
                    yield cname, mname, params, ret
            midx = 0
            for _ in range(vm):
                diff, p = uleb(self.d, p)
                acc, p = uleb(self.d, p)
                code, p = uleb(self.d, p)
                midx += diff
                if acc & 0x100:                  # ACC_NATIVE
                    cname, mname, pidx = self.method(midx)
                    ret, params = self.proto(pidx)
                    yield cname, mname, params, ret


def uleb(d, p):
    result = shift = 0
    while True:
        b = d[p]
        p += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return result, p


JAVA_PRIM = {'V': 'void', 'Z': 'jboolean', 'B': 'jbyte', 'S': 'jshort',
             'C': 'jchar', 'I': 'jint', 'J': 'jlong', 'F': 'jfloat',
             'D': 'jdouble'}


def jni_type(desc):
    if desc in JAVA_PRIM:
        return JAVA_PRIM[desc]
    if desc.startswith('['):
        return 'jarray'
    return 'jobject'


def jni_signature(params, ret):
    """The JNIEnv-th function type as it appears in C."""
    args = ['JNIEnv *', 'jobject']
    args += [jni_type(p) for p in params]
    return '%s (*)(%s)' % (jni_type(ret), ', '.join(args))


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\BenAlan\Downloads\Inotia4.apk'
    z = zipfile.ZipFile(apk)
    dexes = sorted(n for n in z.namelist() if n.endswith('.dex'))
    print('dex files:', dexes)
    seen = {}
    for n in dexes:
        dx = Dex(z.read(n))
        for cls, name, params, ret in dx.native_methods():
            seen[(cls, name)] = (params, ret)

    print('\n%d native methods\n' % len(seen))
    for (cls, name), (params, ret) in sorted(seen.items()):
        short = cls.rsplit('/', 1)[-1].lstrip('L').rstrip(';')
        print('%-34s %-34s %s' % (short, name, jni_signature(params, ret)))


if __name__ == '__main__':
    main()
