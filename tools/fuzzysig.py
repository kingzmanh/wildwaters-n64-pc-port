"""Fuzzy-match unnamed functions that touch hardware (cop0, cache, KSEG1 I/O) or are
small leaf helpers against SDK library functions, by opcode-shape similarity.

  python fuzzysig.py ROM syms.toml lib.a [lib.a...]
"""
import difflib
import re
import struct
import sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
import libsig

def shape(b):
    out = []
    for i in range(0, len(b) - 3, 4):
        w = struct.unpack(">I", b[i:i + 4])[0]
        op = w >> 26
        if op == 0:
            out.append("s%d" % (w & 0x3F))
        elif op in (16, 17):
            out.append("c%d_%d" % (op, (w >> 21) & 31))
        elif op == 0x2F:
            out.append("cache%d" % ((w >> 16) & 31))
        else:
            out.append("o%d" % op)
    return out

def main():
    rom = open(sys.argv[1], "rb").read()
    funcs = re.findall(r'name = "(\w+)", vram = 0x(\w+), size = 0x(\w+)', open(sys.argv[2]).read())
    libf = []
    for lib in sys.argv[3:]:
        for m, obj in libsig.ar_members(open(lib, "rb").read()):
            for n, fb, fm in libsig.elf_funcs(obj):
                libf.append((n, shape(fb), len(fb)))
    seen = set()
    libf = [x for x in libf if not (x[0] in seen or seen.add(x[0]))]
    for name, v, sz in funcs:
        if not name.startswith("func_"):
            continue
        v = int(v, 16); sz = int(sz, 16)
        b = rom[v - 0x80007000:v - 0x80007000 + sz]
        ws = struct.unpack(">%dI" % (sz // 4), b)
        hw = any(w >> 26 in (16, 0x2F) for w in ws) or any(w >> 26 == 0xF and (w & 0xFFFF) in range(0xA400, 0xA4B0) for w in ws) or any(w >> 26 == 0xF and (w & 0xFFFF) in (0xA000, 0xB000, 0x8000) for w in ws)
        if not hw and sz > 0x200:
            continue
        sh = shape(b)
        best = []
        for n, ls, ln in libf:
            if abs(ln - sz) > max(0x40, sz // 3):
                continue
            r = difflib.SequenceMatcher(None, sh, ls, autojunk=False).ratio()
            best.append((r, n, ln))
        best.sort(reverse=True)
        if best and (best[0][0] > 0.8 or hw):
            print("%s size %4X hw=%d  %s" % (name, sz, hw, "  ".join("%s(%X) %.2f" % (n, ln, r) for r, n, ln in best[:3])))

main()
