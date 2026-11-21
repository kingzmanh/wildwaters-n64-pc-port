"""Carves raw pictures (no headers) out of a ROM range: finds where each image starts, its pixel
format (RGBA16 or RGBA32), width and height, by how well neighbouring rows match.

  python -I tools/stale_images.py baserom.z64 OUTDIR START END [START END ...]

For a true width, a pixel matches the one below it about as well as the one beside it; for a wrong
width the row below is unrelated. At each position the best (format, width) is chosen on the next
8 rows, then the image grows row by row until rows stop matching. Leftover bytes are skipped in
64-byte steps. Writes OUTDIR/img_<rom>_<fmt>_<w>x<h>.png and OUTDIR/LIST.md.
"""
import os
import sys

import numpy as np
from PIL import Image

# STALE_FMT=ia16: read 16-bit pixels as intensity+alpha (track maps) instead of RGBA16.
IA16 = os.environ.get("STALE_FMT") == "ia16"
# STALE_RELAX=1: no alpha checks (second pass over the gaps: transparent overlays, soft alpha)
RELAX = os.environ.get("STALE_RELAX") == "1"
WIDTHS = list(range(16, 661, 2))


def to_rgb(raw, bpp):
    if bpp == 2 and IA16:
        d = np.frombuffer(raw[:len(raw) // 2 * 2], np.uint8).reshape(-1, 2).astype(np.float32)
        return np.stack([d[:, 0], d[:, 0], d[:, 1] * 16], -1)
    if bpp == 2:
        d = np.frombuffer(raw[:len(raw) // 2 * 2], ">u2").astype(np.int32)
        return np.stack([(d >> 11) & 31, (d >> 6) & 31, (d >> 1) & 31], -1).astype(np.float32) * (255 / 31)
    d = np.frombuffer(raw[:len(raw) // 4 * 4], ">u4").astype(np.int64)
    return np.stack([(d >> 24) & 255, (d >> 16) & 255, (d >> 8) & 255], -1).astype(np.float32)


def to_rgba(raw, bpp, w, h):
    if bpp == 2 and IA16:
        d = np.frombuffer(raw, np.uint8)[:w * h * 2].reshape(h, w, 2)
        return np.stack([d[..., 0], d[..., 0], d[..., 0], np.full((h, w), 255, np.uint8)], -1)
    if bpp == 2:
        d = np.frombuffer(raw, ">u2").astype(np.uint32)[:w * h].reshape(h, w)
        return np.stack([((d >> 11) & 31) * 255 // 31, ((d >> 6) & 31) * 255 // 31, ((d >> 1) & 31) * 255 // 31,
                         (d & 1) * 255], -1).astype(np.uint8)
    d = np.frombuffer(raw, ">u4").astype(np.uint64)[:w * h].reshape(h, w)
    return np.stack([(d >> 24) & 255, (d >> 16) & 255, (d >> 8) & 255, d & 255], -1).astype(np.uint8)


def score(px, w, rows=8):
    n = w * rows
    if len(px) < n:
        return 1e9, 1e9
    x = px[:n].reshape(rows, w, 3)
    v = np.abs(x[1:] - x[:-1]).mean()
    hz = np.abs(x[:, 1:] - x[:, :-1]).mean()
    return v, hz


def carve(rom, start, end, out, rows_out):
    pos = start
    aligned = -1
    found = 0
    while pos < end - 256:
        best = None
        per_fmt = []
        for bpp in ((2,) if IA16 else (2, 4)):
            px = to_rgb(rom[pos:min(end, pos + 4096 * bpp)], bpp)
            if len(px) < 1024 or np.ptp(px[:256]) < 8:     # flat fill: not worth an image
                continue
            if bpp == 4:
                # RGBA32 needs a real alpha byte: mostly 0x00/0xFF (soft edges in between) or one constant value
                al = np.frombuffer(rom[pos:pos + 4096], np.uint8)[3::4]
                vals, cnt = np.unique(al, return_counts=True)
                if np.isin(al, (0, 255)).mean() < 0.5 and cnt.max() < len(al) * 0.9 and not RELAX:
                    continue
            else:
                # RGBA16 opaque images have the alpha bit set on most pixels
                a16 = np.frombuffer(rom[pos:pos + 4096], ">u2") & 1
                if a16.mean() < 0.5 and not IA16 and not RELAX:
                    continue
            # D(lag): how different a pixel is from the one `lag` later. At the true width the pixel
            # `lag` later is the one below: D dips sharply against the lags around it.
            D = {}
            def dl(lag):
                if lag not in D:
                    D[lag] = np.abs(px[lag:] - px[:-lag]).mean() if lag * 3 < len(px) else np.inf
                return D[lag]
            cand = []
            for w in WIDTHS:
                if not np.isfinite(dl(w + 12)):
                    break
                around = np.median([dl(w - 12) if w > 16 else dl(w + 16), dl(w - 8) if w > 12 else dl(w + 20), dl(w + 8), dl(w + 12)])
                cand.append((dl(w) / (around + 1e-6), w, dl(w)))
            if not cand:
                continue
            r0 = min(c[0] for c in cand)
            # prefer the smallest width whose dip is (nearly) as deep: 2W, 3W dip too
            for ratio, w, dw in sorted(cand, key=lambda c: c[1]):
                if ratio <= r0 + 0.1:
                    # blurry pictures dip at w-2 / w+2 too: take the exact minimum nearby
                    w = min((w2 for w2 in range(max(16, w - 6), w + 7, 2)), key=dl)
                    ratio = dl(w) / (np.median([dl(w + 8), dl(w + 12), dl(max(4, w - 8)), dl(max(4, w - 12))]) + 1e-6)
                    # between RGBA16 and RGBA32 (same row length in bytes), the true format is the one whose
                    # neighbouring pixels match best (the wrong one interleaves halves of pixels)
                    smooth = dl(1) / (np.ptp(px[:4096]) + 1.0)
                    per_fmt.append((ratio, bpp, w, dl(w), smooth))
                    break
        if per_fmt:
            per_fmt.sort(key=lambda c: c[4])
            # a format whose neighbouring pixels match far better is the real one (the other reads
            # halves of pixels); then a shallower width dip is fine (flat pictures like track maps)
            if len(per_fmt) == 2 and per_fmt[0][4] < 0.4 * per_fmt[1][4] and per_fmt[0][0] < 0.85:
                best = per_fmt[0]
                best = (min(best[0], 0.64),) + best[1:]
            else:
                best = min(per_fmt, key=lambda c: c[0])
        if best is None or best[0] > 0.65:
            pos += 64
            continue
        s, bpp, w, v0, _ = best
        rowb = w * bpp
        px = to_rgb(rom[pos:min(end, pos + w * 1024 * bpp)], bpp)
        maxh = len(px) // w
        # Per row: does this width still fit? Compare each row with the pixels exactly one row above
        # (w back) and with pixels w+-8 / w+-12 back. Inside the image the row above is the closest; once
        # the data belongs to the next image, nothing special happens at w and the ratio goes to ~1.
        def fit(i):
            seg = px[i * w:(i + 1) * w]
            base = i * w
            dw = np.abs(seg - px[base - w:base]).mean()
            refs = [np.abs(seg - px[base - s:base - s + w]).mean() for s in (w - 12, w - 8, w + 8, w + 12) if 0 <= base - s and s > 0]
            return dw / (np.median(refs) + 1e-3) if refs else 0.0
        h = 2
        bad = 0
        while h < maxh:
            if fit(h) > 0.8:
                bad += 1
                if bad >= 2:
                    h -= 1
                    break
            else:
                bad = 0
            h += 1
        h = max(h, 2)
        # Left edge: if the start is off by k pixels, every row shows the picture's left/right border
        # as a vertical seam at column k. Move the start there and look again.
        if h >= 6 and pos != aligned and bpp == 4:
            # mostly transparent pictures (outlines, track maps): no colour seam, so put the emptiest
            # column at the border instead
            alpha = np.frombuffer(rom[pos:pos + h * w * 4], np.uint8)[3::4].reshape(h, w).astype(np.int64)
            if (alpha < 128).mean() > 0.5:
                cs = alpha.sum(axis=0)
                empty = cs < 0.08 * cs.mean()
                # longest run of empty columns, going round the row: its middle is the real border
                best_run, k = 0, 0
                for st in range(w):
                    if empty[st] and not empty[st - 1]:
                        n = 0
                        while n < w and empty[(st + n) % w]:
                            n += 1
                        if n > best_run:
                            best_run, k = n, (st + n // 2) % w
                if best_run >= 2 and 0 < k < w and not (empty[0] and empty[w - 1]):
                    pos += k * bpp
                    aligned = pos
                    continue
        if h >= 6 and pos != aligned:
            x = px[:h * w].reshape(h, w, 3)
            col = np.abs(x[:, 1:] - x[:, :-1]).mean(axis=(0, 2))
            k = int(np.argmax(col)) + 1
            # a real seam runs through (nearly) every row, unlike an edge inside the picture
            rows_hit = (np.abs(x[:, k] - x[:, k - 1]).mean(axis=1) > np.median(col) * 3).mean()
            if col[k - 1] > np.median(col) * 4 and rows_hit > 0.7 and 0 < k < w:
                pos += k * bpp
                aligned = pos
                continue
        size = h * rowb
        img = to_rgba(rom[pos:pos + size], bpp, w, h)
        if bpp == 4 and img[..., 3].max() == 0:
            img[..., 3] = 255
        fmt = ("ia16" if IA16 else "rgba16") if bpp == 2 else "rgba32"
        name = "img_%07X_%s_%dx%d.png" % (pos, fmt, w, h)
        if h >= 8 and w * h >= 256:
            Image.fromarray(img, "RGBA").save(os.path.join(out, name))
            rows_out.append((pos, size, fmt, w, h, name))
            found += 1
        pos += size
    return found


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rows = []
    args = sys.argv[3:]
    for i in range(0, len(args), 2):
        a, b = int(args[i], 16), int(args[i + 1], 16)
        n = carve(rom, a, b, out, rows)
        print("0x%X-0x%X: %d images" % (a, b, n))
    with open(os.path.join(out, "LIST.md"), "w") as f:
        f.write("# Pictures carved from the leftover ROM area (no headers: sizes found from the pixels)\n\n"
                "| ROM offset | bytes | format | size | file |\n|---|---|---|---|---|\n")
        for pos, size, fmt, w, h, name in rows:
            f.write("| 0x%07X | %d | %s | %dx%d | %s |\n" % (pos, size, fmt, w, h, name))
    print("total", len(rows))


if __name__ == "__main__":
    main()
