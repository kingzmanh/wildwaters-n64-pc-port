"""Coverage map of the ROM's leftover area (8-16 MB): what every 2 KB block most likely is.

  python -I tools/stale_census.py baserom.z64 out.md [start end]

Classes (first that fits):
  fill         all 00 / all FF
  ww_copy      the block's 64-byte pieces occur in the live first 8 MB (this WildWaters build)
  libmus       inside a libmus ptr bank header / wave table range (given below)
  dl           F3DEX2 display list commands (8-byte aligned, known opcodes)
  code         MIPS instructions (common opcodes, jr ra / jal density)
  floats       mostly plausible IEEE floats
  vtx          F3DEX2 vertices (16-byte records, s16 xyz, flag 0, s16 st, rgba)
  rgba16       16-bit texels (smooth neighbours, alpha bit mostly set)
  adpcm        VADPCM-looking data (9-byte frames, header byte nibble predictor < 8)
  other
Consecutive blocks of one class are merged.
"""
import struct
import sys
from collections import Counter

BLK = 0x800

F3D_OPS = {0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE1, 0xE2,
           0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3,
           0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0x00}


def is_code(b):
    ws = struct.unpack(">%dI" % (len(b) // 4), b)
    good = 0
    ra = 0
    for w in ws:
        op = w >> 26
        if w == 0x03E00008:
            ra += 1
        if op in (0x0F, 0x09, 0x23, 0x2B, 0x03, 0x02, 0x04, 0x05, 0x31, 0x39, 0x21, 0x25, 0x24, 0x28, 0x29, 0x0D, 0x0C, 0x0A, 0x01, 0x11):
            good += 1
        elif op == 0 and (w & 0x3F) in (0x21, 0x23, 0x25, 0x24, 0x2A, 0x2B, 0x00, 0x08, 0x09, 0x10, 0x12, 0x18, 0x19, 0x1A, 0x1B, 0x02, 0x03, 0x04, 0x06, 0x26, 0x27) and w != 0:
            good += 1
    return good / len(ws) > 0.75 and ra >= 1


def is_dl(b):
    n = len(b) // 8
    ok = 0
    for i in range(n):
        op = b[i * 8]
        if op in F3D_OPS and op != 0x00:
            ok += 1
    return ok / n > 0.85


def is_floats(b):
    ws = struct.unpack(">%dI" % (len(b) // 4), b)
    ok = sum(1 for w in ws if w == 0 or 0x30 <= ((w >> 23) & 0xFF) <= 0x8A)
    nz = sum(1 for w in ws if w)
    return nz > len(ws) * 0.5 and ok / len(ws) > 0.9 and sum(1 for w in ws if (w & 0xFFFF) == 0 and w) < len(ws) * 0.6


def is_vtx(b):
    n = len(b) // 16
    ok = 0
    for i in range(n):
        x, y, z, flag = struct.unpack(">hhhH", b[i * 16:i * 16 + 8])
        if flag == 0 and (x or y or z) and max(abs(x), abs(y), abs(z)) < 32000:
            ok += 1
    return ok / n > 0.8


def is_rgba16(b):
    a = struct.unpack(">%dH" % (len(b) // 2), b)
    if len(set(a)) < 8:
        return False
    alpha = sum(v & 1 for v in a) / len(a)
    d = 0
    for i in range(1, len(a)):
        p, q = a[i - 1], a[i]
        d += abs(((p >> 11) & 31) - ((q >> 11) & 31)) + abs(((p >> 6) & 31) - ((q >> 6) & 31)) + abs(((p >> 1) & 31) - ((q >> 1) & 31))
    return alpha > 0.9 and d / len(a) < 9


def is_adpcm(b):
    # header bytes every 9 bytes: predictor nibble small; try all 9 phases
    best = 0
    for ph in range(9):
        hs = b[ph::9]
        best = max(best, sum(1 for h in hs if (h & 0xF) < 4 and (h >> 4) < 13) / len(hs))
    return best > 0.97


def main():
    rom = open(sys.argv[1], "rb").read()
    start = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x800000
    end = int(sys.argv[4], 16) if len(sys.argv) > 4 else len(rom)
    live = set()
    for o in range(0x1000, 0x800000 - 64, 16):
        live.add(rom[o:o + 64])
    rows = []
    for o in range(start, end, BLK):
        b = rom[o:o + BLK]
        if b.count(0) == len(b) or b.count(0xFF) == len(b):
            c = "fill"
        else:
            hits = sum(1 for k in range(0, BLK - 64, 16) if any(rom[o + k + s:o + k + s + 64] in live for s in (0, 4, 8, 12)))
            if hits > (BLK // 16) * 0.5:
                c = "ww_copy"
            elif is_code(b):
                c = "code"
            elif is_dl(b):
                c = "dl"
            elif is_vtx(b):
                c = "vtx"
            elif is_floats(b):
                c = "floats"
            elif is_rgba16(b):
                c = "rgba16"
            elif is_adpcm(b):
                c = "adpcm"
            else:
                c = "other"
        rows.append((o, c))
    merged = []
    for o, c in rows:
        if merged and merged[-1][2] == c:
            merged[-1][1] = o + BLK
        else:
            merged.append([o, o + BLK, c])
    tot = Counter()
    for a, b2, c in merged:
        tot[c] += b2 - a
    with open(sys.argv[2], "w") as f:
        f.write("# Leftover area census 0x%X-0x%X (2 KB blocks)\n\n" % (start, end))
        f.write("| class | KB |\n|---|---|\n" + "".join("| %s | %d |\n" % (c, n // 1024) for c, n in tot.most_common()))
        f.write("\n| start | end | KB | class |\n|---|---|---|---|\n")
        for a, b2, c in merged:
            f.write("| 0x%07X | 0x%07X | %d | %s |\n" % (a, b2, (b2 - a) // 1024, c))
    print(tot.most_common())
    print(len(merged), "runs")


if __name__ == "__main__":
    main()
