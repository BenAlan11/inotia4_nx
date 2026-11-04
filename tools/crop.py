"""Crop a region out of a PPM/PNG screenshot and write it as PNG.

    python tools/crop.py in.ppm out.png X Y W H [SCALE]

X/Y are pixels from the TOP-LEFT of the image (PPM rows are already stored
top-down), W/H the crop size, and SCALE an integer nearest-neighbour zoom so a
20px line of text is readable when the picture is looked at.
"""

import struct
import sys
import zlib

from ppm2png import read_ppm, write_png


def main(argv):
    src, dst = argv[1], argv[2]
    x0, y0, cw, ch = (int(v) for v in argv[3:7])
    scale = int(argv[7]) if len(argv) > 7 else 1

    w, h, rgb = read_ppm(src)
    x0 = max(0, min(w - 1, x0))
    y0 = max(0, min(h - 1, y0))
    cw = max(1, min(w - x0, cw))
    ch = max(1, min(h - y0, ch))

    ow, oh = cw * scale, ch * scale
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        sy = y0 + y // scale
        srow = sy * w * 3
        drow = y * ow * 3
        for x in range(ow):
            so = srow + (x0 + x // scale) * 3
            do = drow + x * 3
            out[do] = rgb[so]
            out[do + 1] = rgb[so + 1]
            out[do + 2] = rgb[so + 2]
    write_png(dst, ow, oh, out)
    print("%s [%d,%d %dx%d] -> %s (%dx%d)" % (src, x0, y0, cw, ch, dst, ow, oh))


if __name__ == "__main__":
    main(sys.argv)
