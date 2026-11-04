#!/usr/bin/env python3
"""Cross-compile switch-tools' nxlink for Windows with the same zig toolchain the
port uses, so the deploy loop does not need devkitPro (or any build tools) on the
machine that talks to the console.

nxlink needs zlib for the netloader's deflate stream, which is the only external
dependency; tools/zlib-1.3.1 is a stock copy.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, 'tools')
SRC = os.path.join(TOOLS, 'switch-tools-master', 'src')
ZLIB = os.path.join(TOOLS, 'zlib-1.3.1')
OUT = os.path.join(TOOLS, 'bin', 'nxlink.exe')

SOURCES = ['nxlink.c']
ZLIB_SOURCES = ['adler32.c', 'compress.c', 'crc32.c', 'deflate.c', 'trees.c',
                'zutil.c']


def main():
    if not os.path.isdir(ZLIB):
        raise SystemExit('missing tools/zlib-1.3.1 (download zlib 1.3.1 into tools/)')
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    cmd = [sys.executable, '-m', 'ziglang', 'cc',
           '-target', 'x86_64-windows-gnu', '-O2', '-D__WIN32__',
           '-I', ZLIB, '-I', SRC]
    cmd += [os.path.join(SRC, s) for s in SOURCES]
    cmd += [os.path.join(ZLIB, s) for s in ZLIB_SOURCES]
    cmd += ['-lws2_32', '-o', OUT]

    print(' '.join(cmd))
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        raise SystemExit('build failed')
    print('wrote %s (%d bytes)' % (OUT, os.path.getsize(OUT)))


if __name__ == '__main__':
    main()
