"""Exports WildWaters' river-surface meshes (assets 0x186A0 Devil's Elbow, 0x186A1 Woodchuck Ridge).

  python -I tools/ww_water_meshes.py baserom.z64 OUTDIR

func_80038DCC ("REINITIALIZING WATER") reads the pack as 16-byte records until a zero word:
  +0 vertex list pointer, +4 triangle list pointer (both: asset offset + position in the pack),
  +8 s16 vertex count, +10 s16 triangle count, +12 / +14 u16 parameters,
and func_800387FC / func_80037E4C build a display list from each: vertex = 18 bytes
  s16 x, y, z; u8 r, g, b, a; u8 foam (> 0x5F marks a vertex, 0 forces alpha 0xFF); u8 ?; s16 ?;
  s16 s (+0x8000), s16 t (texture coordinates, 1/32 texel)
triangle = 3 x u16 vertex index.
Writes OUTDIR/<course>/water_mesh_NN.obj, a combined water_all.obj per course, and LIST.md.
"""
import os
import struct
import sys

TABLE, COUNT, DATA_BASE = 0x89370, 191, 0x8E0C0     # asset directory (ROM), entries, data base (ROM)
PACKS = [(0x186A0, "Devils_Elbow"), (0x186A1, "Woodchuck_Ridge")]


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    entries = {}
    for i in range(COUNT):
        e = TABLE + i * 0x20
        aid, off = struct.unpack(">II", rom[e:e + 8])
        entries[aid] = off
    rows = ["# WildWaters river-surface meshes (from the water packs, exact source data)\n\n"
            "Coordinates are world units as the race draws them (raw s16 / 32, height flipped), matching the course "
            "models. Woodchuck Ridge's raw heights pass the 16-bit limit near its end and wrap; the export undoes that.\n\n"]
    for aid, course in PACKS:
        off = entries[aid]
        base = DATA_BASE + off
        d = os.path.join(out, course)
        os.makedirs(d, exist_ok=True)
        rows.append("## %s (asset 0x%X, ROM 0x%X)\n\n| mesh | vertices | triangles | params | x range | z range |\n|---|---|---|---|---|---|\n" % (course, aid, base))
        allv, allf, allc, allt = [], [], [], []
        k = 0
        p = base
        while True:
            va, ta, nv, nt, p1, p2 = struct.unpack(">IIhhHH", rom[p:p + 16])
            if va == 0:
                break
            vbase = base + (va - off)
            tbase = base + (ta - off)
            verts = []
            for i in range(nv):
                q = vbase + i * 18
                x, y, z = struct.unpack(">hhh", rom[q:q + 6])
                r_, g_, b_, a_ = rom[q + 6:q + 10]
                s_, t_ = struct.unpack(">hh", rom[q + 14:q + 18])
                if y < -1000:
                    y += 65536      # Woodchuck Ridge's river drops past the s16 limit (to ~50000): undo the wrap
                # world units as the race draws them (matches the display-list capture): /32, y down
                verts.append((x / 32, -y / 32, z / 32, r_, g_, b_, ((s_ + 0x8000) & 0xFFFF) - 0x8000, t_))
            tris = [struct.unpack(">HHH", rom[tbase + i * 6:tbase + i * 6 + 6]) for i in range(nt)]
            k += 1
            name = "water_mesh_%02d" % k
            with open(os.path.join(d, name + ".obj"), "w") as o:
                o.write("# WildWaters %s river surface mesh %d (asset 0x%X record %d)\n" % (course, k, aid, k))
                for x, y, z, r_, g_, b_, s_, t_ in verts:
                    o.write("v %.3f %.3f %.3f %.3f %.3f %.3f\nvt %.4f %.4f\n" % (x, y, z, r_ / 255, g_ / 255, b_ / 255, s_ / 1024, 1 - t_ / 1024))
                for a, b, c in tris:
                    if max(a, b, c) < nv:
                        o.write("f %d/%d %d/%d %d/%d\n" % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))
            vo = len(allv)
            allv += verts
            allf += [(a + vo, b + vo, c + vo) for a, b, c in tris if max(a, b, c) < nv]
            xs = [v[0] for v in verts]
            zs = [v[2] for v in verts]
            rows.append("| %s | %d | %d | 0x%04X 0x%04X | %.0f..%.0f | %.0f..%.0f |\n" % (name, nv, nt, p1, p2, min(xs), max(xs), min(zs), max(zs)))
            p += 16
        with open(os.path.join(d, "water_all.obj"), "w") as o:
            o.write("# WildWaters %s: every river surface mesh (asset 0x%X), vertex colours + UVs\n" % (course, aid))
            for x, y, z, r_, g_, b_, s_, t_ in allv:
                o.write("v %.3f %.3f %.3f %.3f %.3f %.3f\nvt %.4f %.4f\n" % (x, y, z, r_ / 255, g_ / 255, b_ / 255, s_ / 1024, 1 - t_ / 1024))
            for a, b, c in allf:
                o.write("f %d/%d %d/%d %d/%d\n" % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))
        rows.append("\n%d meshes, %d vertices, %d triangles in total.\n\n" % (k, len(allv), len(allf)))
        print(course, k, "meshes", len(allv), "verts", len(allf), "tris")
    open(os.path.join(out, "LIST.md"), "w", encoding="utf-8").writelines(rows)


if __name__ == "__main__":
    main()
