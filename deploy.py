#!/usr/bin/env python3
"""Build the NRO, refresh the SD layout and push it to a running console.

The point of this script is to remove the "copy to SD card, walk to the console,
pick the file again, wait for the crash" loop. With hbmenu parked in netloader
mode (Y on the main menu) every iteration is one command: the NRO is pushed over
the network, launched, and the console's stdout is streamed back here.

    python deploy.py                 build, pack, push, follow the log
    python deploy.py --watch         same, and repeat whenever a source changes
    python deploy.py -a 192.168.1.9  skip the broadcast search
    python deploy.py --eden          copy the SD layout into the PC emulator
    python deploy.py --sd            copy onto a mounted console SD card
    python deploy.py --no-build      just push what is already in build/

Press Y in hbmenu first. The console is never power cycled: when the NRO exits
or crashes, hbmenu is back, still listening, ready for the next push.
"""
import argparse
import os
import socket
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
NXLINK = os.path.join(ROOT, 'tools', 'bin', 'nxlink.exe')
NRO = os.path.join(ROOT, 'build', 'inotia4_nx.nro')
LIVE_LOG = os.path.join(ROOT, 'build', 'live.log')
DIST = os.path.join(ROOT, 'dist', 'switch', 'inotia4')

# yuzu-lineage emulators keep the console's SD card as a plain folder; Eden,
# suyu, sudachi and yuzu all agree on <appdata>/<emu>/sdmc.
EMU_ROOTS = ['eden', 'yuzu', 'suyu', 'sudachi', 'citron']


def emu_sdmc():
    appdata = os.environ.get('APPDATA') or os.path.expanduser('~/.config')
    for name in EMU_ROOTS:
        p = os.path.join(appdata, name, 'sdmc')
        if os.path.isdir(p):
            return p, name
    return None, None


def sh(cmd):
    p = subprocess.run(cmd, cwd=ROOT)
    if p.returncode != 0:
        raise SystemExit('%s failed (exit %d)' % (cmd[0], p.returncode))


def build():
    sh([sys.executable, 'build.py'])


def pack():
    sh([sys.executable, os.path.join('tools', 'pack_data.py')])


def discover(timeout=2.0):
    """Ask netloader where it is.

    hbmenu answers the 6-byte "nxboot" datagram with "bootnx", sent to *port*
    28771 rather than back to the source port, which is why a plain UDP probe
    looks dead. Returns the console's address, or None.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    try:
        s.bind(('', 28771))
        s.sendto(b'nxboot', ('255.255.255.255', 28280))
        data, addr = s.recvfrom(64)
        return addr[0] if data.startswith(b'bootnx') else None
    except OSError:
        return None
    finally:
        s.close()


def wait_for_console(seconds):
    print('waiting up to %d s for netloader (press Y in hbmenu)...' % seconds)
    end = time.time() + seconds
    while time.time() < end:
        ip = discover()
        if ip:
            print('found console at %s' % ip)
            return ip
        time.sleep(2)
    return None


def push(ip=None, follow=True):
    if not os.path.exists(NXLINK):
        raise SystemExit('tools/bin/nxlink.exe is missing; run tools/build_nxlink.py')
    if not os.path.exists(NRO):
        raise SystemExit('%s does not exist; build first' % NRO)

    cmd = [NXLINK]
    if ip:
        cmd += ['-a', ip]
    if follow:
        cmd += ['-s']
    cmd.append(NRO)

    print('>>> ' + ' '.join(cmd))
    env = dict(os.environ, PYTHONUNBUFFERED='1')
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1, env=env)
    os.makedirs(os.path.dirname(LIVE_LOG), exist_ok=True)
    with open(LIVE_LOG, 'w', encoding='utf-8', errors='replace') as log:
        for raw in proc.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            print('  ' + line)
            log.write(line + '\n')
            log.flush()          # the last line is the interesting one on a crash
    return proc.wait()


def sources():
    """Every file whose change should trigger a rebuild."""
    seen = {}
    for base, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in files:
            if f.endswith(('.c', '.h')):
                p = os.path.join(base, f)
                seen[p] = os.path.getmtime(p)
    seen[os.path.join(ROOT, 'build.py')] = os.path.getmtime(os.path.join(ROOT, 'build.py'))
    return seen


def watch(ip):
    stamps = sources()
    print('watching %d files; Ctrl-C to stop' % len(stamps))
    while True:
        time.sleep(0.6)
        now = sources()
        changed = [p for p, t in now.items() if stamps.get(p) != t]
        stamps = now
        if not changed:
            continue
        print('\n=== %s ===' % ', '.join(os.path.basename(p) for p in changed[:4]))
        try:
            build()
        except SystemExit as e:
            print(e)
            continue          # stay in the loop: fix the error and save again
        push(ip)


def find_sd():
    """A mounted console card: FAT32 root with switch/ or atmosphere/ on it."""
    for letter in 'DEFGHIJKLMNOPQRSTUVWXYZ':
        root = '%s:////' % letter
        if not os.path.isdir(root):
            continue
        if any(os.path.isdir(os.path.join(root, d)) for d in ('switch', 'atmosphere')):
            return root
    return None


def sync_sd(letter=None):
    """Copy dist/switch/inotia4 onto the card, leaving the user's config alone."""
    root = letter or find_sd()
    if not root:
        raise SystemExit('no SD card found; plug it in and pass -d <drive>:')
    if not root.endswith(('/', '\\')):
        root += '\\'
    dst = os.path.join(root, 'switch', 'inotia4')
    if not os.path.isdir(DIST):
        raise SystemExit('%s does not exist; run tools/pack_data.py first' % DIST)

    os.makedirs(dst, exist_ok=True)
    kept = os.path.exists(os.path.join(dst, 'config.ini'))
    copied = 0
    for base, _, files in os.walk(DIST):
        rel = os.path.relpath(base, DIST)
        target = dst if rel == '.' else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for f in files:
            # config.ini may have been tuned on the card; do not clobber it
            if kept and rel == '.' and f == 'config.ini':
                continue
            shutil.copy2(os.path.join(base, f), os.path.join(target, f))
            copied += 1
    print('>>> %s  ->  %s  (%d files%s)'
          % (DIST, dst, copied, ', kept the existing config.ini' if kept else ''))
    print('now start it on the console; the log appears by itself at')
    print('  %s' % os.path.join(dst, 'debug.log'))
    print('then: python tools/fetch_log.py')


def sync_emu():
    """Mirror dist/switch/inotia4 into the emulator's virtual SD card."""
    sdmc, name = emu_sdmc()
    if not sdmc:
        raise SystemExit('no emulator SD card found (looked for %s under %%APPDATA%%)'
                         % '/'.join(EMU_ROOTS))
    if not os.path.isdir(DIST):
        raise SystemExit('%s does not exist; run tools/pack_data.py first' % DIST)

    dst = os.path.join(sdmc, 'switch', 'inotia4')
    os.makedirs(dst, exist_ok=True)
    print('>>> %s  ->  %s' % (DIST, dst))
    # copy tree, overwriting; the 60 MB of assets only move once because
    # shutil skips identical files cheaply enough at this size
    shutil.copytree(DIST, dst, dirs_exist_ok=True)
    total = sum(len(f) for _, _, f in os.walk(dst))
    print('synced %d files' % total)
    print('open it with File -> Load File... (no homebrew menu needed)')
    print('or: eden-cli.exe -g "%s"' % os.path.join(dst, 'inotia4_nx.nro'))
    print('the port writes its log to %s' % os.path.join(dst, 'debug.log'))
    crash = os.path.join(os.path.dirname(sdmc), 'crash_dumps')
    if os.path.isdir(crash):
        print('emulator crash dumps land in %s' % crash)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-a', '--address', help='console IP; omit to broadcast')
    ap.add_argument('--eden', action='store_true',
                    help="copy the SD layout into the PC emulator instead of pushing")
    ap.add_argument('--sd', nargs='?', const=True, metavar='DRIVE',
                    help="copy onto a mounted console SD card (auto-detects the drive)")
    ap.add_argument('-d', '--drive', help='card root, used with --sd')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--no-pack', action='store_true')
    ap.add_argument('--no-follow', action='store_true', help="don't stream stdout")
    ap.add_argument('--watch', action='store_true', help='rebuild on source change')
    ap.add_argument('--wait', type=int, metavar='SECONDS',
                    help='wait for netloader to answer, then push')
    a = ap.parse_args()

    if not a.no_build:
        build()
    if not a.no_pack:
        pack()

    if a.eden:
        sync_emu()
        return

    if a.sd:
        sync_sd(a.drive or (a.sd if a.sd is not True else None))
        return

    if a.watch:
        watch(a.address)
        return

    address = a.address
    if a.wait:
        address = wait_for_console(a.wait)
        if not address:
            raise SystemExit('netloader never answered; is hbmenu on the Y screen?')

    code = push(address, follow=not a.no_follow)
    print('nxlink exited with %d' % code)


if __name__ == '__main__':
    main()
