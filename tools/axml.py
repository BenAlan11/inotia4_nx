"""Minimal AXML + resources.arsc reader, just enough to resolve android:icon."""
import struct, sys

RES_STRING_POOL = 0x0001
RES_XML           = 0x0003
RES_XML_START_ELEM = 0x0102
RES_TABLE         = 0x0002
RES_TABLE_PKG     = 0x0200
RES_TABLE_TYPE    = 0x0201
RES_TABLE_TYPESPEC= 0x0202

TYPE_REFERENCE = 0x01
TYPE_STRING    = 0x03


def read_pool(d, off):
    typ, hsz, sz = struct.unpack_from('<HHI', d, off)
    cnt, sty, flags, str_start, sty_start = struct.unpack_from('<IIIII', d, off + 8)
    utf8 = bool(flags & 0x100)
    offs = struct.unpack_from('<%dI' % cnt, d, off + hsz)
    base = off + str_start
    out = []
    for o in offs:
        p = base + o
        if utf8:
            # utf16 length varint, then utf8 byte length varint
            n = d[p]; p += 1
            if n & 0x80:
                p += 1
            n = d[p]; p += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | d[p]; p += 1
            out.append(d[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', d, p)[0]; p += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', d, p)[0]; p += 2
            out.append(d[p:p + n * 2].decode('utf-16-le', 'replace'))
    return out


def xml_attrs(d):
    """Yield (element_name, {attr_name: (type, data, raw_string)})."""
    typ, hsz, sz = struct.unpack_from('<HHI', d, 0)
    assert typ == RES_XML
    pool = None
    off = hsz
    while off < sz:
        ctyp, chsz, csz = struct.unpack_from('<HHI', d, off)
        if csz == 0:
            break
        if ctyp == RES_STRING_POOL:
            pool = read_pool(d, off)
        elif ctyp == RES_XML_START_ELEM:
            name = struct.unpack_from('<I', d, off + chsz + 4)[0]
            astart, asize, acount = struct.unpack_from('<HHH', d, off + chsz + 8)
            name_s = pool[name]
            attrs = {}
            for i in range(acount):
                a = off + chsz + astart + i * asize
                ans, anm, araw, vsz, res0, dtype, data = struct.unpack_from('<iiiHBBI', d, a)
                aname = pool[anm]
                raw = pool[araw] if araw >= 0 else None
                attrs[aname] = (dtype, data, raw)
            yield name_s, attrs
        off += csz


def arsc_find(path, res_id):
    d = open(path, 'rb').read()
    typ, hsz, sz = struct.unpack_from('<HHI', d, 0)
    assert typ == RES_TABLE
    gpool = None
    off = hsz
    pkg_id = res_id >> 24
    type_id = (res_id >> 16) & 0xFF
    entry_id = res_id & 0xFFFF
    while off < sz:
        ctyp, chsz, csz = struct.unpack_from('<HHI', d, off)
        if csz == 0:
            break
        if ctyp == RES_STRING_POOL:
            gpool = read_pool(d, off)
        elif ctyp == RES_TABLE_PKG:
            pid = struct.unpack_from('<I', d, off + 8)[0]
            name = d[off + 12:off + 12 + 256].decode('utf-16-le', 'replace').rstrip('\x00')
            tpool_off = off + 8 + 4 + 256 + 4 + 4 + 4 + 4  # header tail
            # walk inner chunks
            io = off + chsz
            end = off + csz
            cur_type = None
            while io < end:
                it, ih, isz = struct.unpack_from('<HHI', d, io)
                if isz == 0:
                    break
                if it == RES_STRING_POOL:
                    pass
                elif it == RES_TABLE_TYPESPEC:
                    cur_type = d[io + 8]
                elif it == RES_TABLE_TYPE:
                    tid = d[io + 8]
                    flags = d[io + 9]
                    ecount, estart = struct.unpack_from('<II', d, io + 12)
                    coff = struct.unpack_from('<I', d, io + 16)[0]
                    if tid == type_id:
                        for i in range(ecount):
                            eo = io + ih + i * 4
                            e = struct.unpack_from('<I', d, eo)[0]
                            eidx = i if e == 0xFFFFFFFF else e + i
                            if eidx != entry_id:
                                continue
                            doff = io + coff + (0 if e == 0xFFFFFFFF else e)
                            esz, eflags, ekey = struct.unpack_from('<HHI', d, doff)
                            if eflags & 0x0008:               # compact entry
                                dtype = eflags >> 8
                                data = ekey
                                return ('inline', dtype, data, None)
                            vtype = d[doff + esz + 3]
                            vdata = struct.unpack_from('<I', d, doff + esz + 4)[0]
                            if vtype == TYPE_STRING:
                                return ('string', vtype, vdata, gpool[vdata])
                            return ('typed', vtype, vdata, None)
                io += isz
        off += csz
    return None


if __name__ == '__main__':
    import zipfile
    z = zipfile.ZipFile(sys.argv[1])
    man = z.read('AndroidManifest.xml')
    app = {}
    for name, attrs in xml_attrs(man):
        if name == 'application':
            app = attrs
            break
    for k, v in sorted(app.items()):
        if 'icon' in k or 'label' in k or 'theme' in k:
            print('%-28s %s' % (k, v))
    ic = app.get('icon')
    if not ic:
        sys.exit('no android:icon')
    dtype, data, raw = ic
    print('icon attr type=0x%x data=0x%08X raw=%r' % (dtype, data, raw))

    import tempfile, os
    tmp = tempfile.mktemp(suffix='.arsc')
    open(tmp, 'wb').write(z.read('resources.arsc'))
    print('resolved:', arsc_find(tmp, data))
    os.unlink(tmp)
