"""Exports the Destruction Derby 64 track paths left in the ROM (0xBDA2B0-0xBF0170, after DD64's
music table at 0xBDA1D0) as OBJ polylines and top-view PNGs.

  python -I tools/dd64_paths.py baserom.z64 OUTDIR

The data is records of float triples (3D points) with small integer fields in between; consecutive
points closer than 60 units with at most 64 bytes between them form one path. Paths of 20+ points
are written (track racing lines / waypoint loops); shorter ones are listed only.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

A, B = 0xBDA2B0, 0xBF0170


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    f = np.frombuffer(rom[A:B], ">f4")
    ok = np.isfinite(f) & (np.abs(f) < 5000) & (np.abs(f) > 1e-3)
    pts, i = [], 0
    while i < len(f) - 2:
        if ok[i] and ok[i + 1] and ok[i + 2]:
            pts.append((A + i * 4, f[i], f[i + 1], f[i + 2]))
            i += 3
        else:
            i += 1
    pts = np.array(pts)
    lines, cur = [], [pts[0]]
    for p in pts[1:]:
        if np.linalg.norm(p[1:] - cur[-1][1:]) > 60 or p[0] - cur[-1][0] > 64:
            lines.append(np.array(cur))
            cur = [p]
        else:
            cur.append(p)
    lines.append(np.array(cur))
    rows = ["# Destruction Derby 64 track paths\n\n3D point paths (x, y up, z) found after DD64's music table. "
            "The long loops are the tracks' racing lines; which track each one is was not stored with them.\n\n"
            "| file | ROM | points | x range | z range | height range |\n|---|---|---|---|---|---|\n"]
    sheet = []
    n = 0
    for l in lines:
        if len(l) < 20:
            continue
        n += 1
        name = "path_%02d_0x%07X" % (n, int(l[0, 0]))
        with open(os.path.join(out, name + ".obj"), "w") as o:
            o.write("# Destruction Derby 64 track path from ROM 0x%X (%d points)\n" % (int(l[0, 0]), len(l)))
            for _, x, y, z in l:
                o.write("v %.3f %.3f %.3f\n" % (x, y, z))
            o.write("l " + " ".join(str(k + 1) for k in range(len(l))) + "\n")
        x, z = l[:, 1], l[:, 3]
        # Edge pairs: points alternate left edge / right edge. Then the path is a road ribbon.
        ribbon = False
        for phase in (0, 1):            # the first pair may start one point in
            even = l[phase::2, 1:]
            odd = l[phase + 1::2, 1:]
            m = min(len(even), len(odd))
            across = np.linalg.norm(even[:m] - odd[:m], axis=1).mean() if m > 2 else 0
            along = np.linalg.norm(even[1:m] - even[:m - 1], axis=1).mean() if m > 2 else 1
            if m > 8 and across > 1.5 * along:
                ribbon = True
                break
        if ribbon and phase:
            l = l[1:]
            x, z = l[:, 1], l[:, 3]
        if ribbon:
            with open(os.path.join(out, name + "_road.obj"), "w") as o:
                o.write("# Destruction Derby 64 road surface rebuilt from the edge pairs at ROM 0x%X\n" % int(l[0, 0]))
                for k in range(m):
                    o.write("v %.3f %.3f %.3f\nv %.3f %.3f %.3f\n" % (*even[k], *odd[k]))
                for k in range(m - 1):
                    a0, a1, b0, b1 = 2 * k + 1, 2 * k + 2, 2 * k + 3, 2 * k + 4
                    o.write("f %d %d %d %d\n" % (a0, b0, b1, a1))
        size = 512
        img = Image.new("RGB", (size, size), (18, 22, 26))
        d = ImageDraw.Draw(img)
        span = max(np.ptp(x), np.ptp(z), 1)
        px = (x - x.min()) / span * (size - 40) + 20
        pz = (z - z.min()) / span * (size - 40) + 20
        hy = (l[:, 2] - l[:, 2].min()) / (np.ptp(l[:, 2]) + 1e-6)
        if ribbon:
            for k in range(m - 1):
                i0, i1, j0, j1 = 2 * k, 2 * k + 1, 2 * k + 2, 2 * k + 3
                c = (int(90 + 165 * hy[i0]), int(200 - 60 * hy[i0]), 255 - int(150 * hy[i0]))
                d.polygon([(px[i0], pz[i0]), (px[j0], pz[j0]), (px[j1], pz[j1]), (px[i1], pz[i1])], fill=c)
        else:
            for k in range(len(l) - 1):
                c = (int(90 + 165 * hy[k]), int(200 - 60 * hy[k]), 255 - int(150 * hy[k]))
                d.line((px[k], pz[k], px[k + 1], pz[k + 1]), fill=c, width=3)
        d.ellipse((px[0] - 6, pz[0] - 6, px[0] + 6, pz[0] + 6), outline=(255, 80, 80), width=2)
        d.text((8, 6), "%s  %d points" % (name, len(l)), fill=(170, 170, 170))
        img.save(os.path.join(out, name + ".png"))
        rows.append(("| %s.obj / .png" + (" / _road.obj" if ribbon else "") + " | 0x%07X | %d | %.0f..%.0f | %.0f..%.0f | %.0f..%.0f |\n")
                    % (name, int(l[0, 0]), len(l), x.min(), x.max(), z.min(), z.max(), l[:, 2].min(), l[:, 2].max()))
    rows.append("\nColour in the pictures = height (blue low, pink high); the red circle marks the first point.\n")
    rows.append("%d shorter point runs (under 20 points: spawn points, markers) are not exported.\n" % sum(1 for l in lines if len(l) < 20))
    open(os.path.join(out, "LIST.md"), "w", encoding="utf-8").writelines(rows)
    print(n, "paths")


if __name__ == "__main__":
    main()
