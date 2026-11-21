"""Cuts every Destruction Derby 64 picture exactly, using DD64's own tables left in the ROM.

  python -I tools/dd64_pictures.py baserom.z64 OUTDIR

The leftover area holds DD64's picture data back to back, followed by its two tables:
  0x89A780  21 font strips (RGBA16) in the order of the font table
  0x8CB3F0  261 pictures in the order of the picture table (ends exactly at 0xBCB6F8)
  0xBCB6F8  font table: 40-byte 0x03EF records (id, 2 pointers, ?, width, height, ...)
  0xBCBA40  picture table: 28-byte 0x03F2 records (kind, id, width, height, 0, format 2=RGBA16 3=RGBA32)
Writes OUTDIR/fonts/font_<id>_<w>x<h>.png, OUTDIR/pictures/pic_<n>_id<id>_<w>x<h>_<fmt>.png and LIST.md.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

FONT_DATA, PIC_DATA = 0x89A780, 0x8CB3F0
FONT_TABLE, PIC_TABLE = 0xBCB6F8, 0xBCBA40


def rgba16(b, w, h):
    d = np.frombuffer(b, ">u2").astype(np.uint32).reshape(h, w)
    return np.stack([((d >> 11) & 31) * 255 // 31, ((d >> 6) & 31) * 255 // 31, ((d >> 1) & 31) * 255 // 31,
                     (d & 1) * 255], -1).astype(np.uint8)


def rgba32(b, w, h):
    return np.frombuffer(b, np.uint8).reshape(h, w, 4).copy()


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(os.path.join(out, "fonts"), exist_ok=True)
    os.makedirs(os.path.join(out, "pictures"), exist_ok=True)
    rows = ["# Destruction Derby 64 pictures, cut with DD64's own tables\n\n",
            "Exact sizes and ids from the font table (ROM 0x%X) and picture table (ROM 0x%X).\n"
            "Pictures whose alpha is all off are drawn opaque by the game and are saved opaque.\n\n" % (FONT_TABLE, PIC_TABLE),
            "| file | id | ROM | size | format | kind |\n|---|---|---|---|---|---|\n"]
    o, pos = FONT_TABLE, FONT_DATA
    while struct.unpack(">I", rom[o:o + 4])[0] == 0x03EF0000:
        w = struct.unpack(">10I", rom[o:o + 40])
        fid, fw, fh = w[1], w[5], w[6]
        n = fw * fh * 2
        img = rgba16(rom[pos:pos + n], fw, fh)
        name = "fonts/font_%05X_%dx%d.png" % (fid, fw, fh)
        Image.fromarray(img, "RGBA").save(os.path.join(out, name))
        rows.append("| %s | 0x%X | 0x%07X | %dx%d | RGBA16 | font |\n" % (name, fid, pos, fw, fh))
        pos += n
        o += 40
    o, pos, k = PIC_TABLE, PIC_DATA, 0
    while struct.unpack(">I", rom[o:o + 4])[0] == 0x03F20000:
        _, kind, pid, pw, ph, _, fmt = struct.unpack(">7I", rom[o:o + 28])
        bpp = 4 if fmt == 3 else 2
        n = pw * ph * bpp
        img = (rgba32 if bpp == 4 else rgba16)(rom[pos:pos + n], pw, ph)
        if img[..., 3].max() == 0:
            img[..., 3] = 255
        k += 1
        name = "pictures/pic_%03d_id%05X_%dx%d_%s.png" % (k, pid, pw, ph, "rgba32" if bpp == 4 else "rgba16")
        Image.fromarray(img, "RGBA").save(os.path.join(out, name))
        rows.append("| %s | 0x%X | 0x%07X | %dx%d | %s | %d |\n" % (name, pid, pos, pw, ph, "RGBA32" if bpp == 4 else "RGBA16", kind))
        pos += n
        o += 28
    rows.append("\nPicture data ends at ROM 0x%X (the font table starts at 0x%X).\n" % (pos, FONT_TABLE))
    open(os.path.join(out, "LIST.md"), "w", encoding="utf-8").writelines(rows)
    print("fonts + pictures:", len(rows) - 4, "data end", hex(pos))


if __name__ == "__main__":
    main()
