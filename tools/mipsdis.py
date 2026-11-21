"""dis.py ROM syms.toml ADDR [ADDR...]: disassemble functions with names for jal targets."""
import re,sys,struct
from capstone import *
rom=open(sys.argv[1],"rb").read()
fs=re.findall(r'name = "(\w+)", vram = 0x(\w+), size = 0x(\w+)',open(sys.argv[2]).read())
nm={int(v,16):(n,int(s,16)) for n,v,s in fs}
md=Cs(CS_ARCH_MIPS,CS_MODE_MIPS64+CS_MODE_BIG_ENDIAN)
for a in sys.argv[3:]:
    a=int(a,16); n,s=nm[a]
    print("==== %s %08X size %X"%(n,a,s))
    for i in md.disasm(rom[a-0x80007000:a-0x80007000+s],a):
        op=i.op_str
        if i.mnemonic in("jal","j"):
            t=int(op,16); op=nm.get(t,(op,))[0]
        print("  %08X %-8s %s"%(i.address,i.mnemonic,op))
