"""Builds assets/models/ from the capture folders (after tools/assets_post.py).

  python -I tools/assets_models.py ../assets

Per course (capture/<Course>/, first frame snapshot):
  models/<Course>/terrain.obj   the course landscape (world coordinates)
  models/<Course>/sky.obj       the sky box (triangles with edges over 100 units)
  models/<Course>/water.obj     the river surface pieces (animated vertex buffers), one frame
  models/<Course>/scene.obj     everything drawn in that frame, one OBJ object per matrix
Small static objects (kayak + kayaker, paddle, ...) go to models/Kayak/ in model coordinates.
Each model folder gets its own textures/ with the PNGs it uses.
"""
import math
import os
import re
import shutil
import sys


def read_obj(path):
    tris = []      # (group, material, [(x,y,z,r,g,b,u,v)]*3)
    mtl = {}
    base = os.path.dirname(path)
    group = mat = None
    vs, vts = [], []
    for line in open(path):
        if line.startswith("v "):
            f = list(map(float, line.split()[1:7]))
            vs.append(tuple(f[:3] + (f[3:6] if len(f) >= 6 else [1.0, 1.0, 1.0])))
        elif line.startswith("vt "):
            vts.append(tuple(map(float, line.split()[1:3])))
        elif line.startswith("f "):
            idx = [p.split("/") for p in line.split()[1:4]]
            tris.append((group, mat, [vs[int(a) - 1] + vts[int(b) - 1] for a, b in idx]))
        elif line.startswith("o "):
            group = line.split()[1]
        elif line.startswith("usemtl "):
            mat = line.split()[1]
        elif line.startswith("mtllib "):
            name = None
            for l2 in open(os.path.join(base, line.split(None, 1)[1].strip())):
                if l2.startswith("newmtl "):
                    name = l2.split()[1]; mtl[name] = None
                elif l2.startswith("map_Kd "):
                    mtl[name] = os.path.normpath(os.path.join(base, l2.split(None, 1)[1].strip()))
    return tris, mtl


def write_obj(path, tris, mtl, groups=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    texdir = os.path.join(os.path.dirname(path), "textures")
    os.makedirs(texdir, exist_ok=True)
    name = os.path.splitext(os.path.basename(path))[0]
    used = sorted({t[1] for t in tris if t[1]})
    with open(os.path.join(os.path.dirname(path), name + ".mtl"), "w") as m:
        for u in used:
            src = mtl.get(u)
            m.write("newmtl %s\n" % u)
            if src and os.path.exists(src):
                shutil.copy2(src, os.path.join(texdir, os.path.basename(src)))
                m.write("Kd 1 1 1\nmap_Kd textures/%s\n\n" % os.path.basename(src))
            else:
                m.write("Kd 0.7 0.7 0.7\n\n")
    with open(path, "w") as o:
        o.write("# WildWaters (N64 prototype, Jun 15 1999) - captured from the PC port's display lists\n")
        o.write("mtllib %s.mtl\n" % name)
        cur_g = cur_m = None
        n = 1
        for g, mat, vv in sorted(tris, key=lambda t: (t[0] or "", t[1] or "")) if groups else tris:
            if groups and g != cur_g:
                o.write("o %s\n" % g); cur_g = g; cur_m = None
            if mat != cur_m:
                o.write("usemtl %s\n" % mat); cur_m = mat
            for x, y, z, r, g, b, u, v in vv:
                o.write("v %.3f %.3f %.3f %.3f %.3f %.3f\nvt %.5f %.5f\n" % (x, y, z, r, g, b, u, v))
            o.write("f %d/%d %d/%d %d/%d\n" % (n, n, n + 1, n + 1, n + 2, n + 2))
            n += 3
    return len(tris)


def main():
    root = sys.argv[1]
    cap = os.path.join(root, "capture")
    out = os.path.join(root, "models")
    report = []
    for course in sorted(os.listdir(cap)):
        d = os.path.join(cap, course)
        frames = sorted(f for f in os.listdir(d) if re.match(r"frame_\d+\.obj$", f))
        if not frames:
            continue
        tris, mtl = read_obj(os.path.join(d, frames[0]))
        groups = {}
        for t in tris:
            groups.setdefault(t[0], []).append(t)
        world = max((g for g in groups if g.endswith("_static")), key=lambda g: len(groups[g]))
        def edge(t):
            p = [v[:3] for v in t[2]]
            return max(math.dist(p[0], p[1]), math.dist(p[1], p[2]), math.dist(p[0], p[2]))
        terrain = [t for t in groups[world] if edge(t) <= 100]
        sky = [t for t in groups[world] if edge(t) > 100]
        water = [t for g in groups if g.endswith("_animated") for t in groups[g]]
        cdir = os.path.join(out, course)
        report.append((course, "terrain.obj", write_obj(os.path.join(cdir, "terrain.obj"), terrain, mtl)))
        report.append((course, "sky.obj", write_obj(os.path.join(cdir, "sky.obj"), sky, mtl)))
        report.append((course, "water.obj", write_obj(os.path.join(cdir, "water.obj"), water, mtl, groups=True)))
        report.append((course, "scene.obj", write_obj(os.path.join(cdir, "scene.obj"), tris, mtl, groups=True)))
        # small static objects in model coordinates
        od = os.path.join(d, "objects")
        for f in sorted(os.listdir(od)):
            m = re.match(r"obj_(\d+)_tris_static_([0-9A-F]+)\.obj$", f)
            if not m or int(m.group(1)) >= 5000:
                continue
            n = int(m.group(1))
            kind = "kayak_and_kayaker" if n > 300 else {28: "paddle", 15: "splash_effect"}.get(n, "object")
            ot, om = read_obj(os.path.join(od, f))
            name = "%s_%s_%dtris.obj" % (kind, course, n)
            report.append(("Kayak", name, write_obj(os.path.join(out, "Kayak", name), ot, om)))
    with open(os.path.join(out, "LIST.md"), "w") as f:
        f.write("# WildWaters 3D models (OBJ + MTL + PNG textures)\n\n"
                "Captured from what the game draws (PC port, headless capture runs). Open the .obj files in "
                "Blender, Windows 3D Viewer or any OBJ viewer. Course models are in world coordinates; the "
                "Kayak folder holds the moving models in their own coordinates.\n\n| folder | file | triangles |\n|---|---|---|\n")
        for a, b, n in report:
            f.write("| %s | %s | %d |\n" % (a, b, n))
    for r in report:
        print(*r)


if __name__ == "__main__":
    main()
