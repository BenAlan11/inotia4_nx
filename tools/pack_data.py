#!/usr/bin/env python3
"""Turn Inotia4.apk + build/inotia4_nx.nro into a ready-to-copy SD card tree.

    python tools/pack_data.py                    # -> dist/
    python tools/pack_data.py --out D:/sd        # writes D:/sd/switch/inotia4/
    python tools/pack_data.py --apk other.apk --no-nro

Layout produced (matches the absolute paths libgame.so was compiled against,
see src/libc_shim.c for the rewrite rules):

    switch/inotia4/
      inotia4_nx.nro
      libgame.so                <- lib/arm64-v8a from the APK
      libHercules.so
      config.ini
      data/
        resources/common/game_res/...    <- assets/common/game_res, de-obfuscated
        res/H7.mp4                       <- intro movie (res/raw in the APK)
      save/

Why the names change
--------------------
Every file in the APK's assets tree carries a bogus ".jpg" tail: game.dat is
shipped as "game.dat.jpg", font.fnt as "font.fnt.jpg", and the BGM/SE ogg
streams as "BGM05.jpg".  libgame.so contains no "jpg" literal at all, so the
engine asks for the real names and the tail is pure asset-ripping camouflage.
We strip it.  Files that would end up with no extension at all (the sound
banks) are also written a second time under the original name, because the
sound index that feeds "SOUND/BGM/%s" lives in game.dat and we cannot yet
prove which spelling it holds.
"""
import argparse
import os
import shutil
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PKG = 'com.com2us.inotia4.normal.freefull.google.global.android.common'
ASSET_ROOT = 'assets/common/game_res/'
MASK_EXT = '.jpg'

CONFIG_INI = """\
; Inotia 4 for Nintendo Switch -- runtime configuration
; Edit freely; nothing here requires rebuilding the NRO.
; Unknown keys are ignored, missing keys fall back to the defaults below.

[display]
; 0 = follow the console (1280x720 handheld, see dock_1080p when docked)
; or pin an explicit target, e.g. 1280x720 or 960x540 for more speed
screen_width = 0
screen_height = 0
; 1920x1080 while docked. Mesa's software rasteriser is fill-rate bound, so
; set this to 0 if the frame rate drops in TV mode.
dock_1080p = 1

[game]
; 0 = let the engine use the console language, otherwise the engine's own
; language index (1 en, 2 jp, 3 fr, 4 de, 5 zhhans, 6 zhhant)
language = 0
; the port has no h264 decoder, so the H7.mp4 intro can only be skipped
skip_intro = 1

[audio]
; audout output rate; 48000 matches the game's ogg assets
audio_frequency = 48000
; buffer length in samples (256..4096). Raise it if the music stutters,
; lower it if the sound lags behind the picture.
audio_latency = 1024
music_volume = 80
sfx_volume = 90

[input]
; The game is touch-only, so the pad drives a virtual finger: the stick moves a
; crosshair, A (or ZR) presses where it is, B releases. A real finger on the
; touchscreen is forwarded straight through. inject = 0 turns all of that off.
inject = 1
; 1 draws the focus box / crosshair; the real touchscreen is the primary
; pointer, so this is off unless you are playing on the pad alone.
overlay = 0
; virtual stick origin and action-spot distance, percent of the view
touch_x_pct = 12
touch_y_pct = 78
button_pct = 18

[debug]
; 0 off, 1 startup + errors, 2 + every file the engine opens, 3 everything
log_level = 1
show_fps = 0
; libHercules.so is only loaded by the Java shell, and 78 of its imports have no
; stand-in here, so it stays out unless you are specifically chasing it
load_hercules = 0
; Com2uS's Protection layer checks processes and calls s.com2us.net; offline it
; always fails and the failure path is "show dialog and exit". Keep it off.
skip_drm = 1

[paths]
; override only if you move things around; relative names are resolved
; against the folder holding the NRO
; data_dir = sdmc:/switch/inotia4/data
; save_dir = sdmc:/switch/inotia4/save
"""


def strip_mask(name):
    """Drop the camouflage suffix; returns (real_name, had_suffix)."""
    if name.endswith(MASK_EXT) and len(name) > len(MASK_EXT):
        return name[:-len(MASK_EXT)], True
    return name, False


def has_extension(name):
    base = name.rsplit('/', 1)[-1]
    return '.' in base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default=os.path.join(
        os.path.expanduser('~'), 'Downloads', 'Inotia4.apk'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--nro', default=os.path.join(ROOT, 'build', 'inotia4_nx.nro'))
    ap.add_argument('--no-nro', action='store_true')
    ap.add_argument('--no-hercules', action='store_true')
    a = ap.parse_args()

    if not os.path.exists(a.apk):
        sys.exit('APK not found: %s' % a.apk)
    z = zipfile.ZipFile(a.apk)

    dest = os.path.join(a.out, 'switch', 'inotia4')
    data = os.path.join(dest, 'data')
    gres = os.path.join(data, 'resources', 'common', 'game_res')
    for d in (dest, gres, os.path.join(dest, 'save'), os.path.join(data, 'res')):
        os.makedirs(d, exist_ok=True)

    # ---- assets ---------------------------------------------------------
    n_main = n_alias = 0
    raw_bytes = out_bytes = 0
    for info in z.infolist():
        n = info.filename
        if not n.startswith(ASSET_ROOT) or n.endswith('/'):
            continue
        rel = n[len(ASSET_ROOT):]
        real, masked = strip_mask(rel)
        blob = z.read(info)
        out = os.path.join(gres, real.replace('/', os.sep))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(blob)
        n_main += 1
        raw_bytes += len(blob)
        out_bytes += len(blob)
        # sound banks: keep the masked spelling too, see module docstring
        if masked and not has_extension(real):
            with open(os.path.join(gres, rel.replace('/', os.sep)), 'wb') as f:
                f.write(blob)
            n_alias += 1
            out_bytes += len(blob)

    # ---- intro movie ----------------------------------------------------
    movie = 0
    for cand in ('res/H7.mp4', 'res/raw/H7.mp4'):
        if cand in z.namelist():
            blob = z.read(cand)
            with open(os.path.join(data, 'res', 'H7.mp4'), 'wb') as f:
                f.write(blob)
            movie = len(blob)
            out_bytes += len(blob)
            break

    # ---- native libraries ----------------------------------------------
    libs = []
    for name in ('libgame.so', 'libHercules.so'):
        src = os.path.join(ROOT, 'data', name)
        if not os.path.exists(src):
            sys.exit('missing %s (extract it from the APK first)' % src)
        shutil.copyfile(src, os.path.join(dest, name))
        libs.append((name, os.path.getsize(src)))
        out_bytes += os.path.getsize(src)

    # ---- nro ------------------------------------------------------------
    nro_size = 0
    if not a.no_nro:
        if not os.path.exists(a.nro):
            sys.exit('NRO not built yet: %s (run build.py)' % a.nro)
        shutil.copyfile(a.nro, os.path.join(dest, 'inotia4_nx.nro'))
        nro_size = os.path.getsize(a.nro)
        out_bytes += nro_size

    # ---- config ---------------------------------------------------------
    cfg = os.path.join(dest, 'config.ini')
    if not os.path.exists(cfg):
        with open(cfg, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(CONFIG_INI)

    # ---- report ---------------------------------------------------------
    print('SD card tree -> %s' % dest)
    print('  assets      %5d files (+%d alias)  %.1f MB'
          % (n_main, n_alias, raw_bytes / 1048576))
    print('  intro movie %s' % ('%.0f KB' % (movie / 1024) if movie else 'MISSING'))
    for nm, sz in libs:
        print('  %-12s %.1f MB' % (nm, sz / 1048576))
    if nro_size:
        print('  %-12s %.1f MB' % ('inotia4_nx.nro', nro_size / 1048576))
    print('  total       %.1f MB' % (out_bytes / 1048576))

    # ---- sanity: does every .dat the engine names exist? ---------------
    import re
    so = open(os.path.join(dest, 'libgame.so'), 'rb').read()
    wanted = set()
    for m in re.finditer(rb'[\x20-\x7e]{3,64}\.(?:dat|fnt|store)\x00', so):
        s = m.group()[:-1].decode()
        if '%' in s:
            continue          # format string, expanded at runtime
        wanted.add(s)
    missing = sorted(w for w in wanted if not os.path.exists(os.path.join(gres, w)))
    present = len(wanted) - len(missing)
    print('  self-check: %d/%d literal .dat/.fnt names present' % (present, len(wanted)))
    if missing:
        print('    not shipped (generated at runtime or optional): %s'
              % ', '.join(missing))
    return 0


if __name__ == '__main__':
    sys.exit(main())
