"""Find libultra functions in a ROM by matching relocation-masked code from the SDK .a
files (used as a reference only; nothing from them is copied into the port).

  python libsig.py ROM VRAM_BASE ROM_START ROM_END lib1.a [lib2.a ...] > matches.txt

Prints "vram size name lib" for every function whose masked bytes match exactly once.
"""
import struct
import sys

def ar_members(data):
    assert data[:8] == b"!<arch>\n"
    off = 8
    longnames = b""
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].decode("latin1").strip()
        size = int(hdr[48:58].decode().strip())
        body = data[off + 60:off + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            i = int(name[1:])
            name = longnames[i:longnames.index(b"/", i)].decode()
        if name not in ("/", "//", "__.SYMDEF", "__.SYMDEF SORTED"):
            yield name.rstrip("/"), body
        off += 60 + size + (size & 1)

def elf_funcs(obj):
    """Return list of (name, bytes, masked-mask list) for every FUNC symbol in .text."""
    if obj[:4] != b"\x7fELF" or obj[5] != 2:
        return []
    shoff, = struct.unpack(">I", obj[0x20:0x24])
    shentsize, shnum, shstrndx = struct.unpack(">HHH", obj[0x2E:0x34])
    secs = []
    for i in range(shnum):
        s = struct.unpack(">IIIIIIIIII", obj[shoff + i * shentsize:shoff + i * shentsize + 40])
        secs.append(s)
    shstr = secs[shstrndx]
    def secname(s):
        o = shstr[4] + s[0]
        return obj[o:obj.index(b"\0", o)].decode()
    names = [secname(s) for s in secs]
    symtab = next((s for s in secs if s[1] == 2), None)
    if symtab is None:
        return []
    strtab = secs[symtab[6]]
    syms = []
    for i in range(symtab[5] // 16):
        o = symtab[4] + i * 16
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack(">IIIBBH", obj[o:o + 16])
        n = obj[strtab[4] + st_name:obj.index(b"\0", strtab[4] + st_name)].decode()
        syms.append((n, st_value, st_size, st_info, st_shndx))
    out = []
    for ti, s in enumerate(secs):
        if names[ti] != ".text" or s[5] == 0:
            continue
        text = bytearray(obj[s[4]:s[4] + s[5]])
        mask = bytearray(b"\xff" * len(text))
        for ri, r in enumerate(secs):
            if r[1] == 9 and r[7] == ti:  # SHT_REL for .text
                for k in range(r[5] // 8):
                    ro, rinfo = struct.unpack(">II", obj[r[4] + k * 8:r[4] + k * 8 + 8])
                    t = rinfo & 0xFF
                    if t == 4:  # R_MIPS_26
                        mask[ro] &= 0xFC; mask[ro + 1] = 0; mask[ro + 2] = 0; mask[ro + 3] = 0
                    elif t in (5, 6, 7):  # HI16 LO16 GPREL16
                        mask[ro + 2] = 0; mask[ro + 3] = 0
                    elif t == 2:  # R_MIPS_32
                        mask[ro:ro + 4] = b"\0\0\0\0"
        fsyms = sorted((v, n, sz) for n, v, sz, info, shn in syms if shn == ti and (info & 0xF) in (0, 2) and n and not n.startswith("."))
        # Collapse duplicates at one address (keep a global-looking name)
        seen = {}
        for v, n, sz in fsyms:
            if v not in seen or (seen[v][0].startswith("$") and not n.startswith("$")):
                seen[v] = (n, sz)
        addrs = sorted(seen)
        for j, v in enumerate(addrs):
            n, sz = seen[v]
            end = addrs[j + 1] if j + 1 < len(addrs) else len(text)
            if sz == 0 or v + sz > end:
                sz = end - v
            if sz >= 8:
                out.append((n, bytes(text[v:v + sz]), bytes(mask[v:v + sz])))
    return out

def main():
    rom = open(sys.argv[1], "rb").read()
    vbase = int(sys.argv[2], 16); rstart = int(sys.argv[3], 16); rend = int(sys.argv[4], 16)
    code = rom[rstart:rend]
    words = [code[i:i + 4] for i in range(0, len(code) - 3, 4)]
    index = {}
    for i, w in enumerate(words):
        index.setdefault(w, []).append(i)
    found = {}
    for lib in sys.argv[5:]:
        for mname, obj in ar_members(open(lib, "rb").read()):
            for fname, fb, fm in elf_funcs(obj):
                if fname in found:
                    continue
                # anchor on first fully unmasked word
                anchor = None
                for k in range(0, len(fb), 4):
                    if fm[k:k + 4] == b"\xff\xff\xff\xff" and fb[k:k + 4] != b"\0\0\0\0":
                        anchor = k; break
                if anchor is None:
                    continue
                hits = []
                for wi in index.get(fb[anchor:anchor + 4], []):
                    st = wi * 4 - anchor
                    if st < 0 or st + len(fb) > len(code) or st % 4:
                        continue
                    seg = code[st:st + len(fb)]
                    if all((seg[k] & fm[k]) == (fb[k] & fm[k]) for k in range(len(fb))):
                        hits.append(st)
                if len(hits) == 1:
                    found[fname] = (vbase + hits[0], len(fb), lib.split("/")[-1] + ":" + mname)
    for n, (v, sz, src) in sorted(found.items(), key=lambda x: x[1][0]):
        print("%08X %6X %-32s %s" % (v, sz, n, src))

if __name__ == "__main__":
    main()
