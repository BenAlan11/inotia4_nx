#!/usr/bin/env python3
"""Minimal ELF64 reader used to audit Android .so files before porting.

Produces the data the so-loader port actually needs:
  * architecture / type / flags
  * DT_NEEDED list
  * imported (undefined, global) symbols with library-version tags
  * exported symbols (for locating engine entry points)
  * JNI_OnLoad / Java_* registration inventory
"""
import struct
import sys
import json
from collections import OrderedDict

SHT_DYNSYM = 11
SHT_DYNAMIC = 6
DT_NULL, DT_NEEDED, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT = 0, 1, 5, 6, 10, 11
DT_GNU_HASH, DT_HASH, DT_VERSYM, DT_VERNEED, DT_VERNEEDNUM = 0x6ffffef5, 4, 0x6ffffff0, 0x6ffffffe, 0x6fffffff
STB_GLOBAL, STB_WEAK = 1, 2


class ELF:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] != b'\x7fELF':
            raise ValueError('not an ELF file')
        self.ei_class = d[4]
        self.ei_data = d[5]
        if self.ei_class != 2:
            raise ValueError('only ELF64 supported')
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize,
         self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', d, 16)

        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from('<IIQQQQQQ', d, off)
            self.phdrs.append(dict(type=p_type, flags=p_flags, offset=p_offset,
                                   vaddr=p_vaddr, filesz=p_filesz, memsz=p_memsz))

        self.shdrs = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = \
                struct.unpack_from('<IIQQQQIIQQ', d, off)
            self.shdrs.append(dict(name_off=name, type=sh_type, flags=sh_flags, addr=sh_addr,
                                   offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                                   entsize=sh_entsize))

        # section name table
        self.shstrtab = b''
        if self.e_shnum:
            s = self.shdrs[self.e_shstrndx]
            self.shstrtab = d[s['offset']:s['offset'] + s['size']]

        self.dynsym = []          # list of dicts
        self.dynamic = {}
        self.needed = []
        self.imports = OrderedDict()
        self.exports = []
        self._parse()

    def sec_name(self, sh):
        n = sh['name_off']
        e = self.shstrtab.find(b'\0', n)
        return self.shstrtab[n:e].decode('latin1')

    def _vread(self, vaddr, size):
        for p in self.phdrs:
            if p['type'] == 1 and p['vaddr'] <= vaddr < p['vaddr'] + p['filesz']:
                off = p['offset'] + (vaddr - p['vaddr'])
                return self.data[off:off + size]
        return b''

    def _parse(self):
        dynsym_off = dynsym_size = dynstr_off = dynstr_size = None
        sym_entsize = 24
        dyn = {}
        for sh in self.shdrs:
            if sh['type'] == SHT_DYNAMIC:
                for i in range(sh['size'] // 16):
                    tag, val = struct.unpack_from('<qQ', self.data, sh['offset'] + i * 16)
                    dyn.setdefault(tag, val)
                    if tag == DT_NULL:
                        break
            elif sh['type'] == SHT_DYNSYM:
                dynsym_off, dynsym_size, sym_entsize = sh['offset'], sh['size'], sh['entsize'] or 24
                dynstr_off, dynstr_size = self.shdrs[sh['link']]['offset'], self.shdrs[sh['link']]['size']
        if dynsym_off is None:
            return
        self.dynstr = self.data[dynstr_off:dynstr_off + dynstr_size]

        def str_at(n):
            e = self.dynstr.find(b'\0', n)
            return self.dynstr[n:e].decode('latin1')

        # version tables
        versym = []
        verneed_names = {}
        if DT_VERSYM in dyn and DT_VERNEED in dyn:
            vaddr = dyn[DT_VERSYM]
            raw = self._vread(vaddr, (dynsym_size // sym_entsize) * 2)
            versym = list(struct.unpack('<%dH' % (len(raw) // 2), raw)) if raw else []
            vn_off = self._vread_vaddr_to_off(dyn[DT_VERNEED])
            if vn_off is not None:
                n = dyn.get(DT_VERNEEDNUM, 0)
                p = vn_off
                for _ in range(n):
                    vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', self.data, p)
                    fname = str_at(vn_file)
                    ap = p + vn_aux
                    for _ in range(vn_cnt):
                        vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from('<IHHII', self.data, ap)
                        verneed_names[vna_other & 0x7fff] = (fname, str_at(vna_name))
                        if not vna_next:
                            break
                        ap += vna_next
                    if not vn_next:
                        break
                    p += vn_next

        for i in range(dynsym_size // sym_entsize):
            off = dynsym_off + i * sym_entsize
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from('<IBBHQQ', self.data, off)
            if st_name == 0:
                continue
            name = str_at(st_name)
            bind = st_info >> 4
            typ = st_info & 0xf
            ver = ''
            if versym and i < len(versym) and versym[i] & 0x7fff:
                v = verneed_names.get(versym[i] & 0x7fff)
                ver = '@%s' % v[1] if v else ''
            rec = dict(name=name, bind=bind, type=typ, shndx=st_shndx,
                       value=st_value, size=st_size, ver=ver)
            if st_shndx == 0:  # UND
                if bind in (STB_GLOBAL, STB_WEAK):
                    if name not in self.imports:
                        self.imports[name] = rec
            else:
                if bind in (STB_GLOBAL, STB_WEAK):
                    self.exports.append(rec)
        # DT_NEEDED via dynamic strtab
        if DT_NEEDED in dyn:
            strtab_off = self._vread_vaddr_to_off(dyn.get(DT_STRTAB, 0))
            if strtab_off:
                n = 1
                vals = []
                for sh in self.shdrs:
                    if sh['type'] == SHT_DYNAMIC:
                        for i in range(sh['size'] // 16):
                            tag, val = struct.unpack_from('<qQ', self.data, sh['offset'] + i * 16)
                            if tag == DT_NEEDED:
                                vals.append(val)
                            if tag == DT_NULL:
                                break
                for v in vals:
                    e = self.data.find(b'\0', strtab_off + v)
                    self.needed.append(self.data[strtab_off + v:e].decode('latin1'))

    def _vread_vaddr_to_off(self, vaddr):
        for p in self.phdrs:
            if p['type'] == 1 and p['vaddr'] <= vaddr < p['vaddr'] + p['filesz']:
                return p['offset'] + (vaddr - p['vaddr'])
        return None

    def is_stripped(self):
        return not any(sh['type'] == 2 and sh['size'] > 0 for sh in self.shdrs)


def classify(sym):
    n = sym['name']
    if n.startswith('gl') and n[2:3].isupper():
        return 'GLES1'
    if n.startswith('Java_com_com2us'):
        return 'JNI'
    if n.startswith('__android') or n in ('__sF', '__errno', '__system_property_get', 'android_set_abort_message'):
        return 'bionic'
    if n.startswith('pthread') or n.startswith('__pthread'):
        return 'pthread'
    if n.startswith(('socket', 'connect', 'select', 'getaddrinfo', 'recvfrom', 'sendto', 'inet_', 'gethostby', 'fcntl', 'send', 'recv', 'setsockopt', 'bind', 'listen', 'accept', 'shutdown', 'poll')):
        return 'net'
    if n.startswith(('fopen', 'open', 'opendir', 'stat', 'mkdir', 'unlink', 'rename', 'access', 'readdir', 'closedir', 'fread', 'fwrite', 'fclose', 'fseek', 'ftell', 'pread', 'lseek', 'remove', 'rmdir', 'chmod', 'ftruncate', 'fsync')):
        return 'fs'
    return 'libc'


def main():
    e = ELF(sys.argv[1])
    print('== %s ==' % e.path)
    print('class=ELF%d data=%d type=%d machine=%d (183=AArch64, 40=ARM) entry=0x%x flags=0x%x'
          % (e.ei_class, e.ei_data, e.e_type, e.e_machine, e.e_entry, e.e_flags))
    print('segments:')
    for p in e.phdrs:
        print('   type=%-2d off=0x%-7x vaddr=0x%-8x filesz=%-9d memsz=%-9d flags=%d'
              % (p['type'], p['offset'], p['vaddr'], p['filesz'], p['memsz'], p['flags']))
    print('DT_NEEDED: %s' % ', '.join(e.needed))
    print('imports: %d   exports: %d   stripped-ish: %s'
          % (len(e.imports), len(e.exports), e.is_stripped()))

    groups = {}
    for s in e.imports.values():
        groups.setdefault(classify(s), []).append(s['name'])
    print('\n-- imports by class --')
    for k in sorted(groups):
        print('%-8s %3d  %s' % (k, len(groups[k]), ' '.join(sorted(groups[k]))))

    jni = sorted(x['name'] for x in e.exports if x['name'].startswith('Java_'))
    print('\n-- JNI exports (%d) --' % len(jni))
    for j in jni:
        print('  ' + j)

    if len(sys.argv) > 2 and sys.argv[2] == '--json':
        json.dump(dict(needed=e.needed,
                       imports=sorted(e.imports.keys()),
                       exports=[x['name'] for x in e.exports],
                       groups={k: sorted(v) for k, v in groups.items()}),
                  open(sys.argv[3], 'w'), indent=1)


if __name__ == '__main__':
    main()
