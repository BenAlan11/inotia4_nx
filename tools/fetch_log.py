#!/usr/bin/env python3
"""Collect the port's diagnostic files into one zip that is easy to send.

Where the files live depends on how the port was run, so this looks in both
places: a mounted SD card (any drive letter that has switch/inotia4 or
atmosphere) and the PC emulator's virtual card (%APPDATA%\\<emu>\\sdmc).

    python tools/fetch_log.py                 find both, collect, zip
    python tools/fetch_log.py -d E:           use a specific card
    python tools/fetch_log.py -o out.zip      choose the output name
    python tools/fetch_log.py --show          just print the summary

It pulls, when present:

    switch/inotia4/debug.log        this run, flushed line by line
    switch/inotia4/debug.prev.log   the run before it
    switch/inotia4/fatal.txt        written by the port's own error path
    switch/inotia4/config.ini       so the settings are known
    atmosphere/crash_reports/*.log  the system's exception report (PC/LR)
"""
import argparse
import glob
import os
import shutil
import string
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU_ROOTS = ['eden', 'yuzu', 'suyu', 'sudachi', 'citron']


def looks_like_card(path):
    return (os.path.isdir(os.path.join(path, 'switch')) or
            os.path.isdir(os.path.join(path, 'atmosphere')) or
            os.path.isdir(os.path.join(path, 'Nintendo')))


def find_cards():
    """Every root that might hold the port's folder."""
    found = []
    seen = set()

    def add(p):
        if p and os.path.isdir(p) and looks_like_card(p):
            key = os.path.normcase(os.path.abspath(p))
            if key not in seen:
                seen.add(key)
                found.append(p)

    for letter in string.ascii_uppercase:
        add('%s:\\' % letter)

    appdata = os.environ.get('APPDATA')
    if appdata:
        for name in EMU_ROOTS:
            add(os.path.join(appdata, name, 'sdmc'))

    return found


def collect(card, dest):
    """Copy the interesting files out of one card root. Returns what was taken."""
    took = []

    def copy(path, tag):
        if not os.path.isfile(path):
            return
        out = os.path.join(dest, tag)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copy2(path, out)
        took.append((tag, os.path.getsize(out)))

    game = os.path.join(card, 'switch', 'inotia4')
    for name in ('debug.log', 'debug.prev.log', 'fatal.txt', 'config.ini'):
        copy(os.path.join(game, name), name)

    # the system's own report: this is the only place the faulting PC/LR shows up
    reports = sorted(glob.glob(os.path.join(card, 'atmosphere', 'crash_reports', '*.log')),
                     key=os.path.getmtime, reverse=True)[:3]
    for i, r in enumerate(reports):
        copy(r, 'crash_report_%d_%s' % (i, os.path.basename(r)))

    return took


def summarise(dest):
    """Print the parts that decide where the run stopped."""
    log = os.path.join(dest, 'debug.log')
    if not os.path.isfile(log):
        print('  no debug.log in this card -- the port never started, or it is')
        print('  looking at the wrong folder')
        return

    lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
    build = next((l for l in lines if l.startswith('port: build')), '(no build stamp)')
    fatal = [l for l in lines if 'FATAL' in l or 'error' in l.lower()]
    unresolved = [l for l in lines if 'missing imports' in l and not l.startswith('libgame')]

    print('  %s' % build)
    print('  %d lines' % len(lines))
    if unresolved:
        print('  %s' % unresolved[-1].strip())

    print('  --- last 10 lines ---')
    for l in lines[-10:]:
        print('    ' + l)

    if fatal:
        print('  --- errors ---')
        for l in fatal[-5:]:
            print('    ' + l)

    # a step without its "-> ok" line is where the process died
    steps = [l for l in lines if l.startswith('main: ')]
    if steps:
        last = steps[-1]
        died = not last.endswith('ok') and '->' not in last
        print('  --- verdict ---')
        print('    last step: %s' % last.replace('main: ', ''))
        if died:
            print('    it did not come back from that call')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-d', '--dir', help='card root or the switch/inotia4 folder')
    ap.add_argument('-o', '--out', help='zip to write')
    ap.add_argument('--show', action='store_true', help='do not write a zip')
    a = ap.parse_args()

    cards = [a.dir] if a.dir else find_cards()
    cards = [c for c in cards if c]
    if not cards:
        print('no card found. Plug the SD in, or pass -d <drive>:')
        return 1

    stamp = time.strftime('%Y%m%d-%H%M%S')
    dest = os.path.join(ROOT, 'logs', stamp)
    os.makedirs(dest, exist_ok=True)

    total = 0
    for card in cards:
        # a card root given as .../switch/inotia4 is accepted too
        base = card
        if os.path.basename(os.path.normpath(card)) == 'inotia4':
            base = os.path.dirname(os.path.dirname(os.path.normpath(card)))
        took = collect(base, dest)
        if took:
            print('%s:' % base)
            for tag, size in took:
                print('  %-40s %8d bytes' % (tag, size))
            total += sum(s for _, s in took)

    if not total:
        print('found %s but nothing to collect' % ', '.join(cards))
        return 1

    summarise(dest)

    if a.show:
        return 0

    out = a.out or os.path.join(ROOT, 'logs', 'inotia4-report-%s.zip' % stamp)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(dest)):
            z.write(os.path.join(dest, name), name)
    print('\nwrote %s (%d bytes)' % (out, os.path.getsize(out)))
    print('send that file and the crash is usually identifiable from the last step')
    return 0


if __name__ == '__main__':
    sys.exit(main())
