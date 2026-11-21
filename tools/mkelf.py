"""Wraps the ROM's main segment (vram 0x80008000-0x80092B40) in a big-endian MIPS ELF with
function symbols from recomp/ww.syms.toml, for Ghidra/REA.  python tools/mkelf.py baserom.z64 out.elf"""
import re, struct, sys
rom = open(sys.argv[1], "rb").read()
VB, VE, RS = 0x80008000, 0x80092B40, 0x1000
seg = rom[RS:RS + (VE - VB)]
funcs = re.findall(r'name = "(\w+)", vram = 0x(\w+), size = 0x(\w+)', open("recomp/ww.syms.toml").read())
strtab = b"\0"; syms = [b"\0" * 16]
for n, v, s in funcs:
    off = len(strtab); strtab += n.encode() + b"\0"
    syms.append(struct.pack(">IIIBBH", off, int(v, 16), int(s, 16), 0x12, 0, 1))
shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
ehsz, phsz, shsz = 52, 32, 40
text_off = 0x1000
sym_off = text_off + len(seg)
str_off = sym_off + 16 * len(syms)
shs_off = str_off + len(strtab)
sh_off = (shs_off + len(shstr) + 3) & ~3
eh = b"\x7fELF" + bytes([1, 2, 1, 0]) + b"\0" * 8
eh += struct.pack(">HHIIIIIHHHHHH", 2, 8, 1, VB, ehsz, sh_off, 0x1000, ehsz, phsz, 1, shsz, 5, 4)
ph = struct.pack(">IIIIIIII", 1, text_off, VB, VB, len(seg), len(seg) + 0xDA860, 7, 0x1000)
def sh(name, typ, flags, addr, off, size, link=0, info=0, align=4, ent=0):
    return struct.pack(">IIIIIIIIII", name, typ, flags, addr, off, size, link, info, align, ent)
shdrs = sh(0, 0, 0, 0, 0, 0) + sh(1, 1, 7, VB, text_off, len(seg), align=16) + \
    sh(7, 2, 0, 0, sym_off, 16 * len(syms), link=3, info=1, ent=16) + sh(15, 3, 0, 0, str_off, len(strtab), align=1) + \
    sh(23, 3, 0, 0, shs_off, len(shstr), align=1)
out = bytearray(eh + ph); out += b"\0" * (text_off - len(out)); out += seg
out += b"".join(syms) + strtab + shstr; out += b"\0" * (sh_off - len(out)); out += shdrs
open(sys.argv[2], "wb").write(out)
print("wrote", sys.argv[2], len(out))
