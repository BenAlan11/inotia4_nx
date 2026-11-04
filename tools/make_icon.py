"""Extract the game's own launcher icon from the APK and emit a 256x256 JPEG.

NRO icons must be exactly 256x256 JPEG with no alpha channel. The APK ships the
icon as a mipmap set; the xxhdpi variant (192x192) is the largest square one, so
upscaling from 256/192 = 1.33x is mild enough to stay clean.
"""
import io
import struct
import sys
import zipfile

from PIL import Image

sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from axml import arsc_find, read_pool, xml_attrs

APK = r'C:/Users/BenAlan/Downloads/Inotia4.apk'
OUT = 'icon.jpg'
SIZE = 256


def icon_resource_names(apk):
    """Yield every (density, path) the launcher icon resolves to."""
    z = zipfile.ZipFile(apk)
    res_id = None
    for name, attrs in xml_attrs(z.read('AndroidManifest.xml')):
        if name == 'application' and 'icon' in attrs:
            res_id = attrs['icon'][1]
            break
    if res_id is None:
        raise SystemExit('AndroidManifest has no android:icon')

    d = z.read('resources.arsc')
    _, hsz, total = struct.unpack_from('<HHI', d, 0)
    pool = read_pool(d, hsz)
    type_id = (res_id >> 16) & 0xFF
    entry_id = res_id & 0xFFFF

    found = []
    off = hsz
    while off < total:
        ctyp, chsz, csz = struct.unpack_from('<HHI', d, off)
        if csz == 0:
            break
        if ctyp == 0x0200:
            io = off + chsz
            end = off + csz
            while io < end:
                it, ih, isz = struct.unpack_from('<HHI', d, io)
                if isz == 0:
                    break
                if it == 0x0201 and d[io + 8] == type_id:
                    ecount, estart = struct.unpack_from('<II', d, io + 12)
                    density = struct.unpack_from('<H', d, io + 20 + 14)[0]
                    for i in range(min(ecount, entry_id + 1)):
                        if i != entry_id:
                            continue
                        e, = struct.unpack_from('<I', d, io + ih + i * 4)
                        doff = io + estart + (0 if e == 0xFFFFFFFF else e)
                        esz = struct.unpack_from('<H', d, doff)[0]
                        if d[doff + esz + 3] == 0x03:
                            found.append((density, pool[struct.unpack_from('<I', d, doff + esz + 4)[0]]))
                io += isz
        off += csz
    return z, found


def main():
    z, cands = icon_resource_names(APK)
    best = None
    for density, path in cands:
        if path.endswith('.xml'):
            continue
        try:
            data = z.read(path)
        except KeyError:
            continue
        im = Image.open(io.BytesIO(data)).convert('RGBA')
        if im.width != im.height:
            continue
        if best is None or im.width > best[0].width:
            best = (im, density, path)
    if best is None:
        raise SystemExit('no square icon found')
    im, density, path = best
    print('icon: %s  %dx%d  density=0x%x' % (path, im.width, im.height, density))

    # the only transparent pixels are the rounded corners; JPEG has no alpha,
    # so flatten onto black, which the dark stone artwork blends into anyway
    flat = Image.new('RGBA', im.size, (0, 0, 0, 255))
    flat.alpha_composite(im)

    out = Image.new('RGB', (SIZE, SIZE), (0, 0, 0))
    rs = flat.convert('RGB').resize((SIZE, SIZE), Image.LANCZOS)
    out.paste(rs, (0, 0))
    out.save(OUT, 'JPEG', quality=95, subsampling=0)
    print('wrote %s (%dx%d, %d bytes)' % (OUT, SIZE, SIZE, __import__('os').path.getsize(OUT)))


if __name__ == '__main__':
    main()
