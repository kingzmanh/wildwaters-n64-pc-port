"""Writes index.html for the Destruction Derby 64 leftovers found in the WildWaters ROM (8-16 MB).

  python -I tools/dd64_index.py "../Destruction Derby 64 leftovers"

Needs tools/dd64_pictures.py output (pictures_exact/) and the ROM (for the screen records).
"""
import html
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assets_index import TEMPLATE  # same page style as the WildWaters asset viewer

ROM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "baserom.z64")


def esc(s):
    return html.escape(s, quote=True)


def main():
    root = sys.argv[1]
    parts = []
    # exact pictures + fonts, from DD64's own tables
    pics = {}
    cards, fcards = [], []
    for line in open(os.path.join(root, "pictures_exact", "LIST.md"), encoding="utf-8"):
        m = re.match(r"\| (\S+) \| 0x([0-9A-F]+) \| 0x([0-9A-F]+) \| (\d+)x(\d+) \| (\w+) \| (\w+) \|", line)
        if not m:
            continue
        f, pid, rom, w, h, fmt, kind = m.groups()
        pics[int(pid, 16)] = f
        card = ('<figure class="card"><a href="pictures_exact/%s" target="_blank"><img class="px" src="pictures_exact/%s" alt="" loading="lazy"></a>'
                '<figcaption><span>id 0x%s · %sx%s</span><span>ROM 0x%s · %s</span></figcaption></figure>'
                % (esc(f), esc(f), pid, w, h, rom.lstrip("0"), fmt))
        (fcards if kind == "font" else cards).append(card)
    parts.append(("pictures", "Pictures",
                  "All 261 pictures, cut exactly with Destruction Derby 64's own picture table (ids, sizes and formats "
                  "survived in the ROM). They lie back to back from ROM 0x8CB3F0. Earlier pixel-guessed cuts: "
                  "<a href=\"pictures_carved_earlier/\">pictures_carved_earlier/</a>.", "".join(cards)))
    parts.append(("fonts", "Fonts", "The 21 font strips from DD64's font table (the same ids and sizes as WildWaters' fonts).",
                  "".join(fcards)))
    # menu screens: element records with their pictures
    rom = open(ROM, "rb").read()
    o = 0xBCBA40
    while struct.unpack(">I", rom[o:o + 4])[0] == 0x03F20000:
        o += 28
    blocks = []
    nscreens = 0
    while o < 0xBF0170:
        t = struct.unpack(">I", rom[o:o + 4])[0]
        if t == 0x03F10000:
            start, rows = o, []
            while struct.unpack(">I", rom[o:o + 4])[0] == t:
                b = rom[o:o + 0x48]
                w = struct.unpack(">18I", b)
                fx = struct.unpack(">18f", b)
                pid = w[5]
                thumb = ('<img class="px" src="pictures_exact/%s" alt="" loading="lazy">' % esc(pics[pid])) if pid in pics else ""
                rows.append("<tr><td>0x%X</td><td>%s</td><td>0x%X</td><td>%.0f, %.0f</td><td>%.2g x %.2g</td><td>%s</td></tr>"
                            % (w[1], thumb, pid, fx[7], fx[8], fx[11], fx[12], "on" if b[0x34] else "off"))
                o += 0x48
            nscreens += 1
            blocks.append('<details><summary><b>Screen %d</b> <span>ROM 0x%X · %d elements</span></summary>'
                          '<table class="els"><tr><th>element</th><th>picture</th><th>picture id</th><th>x, y</th><th>scale</th><th>start</th></tr>%s</table></details>'
                          % (nscreens, start, len(rows), "".join(rows)))
        else:
            o += 4
    parts.append(("screens", "Menu screens",
                  "Each menu screen's element list (DD64's own layout records), with the picture each element shows, "
                  "its position on the 320x240 screen and scale. Which elements show together was decided by DD64's "
                  "program code, which did not survive, so screens are listed, not drawn. Full record dump: "
                  "<a href=\"menu_records/SCREENS.md\">SCREENS.md</a>, <a href=\"menu_records/records.csv\">records.csv</a>.",
                  "".join(blocks)))
    tp = os.path.join(root, "track_paths")
    tcards = []
    if os.path.isdir(tp):
        for f in sorted(x for x in os.listdir(tp) if x.endswith(".png")):
            stem = f[:-4]
            road = os.path.exists(os.path.join(tp, stem + "_road.obj"))
            tcards.append('<figure class="card"><a href="track_paths/%s" target="_blank"><img src="track_paths/%s" alt="" loading="lazy"></a>'
                          '<figcaption><span>%s</span><span><a href="track_paths/%s.obj">path .obj</a>%s</span></figcaption></figure>'
                          % (esc(f), esc(f), esc(stem), esc(stem), (' · <a href="track_paths/%s_road.obj">road .obj</a>' % esc(stem)) if road else ""))
    parts.append(("tracks", "Track layouts",
                  "3D track outlines found after DD64's music table (ROM 0xBDA2B0): the long loops are the tracks' "
                  "edges as left/right point pairs, rebuilt as road surfaces (_road.obj). Colour = height. Which track "
                  "each one is was not stored with them. Short point runs are markers/spawn points. "
                  "<a href=\"track_paths/LIST.md\">LIST.md</a>", "".join(tcards)))
    mus = sorted(f for f in os.listdir(os.path.join(root, "music")) if f.endswith(".wav"))
    rows = "".join('<div class="track"><span>Song %d</span><audio controls preload="none" src="music/%s"></audio><a href="music/%s">%s</a></div>'
                   % (int(re.search(r"(\d+)", f).group(1)), esc(f), esc(f), esc(f)) for f in mus)
    parts.append(("music", "Music",
                  "The 13 songs, played by the WildWaters PC port's music player with the ddmusic bank (river sounds of the "
                  "test race in the background). The song data itself is next to each WAV (.bin).", rows))
    blocks = []
    nwaves = 0
    for d, title, what in (("ddsfx_80", "ddsfx.wbk", "sound effects, 80 samples"),
                           ("ddmusic_129", "ddmusic.wbk", "music instruments, 129 samples"),
                           ("ddsfx_copy_cut_off", "ddsfx.wbk (second copy)", "cut off at the end of the ROM")):
        ws = sorted(f for f in os.listdir(os.path.join(root, "sounds", d)) if f.endswith(".wav"))
        nwaves += len(ws)
        items = "".join('<div class="wave"><span>%s</span><audio controls preload="none" src="sounds/%s/%s"></audio></div>'
                        % (esc(w[:-4].replace("wave_", "#")), esc(d), esc(w)) for w in ws)
        blocks.append('<details%s><summary><b>%s</b> <span>%s · %d samples</span></summary><div class="waves">%s</div></details>'
                      % (" open" if d == "ddsfx_80" else "", esc(title), esc(what), len(ws), items))
    parts.append(("sounds", "Sound banks", "Every sample decoded from N64 VADPCM to WAV (22050 Hz mono).", "".join(blocks)))
    raws = sorted(f for f in os.listdir(os.path.join(root, "raw")) if f.endswith(".bin"))
    rows = "".join('<div class="track"><span>%s</span><a href="raw/%s">%s</a></div>' % (esc(f.split("_0x")[0][3:].replace("_", " ")), esc(f), esc(f)) for f in raws)
    parts.append(("raw", "Raw data",
                  "The whole leftover area cut into its zones, byte for byte. <a href=\"CENSUS.md\">CENSUS.md</a> maps it block "
                  "by block; <a href=\"README.md\">README.md</a> explains what was found.", rows))
    counts = {"pictures": len(cards), "fonts": len(fcards), "screens": nscreens, "tracks": len(tcards), "music": len(mus), "sounds": nwaves, "raw": len(raws)}
    nav = "".join('<a href="#%s">%s <i>%d</i></a>' % (k, t, counts[k]) for k, t, _, _ in parts)
    body = "".join('<section id="%s"><h2>%s</h2><p class="lede">%s</p><div class="grid g-%s">%s</div></section>'
                   % (k, esc(t), d, "textures" if k in ("pictures", "fonts", "tracks") else ("sounds" if k == "screens" else k), c) for k, t, d, c in parts)
    page = TEMPLATE.replace("{NAV}", nav).replace("{BODY}", body)
    page = page.replace("<title>WildWaters Assets</title>", "<title>Destruction Derby 64 Leftovers</title>")
    page = re.sub(r"<header>.*?</header>", "<header><h1>Destruction Derby 64 leftovers</h1><p>Found in the unused second half "
                  "(8-16 MB) of the WildWaters prototype ROM: the development cartridge held a Destruction Derby 64 build "
                  "(Looking Glass Studios / Psygnosis) before WildWaters was written over its first 8 MB. Click any picture "
                  "to open it full size.</p></header>", page, flags=re.S)
    page = page.replace(".tex img.px { width:80px; height:80px; }",
                        ".g-textures img.px { width:100%; height:120px; object-fit:contain; }\n"
                        "table.els { border-collapse:collapse; margin-top:8px; font-size:13px; }\n"
                        "table.els td, table.els th { border-bottom:1px solid var(--line); padding:3px 8px; text-align:left; }\n"
                        "table.els img.px { max-height:48px; max-width:140px; min-width:0; min-height:0; }")
    page = page.replace(".g-textures { grid-template-columns:repeat(auto-fill,minmax(112px,1fr)); }",
                        ".g-textures { grid-template-columns:repeat(auto-fill,minmax(170px,1fr)); }")
    open(os.path.join(root, "index.html"), "w", encoding="utf-8").write(page)
    print(counts)


if __name__ == "__main__":
    main()
