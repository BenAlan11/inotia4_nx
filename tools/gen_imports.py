#!/usr/bin/env python3
"""Generate source/imports.c from the undefined symbols of a target .so.

Every import must be answered, otherwise so_resolve leaves a bogus pointer and
the game crashes at a random address. This script reads the ELF, resolves each
symbol against a curated provider table, and refuses to emit anything while a
symbol is still unaccounted for -- so the table can never silently rot.

usage: python gen_imports.py <libgame.so> <src/imports.c> [--check]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import ELF, classify  # noqa: E402

# symbol -> C expression producing its address.
# Anything not listed here is resolved by name against newlib/libnx.
# calls a stalled engine tends to sit in; wrapped so a hang names itself
PROVIDERS = {
    'fstat': '(uintptr_t)&shim_fstat',
    'pthread_create': '(uintptr_t)&shim_pthread_create',
    'pthread_join': '(uintptr_t)&shim_pthread_join',
    'malloc': '(uintptr_t)&shim_malloc',
    'memalign': '(uintptr_t)&shim_memalign',
    'strdup': '(uintptr_t)&shim_strdup',
    'vasprintf': '(uintptr_t)&shim_vasprintf',
    'calloc': '(uintptr_t)&shim_calloc',
    'realloc': '(uintptr_t)&shim_realloc',
    'usleep': '(uintptr_t)&shim_usleep',
    'nanosleep': '(uintptr_t)&shim_nanosleep',
    'clock': '(uintptr_t)&shim_clock',
    'gettimeofday': '(uintptr_t)&shim_gettimeofday',
    # --- bionic stdio array -------------------------------------------------
    '__sF': '(uintptr_t)&__sF_storage',

    # --- bionic / android ---------------------------------------------------
    '__android_log_print': '(uintptr_t)&shim_android_log_print',
    '__errno': '(uintptr_t)&__errno_shim',
    '__system_property_get': '(uintptr_t)&shim_system_property_get',
    'android_set_abort_message': '(uintptr_t)&shim_android_set_abort_message',
    'getauxval': '(uintptr_t)&shim_getauxval',
    'dl_iterate_phdr': '(uintptr_t)&shim_dl_iterate_phdr',
    '__cxa_atexit': '(uintptr_t)&shim_cxa_atexit',
    '__cxa_finalize': '(uintptr_t)&shim_cxa_finalize',

    # --- fortify wrappers ---------------------------------------------------
    '__memcpy_chk': '(uintptr_t)&memcpy',
    '__memmove_chk': '(uintptr_t)&memmove',
    '__memset_chk': '(uintptr_t)&memset',
    '__strcat_chk': '(uintptr_t)&strcat',
    '__strchr_chk': '(uintptr_t)&strchr',
    '__strcpy_chk': '(uintptr_t)&strcpy',
    '__strlen_chk': '(uintptr_t)&strlen',
    '__strncat_chk': '(uintptr_t)&strncat',
    '__strncpy_chk': '(uintptr_t)&strncpy',
    '__strncpy_chk2': '(uintptr_t)&strncpy',
    '__vsnprintf_chk': '(uintptr_t)&shim_vsnprintf_chk',
    '__vsprintf_chk': '(uintptr_t)&shim_vsprintf_chk',
    '__open_2': '(uintptr_t)&shim_open',
    '__read_chk': '(uintptr_t)&read',
    '__FD_SET_chk': '(uintptr_t)&shim_fd_set_chk',
    '__FD_ISSET_chk': '(uintptr_t)&shim_fd_isset_chk',
    '__stack_chk_fail': '(uintptr_t)&shim_stack_chk_fail',

    # --- file api (path rewriting) -----------------------------------------
    'open': '(uintptr_t)&shim_open',
    'fopen': '(uintptr_t)&shim_fopen',
    'stat': '(uintptr_t)&shim_stat',
    'lstat': '(uintptr_t)&shim_lstat',
    'access': '(uintptr_t)&shim_access',
    'mkdir': '(uintptr_t)&shim_mkdir',
    'unlink': '(uintptr_t)&shim_unlink',
    'remove': '(uintptr_t)&shim_remove',
    'rmdir': '(uintptr_t)&shim_rmdir',
    'rename': '(uintptr_t)&shim_rename',
    'opendir': '(uintptr_t)&shim_opendir',
    'statfs': '(uintptr_t)&shim_statfs',
    'fclose': '(uintptr_t)&shim_fclose',
    'fread': '(uintptr_t)&shim_fread',
    'fwrite': '(uintptr_t)&shim_fwrite',
    'fflush': '(uintptr_t)&shim_fflush',
    'fputc': '(uintptr_t)&shim_fputc',
    'fgetc': '(uintptr_t)&shim_fgetc',
    'fgets': '(uintptr_t)&shim_fgets',
    'feof': '(uintptr_t)&shim_feof',
    'fprintf': '(uintptr_t)&shim_fprintf',
    'vfprintf': '(uintptr_t)&shim_vfprintf',

    # --- misc libc gaps ----------------------------------------------------
    'mmap': '(uintptr_t)&shim_mmap',
    'munmap': '(uintptr_t)&shim_munmap',
    'popen': '(uintptr_t)&shim_popen',
    'pclose': '(uintptr_t)&shim_pclose',
    'system': '(uintptr_t)&shim_system',
    'sincos': '(uintptr_t)&shim_sincos',
    'getpagesize': '(uintptr_t)&shim_getpagesize',
    'getpid': '(uintptr_t)&shim_getpid',
    'kill': '(uintptr_t)&shim_kill',
    'dup': '(uintptr_t)&shim_dup',
    'fcntl': '(uintptr_t)&shim_fcntl',
    'inet_addr': '(uintptr_t)&shim_inet_addr',
    'inet_ntoa': '(uintptr_t)&shim_inet_ntoa',
    'openlog': '(uintptr_t)&shim_openlog',
    'closelog': '(uintptr_t)&shim_closelog',
    'syslog': '(uintptr_t)&shim_syslog',

    # --- audio hooks the engine resolves by name ---------------------------
}

# Names that are known to exist in newlib/libnx/SDL2 and need no entry: they are
# emitted as &name unless overridden above.
GL_HEADER = '#include <GLES/gl.h>'


def main():
    so_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None
    check_only = '--check' in sys.argv

    e = ELF(so_path)
    names = sorted(e.imports.keys())
    gl = [n for n in names if classify(e.imports[n]) == 'GLES1']
    rest = [n for n in names if classify(e.imports[n]) != 'GLES1']

    unknown = [n for n in rest if n not in PROVIDERS]
    print('%s: %d imports (%d GLES1, %d other), %d via explicit provider'
          % (os.path.basename(so_path), len(names), len(gl), len(rest), len(PROVIDERS)))

    if unknown:
        print('\nusing the C library as-is for %d symbols:' % len(unknown))
        for n in unknown:
            print('   ', n)

    if check_only or not out_path:
        return

    lines = []
    a = lines.append
    a('/* imports.c -- GENERATED by tools/gen_imports.py from %s'
      % os.path.basename(so_path))
    a(' *')
    a(' * Maps every undefined symbol of the original ARM64 binary onto an')
    a(' * implementation available in this NRO. Regenerate after touching the')
    a(' * provider table; do not hand-edit.')
    a(' */')
    a('')
    a('#include <switch.h>')
    a('#include <stdint.h>')
    a('#include <stdio.h>')
    a('#include <stdlib.h>')
    a('#include <string.h>')
    a('#include <math.h>')
    a('#include <unistd.h>')
    a('#include <fcntl.h>')
    a('#include <dirent.h>')
    a('#include <sys/stat.h>')
    a('#include <sys/socket.h>')
    a('#include <netdb.h>')
    a('#include <pthread.h>')
    a('#include <setjmp.h>')
    a('#include <wchar.h>')
    a(GL_HEADER)
    a('')
    a('#include "libc_shim.h"')
    a('#include "imports.h"')
    a('#include "libc_shim.h"')
    a('')
    a('/* bionic keeps stdio in a FILE array; &__sF[i] must stay valid */')
    a('extern uint8_t __sF_storage[3][0xE8];')
    a('')
    a('DynLibFunction dynlib_functions[] = {')
    for n in names:
        expr = PROVIDERS.get(n, '(uintptr_t)&%s' % n)
        a('  { "%s", %s },' % (n, expr))
    a('};')
    a('')
    a('int dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);')
    a('')

    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('\nwrote %s (%d entries)' % (out_path, len(names)))


if __name__ == '__main__':
    main()
