"""Build a contact sheet out of the PPM screenshots the port writes.

    python tools/sheet.py out.png seq_00.ppm seq_01.ppm ...

Each frame is nearest-neighbour downsampled into a grid tile and tagged with its
index, so a whole capture run can be scanned in one image instead of opening
sixty files. Pure Python + zlib: no Pillow in this environment.
"""

import struct
import sys
import zlib

# 5x7 digits, one string per row, '#' = ink
DIGITS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
}


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("%s: not a binary PPM" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1
    w, h, _ = fields
    return w, h, data[i : i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride : (y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def draw_label(rgb, w, h, x0, y0, text, scale=2):
    """Draw `text` (digits only) with a white 5x7 font on a dark tag."""
    cw, ch = 5 * scale, 7 * scale
    pad = 2
    tw = len(text) * (cw + scale) + pad * 2
    th = ch + pad * 2
    for y in range(y0, min(h, y0 + th)):
        for x in range(x0, min(w, x0 + tw)):
            o = (y * w + x) * 3
            rgb[o] = rgb[o + 1] = rgb[o + 2] = 0
    for ci, c in enumerate(text):
        rows = DIGITS.get(c)
        if not rows:
            continue
        for ry, row in enumerate(rows):
            for rx, bit in enumerate(row):
                if bit != "1":
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        x = x0 + pad + ci * (cw + scale) + rx * scale + dx
                        y = y0 + pad + ry * scale + dy
                        if 0 <= x < w and 0 <= y < h:
                            o = (y * w + x) * 3
                            rgb[o] = rgb[o + 1] = rgb[o + 2] = 255


def main(argv):
    out = argv[1]
    srcs = argv[2:]
    if not srcs:
        raise SystemExit(__doc__)
    w, h, _ = read_ppm(srcs[0])
    step = 6 if w >= 1800 else 3
    tw, th = w // step, h // step
    cols = 6
    rows = (len(srcs) + cols - 1) // cols
    gap = 4
    sw, sh = cols * (tw + gap), rows * (th + gap)
    sheet = bytearray(b"\x20" * (sw * sh * 3))

    for idx, src in enumerate(srcs):
        fw, fh, rgb = read_ppm(src)
        if (fw, fh) != (w, h):
            continue
        cx, cy = idx % cols, idx // cols
        ox, oy = cx * (tw + gap), cy * (th + gap)
        for y in range(th):
            sy = y * step
            srow = sy * fw * 3
            drow = ((oy + y) * sw + ox) * 3
            for x in range(tw):
                so = srow + x * step * 3
                do = drow + x * 3
                sheet[do] = rgb[so]
                sheet[do + 1] = rgb[so + 1]
                sheet[do + 2] = rgb[so + 2]
        draw_label(sheet, sw, sh, ox, oy, str(idx))

    write_png(out, sw, sh, sheet)
    print("%s -> %s (%dx%d, %d frames)" % (srcs[0], out, sw, sh, len(srcs)))


if __name__ == "__main__":
    main(sys.argv)
