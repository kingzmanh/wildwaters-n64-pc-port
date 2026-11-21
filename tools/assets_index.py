"""Writes assets/index.html: one page to browse every extracted WildWaters asset.

  python -I tools/assets_index.py ../assets
"""
import html
import os
import re
import sys

MUSIC_NAMES = {
    "original_race_music_port.wav": "Original race music (what the prototype plays)",
    "thisbuild_song1.wav": "Unused song 1 (this build)",
    "thisbuild_song2.wav": "Unused song 2 (this build)",
    "thisbuild_song3.wav": "Unused song 3 (this build)",
    "thisbuild_song4.wav": "Unused song 4 (this build)",
    "thisbuild_short16ch.wav": "Short 16-channel song (this build)",
}
IMAGE_NAMES = {
    "112A6": "Ubi Soft logo", "112A8": "Looking Glass logo", "11332": "WildWaters logo",
    "11334": "Welcome to Devil's Elbow", "11336": "GO!", "11338": "Press Start to begin",
    "1133A": "WildWaters logo (small)", "1133C": "You survived!", "1133E": "16x16 image",
}


def esc(s):
    return html.escape(s, quote=True)


def files(d, ext):
    return sorted(f for f in os.listdir(d) if f.lower().endswith(ext)) if os.path.isdir(d) else []


def main():
    root = sys.argv[1]
    parts = []
    counts = {}

    # 2D screens, logos and fonts
    imgs = files(os.path.join(root, "images"), ".png")
    cards = []
    for f in imgs:
        m = re.match(r"id([0-9A-F]+)_(\d+)x(\d+)", f)
        label = IMAGE_NAMES.get(m.group(1), f) if m else f
        cards.append('<figure class="card big"><a href="images/%s" target="_blank"><img src="images/%s" alt="%s"></a>'
                     '<figcaption><b>%s</b><span>asset 0x%s · %sx%s</span></figcaption></figure>'
                     % (esc(f), esc(f), esc(label), esc(label), m.group(1), m.group(2), m.group(3)))
    parts.append(("screens", "Screens and logos", "Full-screen images and title graphics (RGBA16/RGBA32).", "".join(cards)))
    counts["screens"] = len(imgs)
    fonts = files(os.path.join(root, "fonts"), ".png")
    cards = []
    for f in fonts:
        m = re.match(r"id([0-9A-F]+)_(\d+)x(\d+)", f)
        cards.append('<figure class="card"><a href="fonts/%s" target="_blank"><img class="px" src="fonts/%s" alt=""></a>'
                     '<figcaption><span>asset 0x%s · %sx%s</span></figcaption></figure>' % (esc(f), esc(f), m.group(1), m.group(2), m.group(3)))
    parts.append(("fonts", "Fonts and HUD strips", "Digit and letter strips used by the HUD and menus.", "".join(cards)))
    counts["fonts"] = len(fonts)

    # textures
    tex = files(os.path.join(root, "textures"), ".png")
    nd = files(os.path.join(root, "textures", "not_drawn"), ".png")
    cards = []
    for f in tex + ["not_drawn/" + x for x in nd]:
        m = re.match(r"(?:not_drawn/)?tex_([0-9A-F]+)_(.+?)_(\d+)x(\d+)", f)
        where = m.group(2).replace("_", "+") if m else ""
        cards.append('<figure class="card tex" data-q="%s"><a href="textures/%s" target="_blank"><img class="px" src="textures/%s" alt="" loading="lazy"></a>'
                     '<figcaption><span>ROM 0x%s</span><span>%s · %sx%s</span></figcaption></figure>'
                     % (esc(f.lower()), esc(f), esc(f), m.group(1).lstrip("0") or "0", esc(where), m.group(3), m.group(4)))
    parts.append(("textures", "Textures",
                  "Every texture the game drew in the capture runs, at its exact size, matched to its place in the ROM "
                  "(<code>id</code> = asset-directory entry, <code>data</code> = inside the program's data). "
                  "<input id=\"q\" type=\"search\" placeholder=\"Filter (e.g. 2048, id13, 32x32)\">", "".join(cards)))
    counts["textures"] = len(tex) + len(nd)

    # models
    md = os.path.join(root, "models")
    prev = os.path.join(md, "previews")
    tri = {}
    if os.path.exists(os.path.join(md, "LIST.md")):
        for line in open(os.path.join(md, "LIST.md"), encoding="utf-8"):
            m = re.match(r"\| (\S+) \| (\S+\.obj) \| (\d+) \|", line)
            if m:
                tri[m.group(1) + "/" + m.group(2)] = int(m.group(3))
    desc = {"terrain": "Course landscape", "sky": "Sky box", "water": "River surface (one frame)",
            "scene": "Everything drawn in one frame", "kayak_and_kayaker": "Kayak and kayaker",
            "paddle": "Paddle", "splash_effect": "Splash effect"}
    cards = []
    for key, n in tri.items():
        folder, obj = key.split("/")
        stem = obj[:-4]
        kind = next((k for k in desc if stem.startswith(k)), stem)
        title = desc.get(kind, stem)
        if folder == "Kayak":   # e.g. kayak_and_kayaker_Devils_Elbow_334tris
            sub = "captured on " + stem[len(kind) + 1:].rsplit("_", 1)[0].replace("_", " ")
        else:
            sub = folder.replace("_", " ")
        pv = [p for p in ("%s_%s_view.png" % (folder, stem), "%s_%s_top.png" % (folder, stem),
                          "%s_%s_view1.png" % (folder, stem), "%s_%s_view2.png" % (folder, stem)) if os.path.exists(os.path.join(prev, p))]
        imgs_html = "".join('<a href="models/previews/%s" target="_blank"><img src="models/previews/%s" alt="" loading="lazy"></a>' % (esc(p), esc(p)) for p in pv)
        cards.append('<figure class="card model"><div class="shots">%s</div><figcaption><b>%s</b><span>%s · %d triangles</span>'
                     '<span><a href="models/%s/%s">%s</a> (+ .mtl, textures/)</span></figcaption></figure>'
                     % (imgs_html, esc(title), esc(sub), n, esc(folder), esc(obj), esc(folder + "/" + obj)))
    # models taken straight from the game data (not from captures)
    extra = []
    kl = os.path.join(md, "kayakers", "LIST.md")
    if os.path.exists(kl):
        names = {1: "KATANA", 2: "TOKOMO", 3: "TAMARA"}
        for line in open(kl, encoding="utf-8"):
            m = re.match(r"\| kayaker_(\d)\.obj \| \d+ \| \d+ \| \d+ \| (\d+) \|", line)
            if m:
                k = int(m.group(1))
                pv = ["kayakers/previews/kayaker_%d_%s.png" % (k, v) for v in ("front", "back", "view", "top")]
                extra.append(("kayakers/kayaker_%d.obj" % k, names[k], "data segment model, posed", int(m.group(2)), pv))
    for course in ("Devils_Elbow", "Woodchuck_Ridge"):
        f = os.path.join(md, "water_source", course, "water_all.obj")
        if os.path.exists(f):
            nt = sum(1 for l in open(f) if l.startswith("f "))
            pv = ["water_source/previews/%s_%s.png" % (course, v) for v in ("view", "top")]
            extra.append(("water_source/%s/water_all.obj" % course, "River surface (source meshes)",
                          course.replace("_", " ") + " · every water mesh", nt, pv))
    for obj, title, sub, n, pv in extra:
        pv = [p for p in pv if os.path.exists(os.path.join(md, p))]
        imgs_html = "".join('<a href="models/%s" target="_blank"><img src="models/%s" alt="" loading="lazy"></a>' % (esc(p), esc(p)) for p in pv)
        cards.append('<figure class="card model"><div class="shots">%s</div><figcaption><b>%s</b><span>%s · %d triangles</span>'
                     '<span><a href="models/%s">%s</a></span></figcaption></figure>'
                     % (imgs_html, esc(title), esc(sub), n, esc(obj), esc(obj)))
    tri.update({o: n for o, _, _, n, _ in extra})
    sc = os.path.join(root, "screens")
    if os.path.isdir(sc):
        shots = sorted(f for f in os.listdir(sc) if f.endswith(".png"))
        parts.append(("menus", "Menu screens",
                      "Drawn from the game's own screen layout assets (<a href=\"screens/LIST.md\">LIST.md</a>). On the "
                      "race-banner screen each banner appears at a different moment, so each is shown on its own.",
                      "".join('<figure class="card big"><a href="screens/%s" target="_blank"><img src="screens/%s" alt="" loading="lazy"></a>'
                              '<figcaption><span>%s</span></figcaption></figure>' % (esc(f), esc(f), esc(f[:-4].replace("_", " "))) for f in shots)))
        counts["menus"] = len(shots)
    parts.append(("models", "3D models",
                  "Captured from the game's display lists as OBJ + MTL + PNG textures (open in Blender, Windows 3D Viewer or any "
                  "OBJ viewer). Course models are in world coordinates; kayak models in their own. Pictures are previews.", "".join(cards)))
    counts["models"] = len(tri)

    # music
    mus = files(os.path.join(root, "music"), ".wav")
    rows = []
    def mus_label(f):
        if f in MUSIC_NAMES:
            return MUSIC_NAMES[f]
        m = re.match(r"dd64_song_(\d+)", f)
        return "Destruction Derby 64 song %d" % int(m.group(1)) if m else f
    for f in sorted(mus, key=lambda f: (f != "original_race_music_port.wav", not f.startswith("thisbuild"), f)):
        rows.append('<div class="track"><span>%s</span><audio controls preload="none" src="music/%s"></audio><a href="music/%s">%s</a></div>'
                    % (esc(mus_label(f)), esc(f), esc(f), esc(f)))
    parts.append(("music", "Music",
                  "The prototype's race music, the songs left unplayed in this build (probably Destruction Derby 64 music too) and the 13 Destruction Derby 64 songs left on the cartridge, rendered by "
                  "the PC port with the instrument bank they were written for (22050 Hz, ~82 s each, river sounds included).", "".join(rows)))
    counts["music"] = len(mus)

    # sound banks
    sd = os.path.join(root, "sounds")
    banks = sorted(d for d in os.listdir(sd) if os.path.isdir(os.path.join(sd, d))) if os.path.isdir(sd) else []
    info = {}
    if os.path.exists(os.path.join(sd, "LIST.md")):
        cur = None
        for line in open(os.path.join(sd, "LIST.md"), encoding="utf-8"):
            m = re.match(r"## (\S+) \((.*)\) - (.*)", line)
            if m:
                cur = m.group(1); info[cur] = (m.group(2), re.sub(r", \d+ waves$", "", m.group(3).strip())); continue
            m = re.match(r"\| (\d+) \| (wave_\d+\.wav) \| ([\d.]+ s) \| (.*?) \|", line)
            if m and cur:
                info.setdefault(cur + "/w", {})[m.group(2)] = (m.group(3), m.group(4))
    blocks = []
    total = 0
    order = ["bank38_used", "dd64_ddmusic_129", "dd64_ddsfx_80", "dd64_ddsfx_copy_cut_off"]
    for b in sorted(banks, key=lambda b: order.index(b) if b in order else 9):
        ws = files(os.path.join(sd, b), ".wav")
        total += len(ws)
        where, what = info.get(b, ("", ""))
        meta = info.get(b + "/w", {})
        items = "".join('<div class="wave"><span>%s</span><small>%s%s</small><audio controls preload="none" src="sounds/%s/%s"></audio></div>'
                        % (esc(w[:-4].replace("wave_", "#")), esc(meta.get(w, ("", ""))[0]),
                           (" · loop " + esc(meta[w][1])) if meta.get(w, ("", ""))[1] else "", esc(b), esc(w)) for w in ws)
        blocks.append('<details%s><summary><b>%s</b> <span>%s · %d waves · %s</span></summary><div class="waves">%s</div></details>'
                      % (" open" if b == "bank38_used" else "", esc(b.replace("_", " ")), esc(what), len(ws), esc(where), items))
    parts.append(("sounds", "Sound banks",
                  "Every instrument and sound-effect sample, decoded from N64 VADPCM to WAV (22050 Hz mono).", "".join(blocks)))
    counts["sounds"] = total

    raw = files(os.path.join(root, "raw"), ".bin")
    parts.append(("raw", "Raw data",
                  "All %d entries of the game's asset directory, byte for byte, with their ids, types and ROM offsets: "
                  "<a href=\"raw/LIST.md\">raw/LIST.md</a>. Lists for every section: <a href=\"textures/LIST.md\">textures</a>, "
                  "<a href=\"models/LIST.md\">models</a>, <a href=\"sounds/LIST.md\">sounds</a>. The capture runs (every "
                  "frame snapshot and object the game drew) are in <a href=\"capture/\">capture/</a>." % len(raw), ""))

    nav = "".join('<a href="#%s">%s <i>%s</i></a>' % (k, t.split(" ")[0] if k != "models" else "3D models", counts.get(k, len(raw)))
                  for k, t, _, _ in parts)
    body = "".join('<section id="%s"><h2>%s</h2><p class="lede">%s</p><div class="grid g-%s">%s</div></section>'
                   % (k, esc(t), d, k, c) for k, t, d, c in parts)
    page = TEMPLATE.replace("{NAV}", nav).replace("{BODY}", body)
    open(os.path.join(root, "index.html"), "w", encoding="utf-8").write(page)
    print("index.html:", counts, "raw", len(raw))


TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WildWaters Assets</title>
<style>
:root { --bg:#f4f1ea; --panel:#fffdf8; --ink:#1d2321; --muted:#5f6b66; --line:#ddd6c8; --accent:#1f7a74; --chip:#e9f2f0; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg:#121615; --panel:#1b211f; --ink:#e6ebe8; --muted:#97a39e; --line:#2c3532; --accent:#5cc2b8; --chip:#1f2d2a; } }
:root[data-theme="dark"] { --bg:#121615; --panel:#1b211f; --ink:#e6ebe8; --muted:#97a39e; --line:#2c3532; --accent:#5cc2b8; --chip:#1f2d2a; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:15px/1.5 system-ui, "Segoe UI", sans-serif; }
header { padding:28px 16px 12px; max-width:1240px; margin:0 auto; }
h1 { margin:0; font-size:28px; letter-spacing:-.01em; }
header p { margin:6px 0 0; color:var(--muted); max-width:760px; }
nav { position:sticky; top:0; z-index:5; background:color-mix(in srgb, var(--bg) 92%, transparent); backdrop-filter:blur(6px); border-bottom:1px solid var(--line); }
nav div { max-width:1240px; margin:0 auto; padding:8px 16px; display:flex; gap:6px; flex-wrap:wrap; }
nav a { text-decoration:none; color:var(--ink); background:var(--chip); padding:4px 10px; border-radius:999px; font-size:13px; }
nav a i { font-style:normal; color:var(--muted); margin-left:4px; }
main { max-width:1240px; margin:0 auto; padding:0 16px 60px; }
section { padding-top:28px; }
h2 { font-size:20px; margin:0 0 4px; }
.lede { color:var(--muted); margin:0 0 14px; max-width:820px; }
.lede input { display:block; margin-top:10px; width:min(320px,100%); padding:6px 10px; border:1px solid var(--line); border-radius:8px; background:var(--panel); color:var(--ink); font:inherit; }
a { color:var(--accent); }
.grid { display:grid; gap:10px; }
.g-screens { grid-template-columns:repeat(auto-fill,minmax(240px,1fr)); }
.g-fonts { grid-template-columns:repeat(auto-fill,minmax(180px,1fr)); }
.g-textures { grid-template-columns:repeat(auto-fill,minmax(112px,1fr)); }
.g-models { grid-template-columns:repeat(auto-fill,minmax(360px,1fr)); }
.g-music, .g-sounds { grid-template-columns:1fr; }
.card { margin:0; background:var(--panel); border:1px solid var(--line); border-radius:10px; overflow:hidden; display:flex; flex-direction:column; }
.card > a { display:flex; align-items:center; justify-content:center; padding:10px; min-height:96px;
  background:repeating-conic-gradient(color-mix(in srgb, var(--line) 60%, transparent) 0 25%, transparent 0 50%) 0 0/16px 16px; }
.card img { max-width:100%; max-height:200px; }
.card.big img { max-height:220px; }
img.px { image-rendering:pixelated; min-width:64px; min-height:24px; object-fit:contain; }
.tex img.px { width:80px; height:80px; }
figcaption { padding:6px 10px 8px; font-size:12px; color:var(--muted); display:flex; flex-direction:column; gap:1px; overflow-wrap:anywhere; }
figcaption b { color:var(--ink); font-size:14px; }
.model .shots { display:grid; grid-template-columns:1fr 1fr; gap:2px; background:var(--line); }
.model .shots a:only-child { grid-column:span 2; }
.model .shots img { width:100%; display:block; aspect-ratio:16/9; object-fit:cover; }
.track, .wave { display:flex; align-items:center; gap:12px; background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:8px 12px; flex-wrap:wrap; }
.track span { min-width:260px; font-weight:600; }
.track a { font-size:12px; color:var(--muted); }
audio { height:32px; max-width:100%; }
details { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:10px 12px; }
details + details { margin-top:8px; }
summary { cursor:pointer; }
summary span { color:var(--muted); font-size:13px; margin-left:6px; }
.waves { display:grid; grid-template-columns:repeat(auto-fill,minmax(300px,1fr)); gap:6px; margin-top:10px; }
.wave { padding:6px 10px; gap:8px; }
.wave span { font-weight:600; min-width:36px; }
.wave small { color:var(--muted); flex:1; min-width:90px; }
.wave audio { width:100%; }
code { background:var(--chip); padding:0 4px; border-radius:4px; }
@media (max-width:520px) { .g-models { grid-template-columns:1fr; } .track span { min-width:0; } }
</style>
</head>
<body>
<header>
<h1>WildWaters assets</h1>
<p>Everything extracted from the unreleased WildWaters N64 prototype (Jun 15 1999): screens, fonts, textures,
3D models, music and every sound sample. Click any picture to open it full size.</p>
</header>
<nav><div>{NAV}</div></nav>
<main>{BODY}</main>
<script>
const q = document.getElementById('q');
if (q) q.addEventListener('input', () => {
  const v = q.value.trim().toLowerCase();
  document.querySelectorAll('.tex').forEach(c => { c.style.display = !v || c.dataset.q.includes(v) ? '' : 'none'; });
});
document.addEventListener('play', e => {
  document.querySelectorAll('audio').forEach(a => { if (a !== e.target) a.pause(); });
}, true);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
