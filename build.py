#!/usr/bin/env python3
"""Build driver: zig as the compiler, devkitPro's newlib/libnx as the runtime.

devkitPro's own gcc is a Linux binary, but compiling only needs its headers and
static libraries. zig cc (clang) produces compatible AArch64 objects, and
zig's lld links them against newlib + libnx into an ELF that elf2nro packs.

    python build.py            # debug-ish, full log
    python build.py --clean    # rebuild everything
"""
import glob
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
DKP = os.path.join(ROOT, 'tools', 'dkp_rootfs', 'opt', 'devkitpro')
NEWLIB = os.path.join(DKP, 'devkitA64', 'aarch64-none-elf')
LIBPORT = os.path.join(DKP, 'portlibs', 'switch', 'lib')
INCPORT = os.path.join(DKP, 'portlibs', 'switch', 'include')
LIBNX = os.path.join(DKP, 'libnx')
# crt objects and libgcc live under the gcc version dir, not the newlib lib dir
GCCDIR = None
for _v in sorted(glob.glob(os.path.join(DKP, 'devkitA64', 'lib', 'gcc',
                                        'aarch64-none-elf', '*'))):
    GCCDIR = _v
BUILD = os.path.join(ROOT, 'build')
SRC = os.path.join(ROOT, 'src')

APP = 'inotia4_nx'
TITLE = 'Inotia 4'
AUTHOR = 'port by WorkBuddy'
VERSION = '0.1.0'
TITLEID = '0x010000000000F0A4'

CFLAGS = [
    '-target', 'aarch64-freestanding-none',
    '-mcpu=cortex_a57',
    '-O2', '-g',
    '-fPIC', '-ftls-model=local-exec',
    '-ffunction-sections', '-fdata-sections',
    '-fno-omit-frame-pointer',
    '-fno-strict-aliasing',
    '-Wall', '-Wno-unused-parameter', '-Wno-unused-variable',
    '-Wno-missing-field-initializers', '-Wno-sign-compare',
    '-D__SWITCH__', '-DPATH_MAX=4096',
    '-D_GNU_SOURCE',
    '-isystem', os.path.join(NEWLIB, 'include'),
    '-I', os.path.join(LIBNX, 'include'),
    '-I', INCPORT,
    '-I', os.path.join(INCPORT, 'freetype2'),
    '-I', SRC,
]

LIBS = [
    os.path.join(LIBPORT, 'libGLESv1_CM.a'),
    os.path.join(LIBPORT, 'libEGL.a'),
    os.path.join(LIBPORT, 'libglapi.a'),
    os.path.join(LIBPORT, 'libdrm_nouveau.a'),
    os.path.join(LIBPORT, 'libfreetype.a'),
    os.path.join(LIBPORT, 'libharfbuzz.a'),   # freetype's auto-hinter dependency
    os.path.join(LIBPORT, 'libbz2.a'),
    os.path.join(LIBPORT, 'libpng16.a'),
    os.path.join(LIBPORT, 'libz.a'),
    os.path.join(LIBNX, 'lib', 'libnx.a'),
]


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        print(' '.join(cmd))
        print(p.stdout)
        print(p.stderr)
        raise SystemExit('command failed: %s' % cmd[0])
    if p.stderr.strip():        # warnings are worth seeing, not worth failing on
        sys.stderr.write(p.stderr)
    return p


def zig(*args, **kw):
    return run([sys.executable, '-m', 'ziglang'] + list(args), **kw)


def sources():
    files = []
    for pat in ('*.c', 'third_party/*.c'):
        files += sorted(glob.glob(os.path.join(SRC, pat)))
    return files


def write_buildstamp():
    """Regenerated on every build so a log always names the binary that wrote
    it -- otherwise an old NRO on the card is indistinguishable from a new one."""
    path = os.path.join(BUILD, 'buildstamp.c')
    stamp = time.strftime('%Y-%m-%d %H:%M:%S')
    with open(path, 'w') as f:
        f.write('const char *const g_build_stamp = "%s";\n' % stamp)
    return path


def compile_all():
    os.makedirs(BUILD, exist_ok=True)
    objs = []
    files = sources() + [write_buildstamp()]
    for f in files:
        rel = os.path.relpath(f, SRC).replace(os.sep, '_')
        obj = os.path.join(BUILD, rel[:-2] + '.o')
        objs.append(obj)
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(f):
            continue
        print('  CC %s' % os.path.relpath(f, ROOT))
        zig('cc', *CFLAGS, '-c', f, '-o', obj)
    return objs


def patch_linker_script():
    """lld refuses STT_TLS objects unless the script declares PT_TLS; libnx's
    stock switch.ld does not, so add the segment and put .tdata/.tbss in it
    while keeping them inside the RW load segment."""
    src = os.path.join(LIBNX, 'switch.ld')
    dst = os.path.join(BUILD, 'switch.ld')
    s = open(src, encoding='utf-8').read()
    s = s.replace('\tdyn    PT_DYNAMIC;\n}', '\tdyn    PT_DYNAMIC;\n\ttls    PT_TLS;\n}')
    for sec in ('.tdata', '.tbss'):
        s = s.replace('\t%s :\n' % sec, '\t%s :\n' % sec)
    s = s.replace('\t} :data\n\n\tPROVIDE_HIDDEN( __tdata_lma',
                  '\t} :data :tls\n\n\tPROVIDE_HIDDEN( __tdata_lma')
    s = s.replace('\t} :data\n\n\t.bss :', '\t} :data :tls\n\n\t.bss :')
    open(dst, 'w', encoding='utf-8').write(s)
    return dst


def link(objs):
    elf = os.path.join(BUILD, APP + '.elf')
    rsp = os.path.join(BUILD, 'link.rsp')
    libdir = os.path.join(NEWLIB, 'lib')
    ld = patch_linker_script()
    with open(rsp, 'w') as f:
        f.write('-T %s\n' % ld)
        f.write('-pie --no-dynamic-linker --gc-sections --build-id=sha1\n')
        f.write('--undefined=main\n')
        f.write('-o %s\n' % elf)
        f.write('%s\n' % os.path.join(GCCDIR, 'pic', 'crti.o'))
        f.write('%s\n' % os.path.join(GCCDIR, 'pic', 'crtbegin.o'))
        for o in objs:
            f.write('%s\n' % o)
        for l in LIBS:
            f.write('%s\n' % l)
        f.write('%s\n' % os.path.join(GCCDIR, 'pic', 'crtend.o'))
        f.write('%s\n' % os.path.join(GCCDIR, 'pic', 'crtn.o'))
        f.write('-L %s\n' % os.path.join(libdir, 'pic'))
        f.write('-L %s\n' % libdir)
        f.write('-L %s\n' % os.path.join(GCCDIR, 'pic'))
        f.write('-L %s\n' % LIBPORT)
        # libharfbuzz.a is C++ and drags in the whole libstdc++ runtime, so the
        # C++ archives have to be inside the same group as libc.
        f.write('--start-group\n')
        f.write('  -lstdc++ -lsupc++ -lgcc -lc -lsysbase -lm\n')
        f.write('--end-group\n')
    print('  LD %s' % os.path.basename(elf))
    zig('ld.lld', '@' + rsp)
    return elf


def package(elf):
    nacp = os.path.join(BUILD, 'control.nacp')
    icon = os.path.join(ROOT, 'icon.jpg')
    # nacptool sscanf's the value with a %016x field width, and the width
    # counts the "0x" prefix -- passing it would silently drop a hex digit.
    run([os.path.join(ROOT, 'tools', 'bin', 'nacptool.exe'), '--create',
         TITLE, AUTHOR, VERSION, nacp, '--titleid=' + TITLEID.replace('0x', '')])
    nro = os.path.join(BUILD, APP + '.nro')
    args = [os.path.join(ROOT, 'tools', 'bin', 'elf2nro.exe'), elf, nro,
            '--nacp=' + nacp]
    if os.path.exists(icon):
        args.append('--icon=' + icon)
    run(args)
    print('  NRO %s (%.2f MB)' % (nro, os.path.getsize(nro) / 1048576))
    return nro


def main():
    if '--clean' in sys.argv and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    print('building %s' % APP)
    objs = compile_all()
    elf = link(objs)
    package(elf)


if __name__ == '__main__':
    main()
