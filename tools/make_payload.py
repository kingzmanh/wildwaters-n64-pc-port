"""Packs the PC runtime files into one archive that the setup program embeds (host/installer/setup.c).

  python -I tools/make_payload.py host/build-win host/installer/payload.bin <VC redist CRT folder>

The Visual C++ runtime DLLs the exe needs (msvcp140, vcruntime140, ...) come from Visual Studio's
redistributable folder (Microsoft.VC14x.CRT); Microsoft allows shipping them next to the program.

Archive: "WWPK", u32 version 1, u32 file count, then per file: u16 path length, path (UTF-8, '\\'
separators, relative to the install folder), u64 size, data. Only the program files go in: no ROM,
nothing from the game.
"""
import os
import struct
import sys

FILES = ["WildWatersPC.exe", "SDL2.dll", "dxcompiler.dll", "dxil.dll"]
CRT = ["msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"]
DIRS = ["assets"]


def main():
    src, out = sys.argv[1], sys.argv[2]
    items = []
    for f in FILES:
        items.append((f, os.path.join(src, f)))
    if len(sys.argv) > 3:
        for f in CRT:
            items.append((f, os.path.join(sys.argv[3], f)))
    for d in DIRS:
        for root, _, names in os.walk(os.path.join(src, d)):
            for n in sorted(names):
                full = os.path.join(root, n)
                items.append((os.path.relpath(full, src).replace("/", "\\"), full))
    total = 0
    with open(out, "wb") as o:
        o.write(b"WWPK" + struct.pack("<II", 1, len(items)))
        for rel, full in items:
            data = open(full, "rb").read()
            p = rel.encode("utf-8")
            o.write(struct.pack("<H", len(p)) + p + struct.pack("<Q", len(data)) + data)
            total += len(data)
    print("payload: %d files, %.1f MB -> %s" % (len(items), total / 1048576, out))


if __name__ == "__main__":
    main()
