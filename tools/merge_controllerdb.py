"""Builds host/assets/controllerdb.txt: the full SDL_GameControllerDB (community mappings for every known
game controller, zlib license) merged from the copies given, plus the port's N64 pad / adapter entries,
which come last so they win for those devices.

  python -I tools/merge_controllerdb.py OUT n64_entries.txt db1.txt [db2.txt ...]

Entries are keyed by (GUID, platform); a later file's entry replaces an earlier one, so pass the
newest database last.
"""
import re
import sys


def entries(path):
    out = []
    for line in open(path, encoding="utf-8", errors="ignore"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        if len(parts) < 3 or len(parts[0]) != 32:
            continue
        m = re.search(r"platform:([^,]*)", line)
        out.append(((parts[0].lower(), m.group(1) if m else ""), line if line.endswith(",") else line + ","))
    return out


def main():
    out_path, n64, dbs = sys.argv[1], sys.argv[2], sys.argv[3:]
    merged = {}
    for db in dbs:
        for key, line in entries(db):
            merged[key] = line
    n64_entries = entries(n64)
    for key, _ in n64_entries:
        merged.pop(key, None)
    platforms = {}
    for (g, p) in merged:
        platforms[p] = platforms.get(p, 0) + 1
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Game controller mappings for SDL, loaded at startup (host/src/frontend.cpp).\n")
        f.write("# 1) SDL_GameControllerDB (https://github.com/mdqinc/SDL_GameControllerDB, zlib license):\n")
        f.write("#    community mappings for every known controller, merged from local copies.\n")
        f.write("# 2) Last, so they win: N64 pads and adapters for real N64 controllers (raphnet-tech etc.),\n")
        f.write("#    which SDL's own mappings lack or map as other pads.\n")
        for key in sorted(merged, key=lambda k: (k[1], merged[k].split(",")[1].lower())):
            f.write(merged[key] + "\n")
        f.write("# --- N64 pads and adapters ---\n")
        for _, line in n64_entries:
            f.write(line + "\n")
    print("mappings:", len(merged), "+", len(n64_entries), "N64;", platforms)


if __name__ == "__main__":
    main()
