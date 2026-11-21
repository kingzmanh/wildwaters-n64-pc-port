"""Second pass over tools/stale_images.py output: fixes pictures that start a few columns late.

  python -I tools/stale_fix_wrap.py baserom.z64 IMAGEDIR

A picture whose start is off by k pixels shows its right part on the left ("lete | de"). Two signs:
  - the end of each row runs straight on into the start of the next row (they really are neighbours),
  - one column c has a seam in every row (the picture's real left/right edges meet there).
Such a picture is cut again from ROM at start + c pixels, same width and height. LIST.md is rewritten;
replaced PNGs are moved to IMAGEDIR/_before_wrap_fix/.
"""
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stale_images import to_rgb, to_rgba


def fix_offset(rom, pos, bpp, w, h):
    """Start offset (pixels) that makes the picture whole, or 0."""
    if h < 6 or w < 16:
        return 0
    px = to_rgb(rom[pos:pos + w * h * bpp], bpp).reshape(h, w, 3)
    hd = np.abs(px[:, 1:] - px[:, :-1]).mean(axis=2)
    # The row end really continues into the next row's start when the picture is shifted.
    wrap = float(np.abs(px[:-1, -1] - px[1:, 0]).mean())
    wrap_ok = wrap <= 2.0 * float(hd.mean()) + 2
    # Border energy: how busy the two columns that would become the picture's edges are
    # (vertical change along them). Pictures have calm edges; a wrong cut runs through text.
    vd = np.abs(px[1:] - px[:-1]).mean(axis=2)          # (h-1, w)
    colbusy = vd.mean(axis=0) + np.concatenate([[0], hd.mean(axis=0)]) * 0.5
    energy = colbusy + np.roll(colbusy, 1)              # new first column c, new last column c-1
    e0 = energy[0]
    c = int(np.argmin(energy[4:w - 4])) + 4
    med = float(np.median(colbusy))
    # with continuous row ends a milder difference is enough; text cutting the wrap point hides
    # that sign, so then the edges must be clearly busy and the new ones clearly calm
    if (wrap_ok and energy[c] < 0.5 * e0 and e0 > 1.5 * med) or (energy[c] < 0.35 * e0 and e0 > 2.5 * med):
        return c
    return 0


def main():
    rom = open(sys.argv[1], "rb").read()
    d = sys.argv[2]
    lst = os.path.join(d, "LIST.md")
    lines = open(lst, encoding="utf-8").read().splitlines()
    bak = os.path.join(d, "_before_wrap_fix")
    os.makedirs(bak, exist_ok=True)
    out = []
    fixed = 0
    for line in lines:
        m = re.match(r"\| 0x([0-9A-F]+) \| (\d+) \| (\w+) \| (\d+)x(\d+) \| (\S+) \|", line)
        if not m:
            out.append(line)
            continue
        pos, size, fmt, w, h, name = int(m.group(1), 16), int(m.group(2)), m.group(3), int(m.group(4)), int(m.group(5)), m.group(6)
        bpp = 4 if fmt == "rgba32" else 2
        c = fix_offset(rom, pos, bpp, w, h)
        if c:
            npos = pos + c * bpp
            img = to_rgba(rom[npos:npos + w * h * bpp], bpp, w, h)
            if bpp == 4 and img[..., 3].max() == 0:
                img[..., 3] = 255
            nname = "img_%07X_%s_%dx%d.png" % (npos, fmt, w, h)
            if os.path.exists(os.path.join(d, name)):
                shutil.move(os.path.join(d, name), os.path.join(bak, name))
            Image.fromarray(img, "RGBA").save(os.path.join(d, nname))
            out.append("| 0x%07X | %d | %s | %dx%d | %s |" % (npos, size, fmt, w, h, nname))
            fixed += 1
        else:
            out.append(line)
    open(lst, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("re-aligned", fixed)


if __name__ == "__main__":
    main()
