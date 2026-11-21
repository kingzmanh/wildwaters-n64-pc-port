"""Turns the WW_ASSET_DIR captures into a browsable asset folder.

  python -I tools/assets_post.py baserom.z64 ../assets

For each capture folder (assets/capture/<name>):
  - textures/*.rgba -> *.png (RGBA8 dumps from host/src/asset_capture.cpp)
  - every texture is re-encoded to RGBA16 and looked up in the ROM, so it gets its ROM offset
    and, when it lies inside an asset-directory entry, the asset id
Then writes assets/textures/ (one PNG per unique texture, named by ROM offset/asset id, plus
the asset-table textures no capture drew, at guessed sizes) and assets/textures/LIST.md.
"""
import os
import re
import sys
import zlib
import struct

import numpy as np
from PIL import Image


def load_list(path):
    entries = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\| \d+ \| 0x([0-9A-F]+) \| (\w+) .*?\| 0x([0-9A-F]+) \| (\d+) \|", line)
        if m:
            entries.append((int(m.group(1), 16), m.group(2), int(m.group(3), 16), int(m.group(4))))
    return entries


def rgba16(px):
    r = (px[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (px[..., 1].astype(np.uint32) * 31 + 127) // 255
    b = (px[..., 2].astype(np.uint32) * 31 + 127) // 255
    a = (px[..., 3] >= 128).astype(np.uint32)
    return ((r << 11) | (g << 6) | (b << 1) | a).astype(">u2").tobytes()


def decode16(data, w, h):
    a = np.frombuffer(data[: w * h * 2], ">u2").reshape(h, w).astype(np.uint32)
    out = np.zeros((h, w, 4), np.uint8)
    out[..., 0] = ((a >> 11) & 31) * 255 // 31
    out[..., 1] = ((a >> 6) & 31) * 255 // 31
    out[..., 2] = ((a >> 1) & 31) * 255 // 31
    out[..., 3] = (a & 1) * 255
    return out


def main():
    rom = open(sys.argv[1], "rb").read()
    root = sys.argv[2]
    entries = load_list(os.path.join(root, "raw", "LIST.md"))
    def owner(off):
        for i, t, s, n in entries:
            if s <= off < s + n:
                return i, t, off - s
        return None
    cap_root = os.path.join(root, "capture")
    out_dir = os.path.join(root, "textures")
    os.makedirs(out_dir, exist_ok=True)
    unique = {}       # content crc -> info
    drawn_assets = set()
    for name in sorted(os.listdir(cap_root)):
        d = os.path.join(cap_root, name)
        lst = os.path.join(d, "textures.txt")
        if not os.path.exists(lst):
            continue
        for line in open(lst):
            addr, fmt, siz, w, h, tlut, fn = line.split()
            w, h = int(w), int(h)
            src = os.path.join(d, "textures", fn)
            png = src[:-5] + ".png"
            if os.path.exists(src):
                px = np.frombuffer(open(src, "rb").read(), np.uint8).reshape(h, w, 4)
                Image.fromarray(px, "RGBA").save(png)
                os.remove(src)   # intermediate dump, now the PNG
            else:
                px = np.array(Image.open(png).convert("RGBA"))
            enc = rgba16(px)
            crc = zlib.crc32(enc)
            if crc in unique:
                unique[crc]["seen"].add(name)
                continue
            off = rom.find(enc)
            info = {"w": w, "h": h, "ram": int(addr, 16), "rom": off, "seen": {name}, "px": px}
            o = owner(off) if off >= 0 else None
            info["owner"] = o
            if o:
                drawn_assets.add(o[0])
            unique[crc] = info
    rows = []
    for crc, t in sorted(unique.items(), key=lambda kv: (kv[1]["rom"], kv[1]["w"], kv[1]["h"])):
        if t["owner"]:
            i, typ, rel = t["owner"]
            tag = "id%05X" % i if rel == 0 else "id%05X+%X" % (i, rel)
        elif t["rom"] >= 0:
            tag = "data" if t["rom"] < 0x8E0C0 else "rom"
        else:
            tag = "ram%06X" % t["ram"]
        fn = "tex_%07X_%s_%dx%d.png" % (max(t["rom"], 0), tag.replace("+", "_"), t["w"], t["h"])
        Image.fromarray(t["px"], "RGBA").save(os.path.join(out_dir, fn))
        rows.append((fn, t["rom"], tag, t["w"], t["h"], ", ".join(sorted(t["seen"]))))
    # Asset-table textures no capture drew: decode at a guessed square-ish size.
    und = os.path.join(out_dir, "not_drawn")
    os.makedirs(und, exist_ok=True)
    nd = 0
    for i, typ, s, n in entries:
        if typ != "texture" or i in drawn_assets:
            continue
        px = n // 2
        w = int(np.sqrt(px))
        if w * w != px:
            w = 32 if px % 32 == 0 else 16
        h = px // w
        Image.fromarray(decode16(rom[s:s + n], w, h), "RGBA").save(os.path.join(und, "tex_%07X_id%05X_%dx%d_guess.png" % (s, i, w, h)))
        nd += 1
    with open(os.path.join(out_dir, "LIST.md"), "w", encoding="utf-8") as f:
        f.write("# WildWaters textures (all RGBA16)\n\n")
        f.write("Drawn by the game in the capture runs (exact size). `id` = asset-directory entry, "
                "`data` = inside the program's data segment, `rom` = elsewhere in the ROM.\n\n")
        f.write("| file | ROM offset | where | size | seen in |\n|---|---|---|---|---|\n")
        for fn, off, tag, w, h, seen in rows:
            f.write("| %s | %s | %s | %dx%d | %s |\n" % (fn, "0x%07X" % off if off >= 0 else "-", tag, w, h, seen))
        f.write("\n`not_drawn/`: %d asset-table textures no capture drew; sizes are guesses.\n" % nd)
    print("unique textures %d (rom-matched %d, in asset table %d), not drawn %d" % (
        len(unique), sum(1 for t in unique.values() if t["rom"] >= 0), len(drawn_assets), nd))


if __name__ == "__main__":
    main()
