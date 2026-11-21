"""Exports the three kayaker models stored in WildWaters' data segment: KATANA, TOKOMO, TAMARA (the menu's
kayaker choices).

  python -I tools/ww_kayakers.py baserom.z64 OUTDIR

Material table ROM 0x7AA00-0x7AF20: 28-byte records (texture pointer, width, height, 0, 0, format 2 = RGBA16,
material id). Ids 0x0C0x / 0x0C8x / 0x0C4x are the three characters' materials. Right after it (ROM
0x7AF28) three model pointers. Model header:
  +0 part count, +4 parent table (u16 per part), +8 vertex count, +0xC vertices (u16 0, u16 part, f32 x, y, z),
  +0x10 corner count, +0x14 corners (u16 0, u16 vertex, u8 r, g, b, a, f32 s, f32 t, u16 material, u16 0),
  +0x18 ?, +0x1C command word count, +0x20 commands (u16): 1 start count = use corners start..start+count-1,
  2 part, 3 material, 4 a b c d e f = two triangles, 5 a b c = one triangle (indices into the corner window),
  then animation records (frames, ..., pointer).
"""
import os
import struct
import sys

from PIL import Image
import numpy as np

R = lambda a: a - 0x80008000 + 0x1000
MODELS = [0x8007ADB8, 0x8007E1D0, 0x800818D0]


def rgba16(b, w, h):
    d = np.frombuffer(b, ">u2").astype(np.uint32).reshape(h, w)
    return np.stack([((d >> 11) & 31) * 255 // 31, ((d >> 6) & 31) * 255 // 31, ((d >> 1) & 31) * 255 // 31,
                     (d & 1) * 255], -1).astype(np.uint8)


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    mats = {}
    o = 0x7A974                       # first record of the material table
    while True:
        ptr, w, h, _, _, fmt, mid = struct.unpack(">7I", rom[o:o + 28])
        if not (0x80060000 <= ptr < 0x80090000 and 0 < w <= 64 and 0 < h <= 64 and 0xC00 <= mid < 0xD00):
            break
        name = "tex_%07X_%dx%d.png" % (R(ptr), w, h)
        Image.fromarray(rgba16(rom[R(ptr):R(ptr) + w * h * 2], w, h), "RGBA").save(os.path.join(out, "textures", name))
        mats[mid] = (name, w, h)
        o += 28
    rows = ["# WildWaters kayaker models (data segment)\n\nThree models share one format: 1 KATANA, 2 TOKOMO, 3 TAMARA (picked on the kayaker menu). "
            "Vertices are posed with frame 0 of each model's first animation (per-part rotation + translation "
            "down the part hierarchy). Material table: ROM 0x7A974, %d materials.\n\n"
            "| file | parts | vertices | corners | triangles | materials |\n|---|---|---|---|---|---|\n" % len(mats)]
    for k, mp in enumerate(MODELS):
        h = struct.unpack(">10I", rom[R(mp):R(mp) + 40])
        nparts, pparts, nv, pv, nc, pc, _, ncmd, pcmd = h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]
        # Pose: frame 0 of the first animation. Its channel table (pointer in the first animation record
        # at header +0x24) holds per part a rotation channel (type 0: keys of u32 frame + quaternion x y z w)
        # and a translation channel (type 1: u32 frame + x y z + unused). A part's world transform is its
        # parent's transform times translate(t) * rotate(q).
        parents = struct.unpack(">%dH" % nparts, rom[R(pparts):R(pparts) + 2 * nparts])
        anim = struct.unpack(">I", rom[R(mp) + 0x24 + 20:R(mp) + 0x24 + 24])[0]
        rot = {p: (0.0, 0.0, 0.0, 1.0) for p in range(nparts)}
        tra = {p: (0.0, 0.0, 0.0) for p in range(nparts)}
        for ch in range(2 * nparts):
            part, nkeys, typ, kp = struct.unpack(">4I", rom[R(anim) + ch * 16:R(anim) + ch * 16 + 16])
            if part >= nparts or nkeys == 0:
                continue
            f = struct.unpack(">4f", rom[R(kp) + 4:R(kp) + 20])
            if typ == 0:
                rot[part] = f
            else:
                tra[part] = f[:3]

        def mat(q, t):
            x, y, z, w = q
            m = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
                          [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
                          [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
                          [0, 0, 0, 1]])
            return m
        world = {}

        def wm(p):
            if p not in world:
                local = mat(rot[p], tra[p])
                par = parents[p]
                world[p] = local if par == 0xFFFF or par == p else wm(par) @ local
            return world[p]
        verts = []
        for i in range(nv):
            q = R(pv) + i * 16
            _, part, x, y, z = struct.unpack(">HHfff", rom[q:q + 16])
            if part < nparts:
                x, y, z, _ = wm(part) @ np.array([x, y, z, 1.0])
            verts.append((part, x, y, z))
        corners = []
        for i in range(nc):
            q = R(pc) + i * 20
            _, vi, r_, g_, b_, a_, s_, t_, mid, _ = struct.unpack(">HHBBBBffHH", rom[q:q + 20])
            corners.append((vi, r_, g_, b_, s_, t_, mid))
        cmd = struct.unpack(">%dH" % ncmd, rom[R(pcmd):R(pcmd) + 2 * ncmd])
        tris = []
        base = 0
        mat = None
        i = 0
        while i < len(cmd):
            c = cmd[i]
            if c == 1:
                base = cmd[i + 1]; i += 3
            elif c == 2:
                i += 2
            elif c == 3:
                mat = cmd[i + 1]; i += 2
            elif c == 4:
                a = cmd[i + 1:i + 7]
                tris.append((base + a[0], base + a[1], base + a[2], mat))
                tris.append((base + a[3], base + a[4], base + a[5], mat))
                i += 7
            elif c == 5:
                a = cmd[i + 1:i + 4]
                tris.append((base + a[0], base + a[1], base + a[2], mat))
                i += 4
            else:
                i += 1
        name = "kayaker_%d" % (k + 1)
        with open(os.path.join(out, name + ".mtl"), "w") as m:
            for mid in sorted({t[3] for t in tris if t[3] is not None}):
                if mid in mats:
                    m.write("newmtl m%04X\nKd 1 1 1\nmap_Kd textures/%s\n\n" % (mid, mats[mid][0]))
                else:
                    m.write("newmtl m%04X\nKd 0.8 0.8 0.8\n\n" % mid)
        with open(os.path.join(out, name + ".obj"), "w") as f:
            f.write("# WildWaters kayaker model %d (data segment, RAM 0x%08X)\nmtllib %s.mtl\n" % (k + 1, mp, name))
            n = 1
            cur = -1
            for a, b, c, mid in tris:
                if max(a, b, c) >= nc:
                    continue
                if mid != cur:
                    f.write("usemtl m%04X\n" % (mid or 0)); cur = mid
                for ci in (a, b, c):
                    vi, r_, g_, b_, s_, t_, _ = corners[ci]
                    _, x, y, z = verts[vi] if vi < nv else (0, 0, 0, 0)
                    f.write("v %.4f %.4f %.4f %.3f %.3f %.3f\nvt %.5f %.5f\n" % (x, y, z, r_ / 255, g_ / 255, b_ / 255, s_, 1 - t_))
                f.write("f %d/%d %d/%d %d/%d\n" % (n, n, n + 1, n + 1, n + 2, n + 2))
                n += 3
        used = sorted({t[3] for t in tris if t[3] is not None})
        rows.append("| %s.obj | %d | %d | %d | %d | %s |\n" % (name, nparts, nv, nc, len(tris), " ".join("%X" % u for u in used)))
        print(name, nparts, nv, nc, len(tris))
    open(os.path.join(out, "LIST.md"), "w", encoding="utf-8").writelines(rows)


if __name__ == "__main__":
    main()
