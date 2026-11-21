"""Shared helpers: load a capture OBJ (terrain only), clip triangles to a tile."""
import struct


def load_terrain(path, tex_lo=0x1F0000, tex_hi=0x220000, snap=0.5, vtx_filter=None):
    vs = []
    tris = set()
    keep = False
    for line in open(path):
        c = line[0]
        if c == "v":
            p = line.split()
            vs.append((float(p[1]), float(p[2]), float(p[3])))
        elif c == "g":
            name = line.split()[1]
            t = int(name[4:10], 16)
            keep = tex_lo <= t < tex_hi and (vtx_filter is None or vtx_filter(int(name[15:21], 16)))
        elif c == "f" and keep:
            ids = [int(x) - 1 for x in line.split()[1:4]]
            key = tuple(tuple(round(vs[i][a] / snap) for a in range(3)) for i in ids)
            if len(set(key)) == 3:
                tris.add(tuple(sorted(key)) if False else key)
    # merge duplicates in any vertex order
    uniq = {}
    for t in tris:
        uniq.setdefault(frozenset(t), t)
    return [tuple(tuple(c * snap for c in v) for v in t) for t in uniq.values()]


def clip_poly(poly, axis, value, keep_greater):
    out = []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        ina = a[axis] >= value if keep_greater else a[axis] <= value
        inb = b[axis] >= value if keep_greater else b[axis] <= value
        if ina:
            out.append(a)
        if ina != inb:
            t = (value - a[axis]) / (b[axis] - a[axis])
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(3)))
    return out


def clip_to_tile(tris, xmin, xmax, zmin, zmax):
    out = []
    for t in tris:
        if max(v[0] for v in t) <= xmin or min(v[0] for v in t) >= xmax:
            continue
        if max(v[2] for v in t) <= zmin or min(v[2] for v in t) >= zmax:
            continue
        p = list(t)
        for axis, val, g in ((0, xmin, True), (0, xmax, False), (2, zmin, True), (2, zmax, False)):
            p = clip_poly(p, axis, val, g)
            if len(p) < 3:
                break
        if len(p) >= 3:
            for i in range(1, len(p) - 1):
                out.append((p[0], p[i], p[i + 1]))
    return out
