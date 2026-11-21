"""Draws WildWaters' menu screens from their layout assets (0x03F2 picture elements, 72 bytes each:
+4 element id, +8 kind, +0xC code, +0x14 picture id, +0x1C x (float), +0x20 y (float), +0x2C/+0x30 scale)
and lists the text layouts (0x03F1, 36 bytes: +4 text id, +0x14 font id, +0x1C x<<16|y, +0x20 flags).

  python -I tools/ww_screens.py ../assets

Elements scaled up (scale > 1) are backgrounds and are drawn first. The title screen shows all its
elements at once; on the others the elements appear at different moments, so each is drawn on its own
over the background.
"""
import os
import re
import struct
import sys

from PIL import Image

SCREENS = {0x11497: "boot logos", 0x11498: "title screen", 0x11499: "race banners", 0x11369: "background"}
TEXT = {0x111A2: "text layout A", 0x111A3: "text layout B", 0x111A4: "text layout C", 0x111A5: "text layout D"}


def main():
    root = sys.argv[1]
    raw = os.path.join(root, "raw")
    imgs = {}
    for f in os.listdir(os.path.join(root, "images")):
        m = re.match(r"id([0-9A-F]+)_", f)
        if m:
            imgs[int(m.group(1), 16)] = os.path.join(root, "images", f)
    out = os.path.join(root, "screens")
    os.makedirs(out, exist_ok=True)
    rows = ["# WildWaters menu screens (drawn from the game's layout assets)\n\n"]
    files = {int(re.match(r"\d+_id([0-9A-F]+)_", f).group(1), 16): f for f in os.listdir(raw) if re.match(r"\d+_id[0-9A-F]+_data\.bin$", f)}
    for sid, title in SCREENS.items():
        d = open(os.path.join(raw, files[sid]), "rb").read()
        els = []
        for i in range(len(d) // 0x48):
            b = d[i * 0x48:(i + 1) * 0x48]
            w = struct.unpack(">18I", b)
            fl = struct.unpack(">18f", b)
            els.append((w[1], w[5], fl[7], fl[8], fl[11], fl[12]))
        rows.append("## 0x%X %s\n\n| element | picture | x | y | scale |\n|---|---|---|---|---|\n" % (sid, title))
        for e in els:
            rows.append("| 0x%X | 0x%X | %.0f | %.0f | %.0f x %.0f |\n" % e)
        bg = Image.new("RGBA", (320, 240), (0, 0, 0, 255))
        for eid, pid, x, y, sx, sy in els:
            if (sx > 1 or sy > 1) and pid in imgs:
                im = Image.open(imgs[pid]).convert("RGBA")
                im = im.resize((int(im.width * sx), int(im.height * sy)), Image.NEAREST)
                bg.alpha_composite(im, (int(x), int(y)))
        fg = [e for e in els if not (e[4] > 1 or e[5] > 1) and e[1] in imgs]
        if title == "title screen" or len(fg) <= 1:
            frames = [fg]
        else:
            frames = [[e] for e in fg]
        for k, group in enumerate(frames):
            s = bg.copy()
            for eid, pid, x, y, sx, sy in group:
                s.alpha_composite(Image.open(imgs[pid]).convert("RGBA"), (int(x), int(y)))
            name = "screen_%05X_%s%s.png" % (sid, title.replace(" ", "_"), "" if len(frames) == 1 else "_%d" % (k + 1))
            s.resize((640, 480), Image.NEAREST).save(os.path.join(out, name))
            rows.append("\n![%s](%s)\n" % (name, name))
        rows.append("\n")
    for sid, title in TEXT.items():
        d = open(os.path.join(raw, files[sid]), "rb").read()
        rows.append("## 0x%X %s (text elements; the strings come from the program by text id)\n\n| text id | font | x | y | flags |\n|---|---|---|---|---|\n" % (sid, title))
        for i in range(len(d) // 36):
            w = struct.unpack(">9I", d[i * 36:(i + 1) * 36])
            rows.append("| 0x%X | 0x%X | %d | %d | %08x |\n" % (w[1], w[5], w[7] >> 16, w[7] & 0xFFFF, w[8]))
        rows.append("\n")
    open(os.path.join(out, "LIST.md"), "w", encoding="utf-8").writelines(rows)
    print("screens written to", out)


if __name__ == "__main__":
    main()
