"""Decodes the menu/screen records left in the Destruction Derby 64 data (same UI engine as
WildWaters: records start with a 0x03xx0000 type word; WildWaters' loader is func_8003456C).

  python -I tools/dd64_records.py baserom.z64 OUT.md OUT.csv

Record sizes in the DD64 data (found from the spacing of consecutive type words):
  0x03EF  40 bytes  FONT TABLE (one run at 0xBCB6F8): id, 2 pointers, ?, width, height, ...
  0x03F0  36 bytes  text/font element: element id, ?, code pointer, ?, font id, descriptor id, packed (x, y), flags
  0x03F1  72 bytes  button/sprite element: element id, kind, code pointer, ?, picture id, descriptor id,
                    x (float), y (float), ..., scale x, scale y (floats), ...
  0x03F2  28 bytes  PICTURE TABLE (one run at 0xBCBA40): kind (1/2), picture id, width, height, 0,
                    format (2 RGBA16, 3 RGBA32); the pictures lie back to back in this order from 0x8CB3F0
                    (tools/dd64_pictures.py cuts them)
  others (0x0300, 0x0301, 0x0400) are listed raw.
A run of records of one type with nothing in between is one screen (one asset in the original).
"""
import struct
import sys

SIZES = {0x3EF: 0x28, 0x3F0: 0x24, 0x3F1: 0x48, 0x3F2: 0x1C}
RANGES = [(0x8CB2E0, 0xBF0170), (0xFC2000, 0xFEE170)]


def f32(b):
    return struct.unpack(">f", b)[0]


def main():
    rom = open(sys.argv[1], "rb").read()
    screens = []
    for a, b in RANGES:
        o = a - a % 4
        while o < b - 4:
            t = struct.unpack(">I", rom[o:o + 4])[0]
            typ = t >> 16
            if (t & 0xFFFF) == 0 and typ in SIZES:
                st = SIZES[typ]
                recs = []
                p = o
                while p + st <= b and struct.unpack(">I", rom[p:p + 4])[0] == t:
                    recs.append(rom[p:p + st])
                    p += st
                if len(recs) >= 1:
                    screens.append((o, typ, recs))
                    o = p
                    continue
            o += 4
    md = ["# Destruction Derby 64 menu/screen records\n",
          "Same UI engine as WildWaters (loader func_8003456C). Picture/font ids are DD64 asset ids; the asset "
          "directory that maps them to data is gone, so ids are not linked to the carved pictures.\n"]
    csv = ["rom,screen,type,index,element_id,kind,picture_id,descriptor_id,x,y,scale_x,scale_y,extra"]
    sid = 0
    for o, typ, recs in screens:
        sid += 1
        md.append("\n## Screen %d - ROM 0x%07X - type 0x%X, %d records\n" % (sid, o, typ, len(recs)))
        if typ == 0x3F2:
            md.append("Picture table (sizes of the pictures, in storage order).\n\n| # | kind | picture id | width | height | ? | format |\n|---|---|---|---|---|---|---|\n")
            for i, r in enumerate(recs):
                _, kind, pid, x, y, u, layer = struct.unpack(">7I", r)
                md.append("| %d | %d | 0x%X | %d | %d | %d | %d |\n" % (i, kind, pid, x, y, u, layer))
                csv.append("0x%X,%d,0x3F2,%d,,%d,0x%X,,%d,%d,,,%d/%d" % (o + i * 28, sid, i, kind, pid, x, y, u, layer))
        elif typ == 0x3F1:
            md.append("| # | element | kind | code | picture id | descriptor | x | y | scale | raw tail |\n|---|---|---|---|---|---|---|---|---|---|\n")
            for i, r in enumerate(recs):
                w = struct.unpack(">18I", r)
                x, y, sx, sy = f32(r[28:32]), f32(r[32:36]), f32(r[44:48]), f32(r[48:52])
                tail = " ".join("%08x" % v for v in w[13:])
                md.append("| %d | 0x%X | %d | 0x%08X | 0x%X | 0x%X | %.1f | %.1f | %.2f x %.2f | %s |\n"
                          % (i, w[1], w[2], w[3], w[5], w[6], x, y, sx, sy, tail))
                csv.append("0x%X,%d,0x3F1,%d,0x%X,%d,0x%X,0x%X,%.1f,%.1f,%.2f,%.2f,%s" % (o + i * 72, sid, i, w[1], w[2], w[5], w[6], x, y, sx, sy, tail))
        elif typ == 0x3F0:
            md.append("| # | element | code | font id | descriptor | x | y | flags |\n|---|---|---|---|---|---|---|---|\n")
            for i, r in enumerate(recs):
                w = struct.unpack(">9I", r)
                x, y = w[7] >> 16, w[7] & 0xFFFF
                md.append("| %d | 0x%X | 0x%08X | 0x%X | 0x%X | %d | %d | %08x |\n" % (i, w[1], w[3], w[5], w[6], x, y, w[8]))
                csv.append("0x%X,%d,0x3F0,%d,0x%X,,0x%X,0x%X,%d,%d,,,%08x" % (o + i * 36, sid, i, w[1], w[5], w[6], x, y, w[8]))
        elif typ == 0x3EF:
            md.append("| # | font id | pointers | width | height | raw |\n|---|---|---|---|---|---|\n")
            for i, r in enumerate(recs):
                w = struct.unpack(">10I", r)
                md.append("| %d | 0x%X | 0x%08X 0x%08X | %d | %d | %s |\n" % (i, w[1], w[2], w[3], w[5], w[6], " ".join("%08x" % v for v in (w[4],) + w[7:])))
                csv.append("0x%X,%d,0x3EF,%d,,,0x%X,,,,%d,%d," % (o + i * 40, sid, i, w[1], w[5], w[6]))
    open(sys.argv[2], "w", encoding="utf-8").write("".join(md))
    open(sys.argv[3], "w", encoding="utf-8").write("\n".join(csv) + "\n")
    from collections import Counter
    print(len(screens), "screens", Counter(t for _, t, _ in screens), sum(len(r) for _, _, r in screens), "records")


if __name__ == "__main__":
    main()
