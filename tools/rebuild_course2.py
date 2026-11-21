"""Rebuilds Woodchuck Ridge's data lost past the 8 MB flash cut-off and writes a patch
file the host applies to its in-memory copy of the ROM (the ROM file is never changed).

  python tools/rebuild_course2.py baserom.z64 host/assets/restore/course2.patch [--mesh capture.obj]

Lost data (func_80010820 gives the addresses):
  - water block 0x7B7F30: header says 30 tiles; tile 28 is cut at 0x800000, tile 29 is gone
  - track-segment pack 0x8047A0: count, 31 offsets, 30 collision segments (all gone)

Water tile (0x28D0 bytes): s32 link[4] (-x,+x,-z,+z neighbour, -1 none), s32 50, s32 50,
  f32 xmin, xmax, zmin, zmax, f32 level, then 51x51 x 4 s8 (mask/flow channels).
Segment: f32 bbox[6] (xmin ymin zmin xmax ymax zmax), u16 nodes, u16 tris, u16 verts, u16 0xCCCC,
  nodes[] 14 bytes (u8 box x0 x1 y0 y1 z0 z1 quantised 0-255 to the bbox, u16 left, u16 right,
  s16 tri0, s16 tri1; leaf = both children 0), tris[] 3 x u16, pad to 4, verts[] 3 x f32.

Patch file: repeated records of u32 rom_offset, u32 length, bytes (big-endian).
"""
import math
import struct
import sys

WATER = 0x7B7F30
PACK = 0x8047A0
TILE = 0x28D0
CUT = 0x800000
NTILES = 30


def tile(rom, k):
    o = WATER + 4 + k * TILE
    return bytearray(rom[o:o + TILE])


def tile_header(t):
    return list(struct.unpack(">6i5f", t[:0x2C]))


def set_header(t, h):
    t[:0x2C] = struct.pack(">6i5f", *h)


def rebuild_water(rom):
    """Tile 28: keep the original bytes before 0x800000 and continue the grid from tile 27's
    pattern; tile 29: the next tile in the +x direction tile 28 links to (link[0] = 29)."""
    t27 = tile(rom, 27)
    t28 = tile(rom, 28)
    keep = CUT - (WATER + 4 + 28 * TILE)          # original bytes of tile 28 that survived
    grid28 = bytearray(t28[0x2C:])
    grid27 = t27[0x2C:]
    # continue tile 28's grid with tile 27's rows (same river width, the flow keeps going)
    grid28[keep - 0x2C:] = grid27[keep - 0x2C:]
    t28[0x2C:] = grid28
    h28 = tile_header(t28)
    # tile 29: tile 28 points to it with link[0] (-x side)
    t29 = bytearray(t28)
    h29 = list(h28)
    w = h28[7] - h28[6]
    h29[0:4] = [-1, 28, -1, -1]
    h29[6], h29[7] = h28[6] - w, h28[7] - w
    h29[10] = 2 * h28[10] - tile_header(t27)[10]   # keep descending as from 27 to 28
    set_header(t29, h29)
    return t28, t29


def terrain_height(T, x, z):
    """Lowest terrain surface (numpy array of triangles) at point (x, z), or None: the riverbed
    under the channel, the bank elsewhere (flat planes above the river are ignored)."""
    import numpy as np
    a, b, c = T[:, 0], T[:, 1], T[:, 2]
    v0, v1 = c - a, b - a
    d00 = v0[:, 0] ** 2 + v0[:, 2] ** 2
    d01 = v0[:, 0] * v1[:, 0] + v0[:, 2] * v1[:, 2]
    d11 = v1[:, 0] ** 2 + v1[:, 2] ** 2
    px, pz = x - a[:, 0], z - a[:, 2]
    d02 = v0[:, 0] * px + v0[:, 2] * pz
    d12 = v1[:, 0] * px + v1[:, 2] * pz
    den = d00 * d11 - d01 * d01
    den[den == 0] = 1e-9
    u = (d11 * d02 - d01 * d12) / den
    v = (d00 * d12 - d01 * d02) / den
    m = (u >= -1e-4) & (v >= -1e-4) & (u + v <= 1 + 1e-4)
    if not m.any():
        return None
    return float((a[m, 1] + u[m] * v0[m, 1] + v[m] * v1[m, 1]).min())


def rebuild_water_from_mesh(rom, t28, t29, pieces28, pieces29):
    """Real tiles (from Devil's Elbow): grid index [x][z]; channel 0 is non-zero where the
    terrain is under the water level (plus a bank margin), channels 1-3 a flow vector; the
    level sits ~2 units above the tile's lowest terrain. Rebuild the lost cells of tile 28
    and all of tile 29: wet where terrain < level + 1, values from the nearest real wet cell
    (tile 28's surviving columns next to tile 29, and tile 27)."""
    import numpy as np
    h27, h28, h29 = tile_header(tile(rom, 27)), tile_header(t28), tile_header(t29)
    if pieces29:
        h29[10] = min(v[1] for t in pieces29 for v in t) + 2.0
        set_header(t29, h29)

    def band(pieces, lvl):
        # terrain near the water only: scenery planes above the river would hide it
        return np.array([t for t in pieces if -8.0 <= sum(v[1] for v in t) / 3 - lvl <= 40.0])
    T28, T29 = band(pieces28, h28[10]), band(pieces29, h29[10])
    keep_cells = (CUT - (WATER + 4 + 28 * TILE) - 0x2C) // 4
    # The game's water query (func_80013C2C): height = level + 0.2 * u8 ch0 (bilinear),
    # flow = 0.2 * s8 ch1..3; grid index [x][z]. Known wet cells in world space, with their
    # surface height: tile 28's surviving cells and tile 27.
    known = []

    def add_known(hh, raw, limit):
        grid = np.frombuffer(bytes(raw), np.uint8).reshape(51, 51, 4)
        xs, zs = np.linspace(hh[6], hh[7], 51), np.linspace(hh[8], hh[9], 51)
        for n in range(limit):
            i, j = divmod(n, 51)
            if grid[i, j, 0] != 0:
                vec = grid[i, j, 1:].astype(np.int8).astype(float)
                known.append((xs[i], zs[j], hh[10] + 0.2 * float(grid[i, j, 0]), vec))

    add_known(h28, tile(rom, 28)[0x2C:], keep_cells)
    add_known(h27, tile(rom, 27)[0x2C:], 51 * 51)

    def fill(t, hh, T, first_cell):
        kx = np.array([k[0] for k in known]); kz = np.array([k[1] for k in known])
        ky = np.array([k[2] for k in known]); kv = np.array([k[3] for k in known])
        g = np.frombuffer(bytes(t[0x2C:]), np.uint8).reshape(51, 51, 4).copy()
        xs, zs = np.linspace(hh[6], hh[7], 51), np.linspace(hh[8], hh[9], 51)
        new = []
        for n in range(first_cell, 51 * 51):
            i, j = divmod(n, 51)
            d2 = (kx - xs[i]) ** 2 + (kz - zs[j]) ** 2
            near = np.argsort(d2)[:8]
            w = 1.0 / (d2[near] + 1.0)
            surf = float((ky[near] * w).sum() / w.sum())
            vec = (kv[near] * w[:, None]).sum(axis=0) / w.sum()
            bed = terrain_height(T, xs[i], zs[j]) if len(T) else None
            surf = max(surf, hh[10])
            if bed is not None and bed < surf - 0.3:
                g[i, j, 0] = int(max(1, min(255, round((surf - hh[10]) / 0.2))))
                g[i, j, 1:] = np.clip(np.round(vec), -127, 127).astype(np.int8).view(np.uint8)
                new.append((xs[i], zs[j], hh[10] + 0.2 * g[i, j, 0], vec))
            else:
                g[i, j] = 0
        t[0x2C:] = g.tobytes()
        return new

    known += fill(t28, h28, T28, keep_cells)

    # Tile 29 is the plunge pool at the end, ~24 units below tile 28: a flat surface 2 units
    # over its level, wet where the bed is below it, with tile 28's exit flow (cells near its
    # -x edge) carrying on at half strength.
    g = np.zeros((51, 51, 4), np.uint8)
    xs, zs = np.linspace(h29[6], h29[7], 51), np.linspace(h29[8], h29[9], 51)
    exit_vecs = [k[3] for k in known if h28[6] <= k[0] < h28[6] + 8 and h28[8] <= k[1] < h28[9]]
    vec = np.mean(exit_vecs, axis=0) * 0.5 if exit_vecs else np.zeros(3)
    surf = h29[10] + 2.0
    for i in range(51):
        for j in range(51):
            bed = terrain_height(T29, xs[i], zs[j]) if len(T29) else None
            if bed is not None and bed < surf - 0.3:
                g[i, j, 0] = 10
                g[i, j, 1:] = np.clip(np.round(vec), -127, 127).astype(np.int8).view(np.uint8)
    t29[0x2C:] = g.tobytes()
    return t28, t29


def empty_segment(bbox):
    data = struct.pack(">6f", *bbox) + struct.pack(">HHHH", 1, 0, 0, 0xCCCC)
    data += bytes([0, 255, 0, 255, 0, 255]) + struct.pack(">HHhh", 0, 0, -1, -1)
    while len(data) % 4:
        data += b"\0"
    return data


def mesh_segment(bbox, tris_world):
    """Collision segment from world-space triangles (list of 3 x (x,y,z)), with a BVH."""
    verts = []
    index = {}
    tris = []
    for tri in tris_world:
        ids = []
        for v in tri:
            key = tuple(round(c, 3) for c in v)
            if key not in index:
                index[key] = len(verts)
                verts.append(key)
            ids.append(index[key])
        tris.append(ids)
    if not tris:
        return empty_segment(bbox)
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    bb = [lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]]

    def q(v, a, up):
        span = bb[a + 3] - bb[a]
        if span <= 0:
            return 0 if not up else 255
        f = (v - bb[a]) / span * 255.0
        return max(0, min(255, int(math.ceil(f)) if up else int(math.floor(f))))

    def box_of(tids):
        pts = [verts[i] for t in tids for i in tris[t]]
        b = []
        for a in range(3):
            b += [q(min(p[a] for p in pts), a, False), q(max(p[a] for p in pts), a, True)]
        return b

    nodes = []

    def build(tids):
        me = len(nodes)
        nodes.append(None)
        if len(tids) <= 2:
            nodes[me] = (box_of(tids), 0, 0, tids[0], tids[1] if len(tids) > 1 else -1)
            return me
        cent = {t: [sum(verts[i][a] for i in tris[t]) / 3 for a in range(3)] for t in tids}
        ext = [max(cent[t][a] for t in tids) - min(cent[t][a] for t in tids) for a in range(3)]
        ax = ext.index(max(ext))
        s = sorted(tids, key=lambda t: cent[t][ax])
        h = len(s) // 2
        left = build(s[:h])
        right = build(s[h:])
        nodes[me] = (box_of(tids), left, right, -1, -1)
        return me

    root_children = build(list(range(len(tris))))
    # The game starts the walk at node 0 and only visits its children: put a root above.
    nodes = [([0, 255, 0, 255, 0, 255], 1, 0, -1, -1)] + [
        (b, l + 1 if l or r else 0, r + 1 if l or r else 0, t0, t1) for (b, l, r, t0, t1) in nodes]
    assert root_children == 0
    data = struct.pack(">6f", *bb) + struct.pack(">HHHH", len(nodes), len(tris), len(verts), 0xCCCC)
    for b, l, r, t0, t1 in nodes:
        data += bytes(b) + struct.pack(">HHhh", l, r, t0, t1)
    for t in tris:
        data += struct.pack(">HHH", *t)
    while len(data) % 4:
        data += b"\0"
    for v in verts:
        data += struct.pack(">3f", *v)
    return data


def load_capture(path):
    """Terrain triangles from a display-list capture (host WW_CAPTURE_OBJ)."""
    sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
    import meshlib
    return meshlib.load_terrain(path), meshlib


def load_obj(path):
    vs, tris = [], []
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            vs.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == "f":
            tris.append(tuple(vs[int(x.split("/")[0]) - 1] for x in p[1:4]))
    return tris


def clip_to_tile(tris, h):
    xmin, xmax, zmin, zmax = h[6], h[7], h[8], h[9]
    out = []
    for t in tris:
        cx = sum(v[0] for v in t) / 3
        cz = sum(v[2] for v in t) / 3
        if xmin <= cx < xmax and zmin <= cz < zmax:
            out.append(t)
    return out


def main():
    rom = open(sys.argv[1], "rb").read()
    out_path = sys.argv[2]
    mesh = None
    meshlib = None
    if "--mesh" in sys.argv:
        mesh, meshlib = load_capture(sys.argv[sys.argv.index("--mesh") + 1])
    t28, t29 = rebuild_water(rom)
    headers = [tile_header(tile(rom, k)) for k in range(28)] + [tile_header(t28), tile_header(t29)]
    raw = {}
    if mesh:
        raw = {k: meshlib.clip_to_tile(mesh, headers[k][6], headers[k][7], headers[k][8], headers[k][9])
               for k in range(NTILES)}
        t28, t29 = rebuild_water_from_mesh(rom, t28, t29, raw[28], raw[29])
        headers[28], headers[29] = tile_header(t28), tile_header(t29)
    segs = []
    for k in range(NTILES):
        h = headers[k]
        bbox = [h[6], -2000.0, h[8], h[7], 2000.0, h[9]]
        if mesh:
            # Collision = terrain clipped to the tile, near the water (Devil's Elbow's real
            # segments span -2..+32 units around each tile's water level).
            lvl = h[10]
            pieces = [t for t in raw[k] if -8.0 <= sum(v[1] for v in t) / 3 - lvl <= 40.0]
            segs.append(mesh_segment(bbox, pieces))
        else:
            segs.append(empty_segment(bbox))
    table = struct.pack(">I", NTILES)
    off = 4 * (NTILES + 2)
    for s in segs:
        table += struct.pack(">I", off)
        off += len(s)
    table += struct.pack(">I", off)
    pack = table + b"".join(segs)
    water_tail = t28 + t29                          # tiles 28 and 29, written at their real place
    recs = [(WATER + 4 + 28 * TILE, bytes(water_tail)), (PACK, pack)]
    with open(out_path, "wb") as f:
        for o, b in recs:
            f.write(struct.pack(">II", o, len(b)) + b)
    print("tiles 28/29 + pack of %d segments (%d bytes, %d triangles)" % (
        NTILES, len(pack), sum(struct.unpack(">H", s[0x1A:0x1C])[0] for s in segs)))
    for k in (27, 28, 29):
        print("  tile %d: %s" % (k, headers[k]))


main()
