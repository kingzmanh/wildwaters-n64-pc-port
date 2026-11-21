"""Split WildWaters' flat .text into functions for N64Recomp (no symbols exist).

  python funcfind.py ROM VRAM ROM_START TEXT_END_VRAM libultra_matches.txt out_syms.toml

Linear sweep: a function runs from its start until a `jr $ra` (or unconditional
`j`/`b` out of the function) with no pending branch target past it; the next non-nop
word starts the next function. jal targets and libultra matches force splits.
Jump tables (lui/addiu|lw base before `jr $reg`) extend the pending target range.
"""
import struct
import sys

def s16(x):
    return x - 0x10000 if x & 0x8000 else x

def main():
    rom = open(sys.argv[1], "rb").read()
    vbase = int(sys.argv[2], 16); rs = int(sys.argv[3], 16); tend = int(sys.argv[4], 16)
    W = lambda v: struct.unpack(">I", rom[v - vbase + rs:v - vbase + rs + 4])[0]
    names = {}
    # names files: comma-separated; later files win (manual IDs override signature hits)
    for path in sys.argv[5].split(","):
        for line in open(path):
            p = line.split()
            if p and not line.startswith("#"):
                names[int(p[0], 16)] = p[2]
    # RSP microcode blobs linked into .text (rspboot, aspMain, F3DEX2 2.07, S2DEX2 2.07)
    # and the exception handler assembly, which never runs on PC.
    EXCLUDE = [(0x80014AD0, 0x800159C0), (0x800161B0, 0x80018E38),
               (0x800243C0, 0x800251A0)]  # exceptasm.s: replaced by the runtime
    excluded = lambda v: any(a <= v < b for a, b in EXCLUDE)
    forced = {v for v in names if vbase <= v < tend}
    forced |= {b for a, b in EXCLUDE}
    for v in range(vbase, tend, 4):
        if excluded(v):
            continue
        w = W(v)
        if w >> 26 == 3:
            t = (v & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if vbase <= t < tend:
                forced.add(t)

    def table_targets(pc):
        out = []
        for back in range(4, 80, 4):
            b = W(pc - back)
            if b >> 26 == 0xF:
                hi = (b & 0xFFFF) << 16; reg = (b >> 16) & 31
                for fw in range(pc - back + 4, pc, 4):
                    x = W(fw)
                    if (x >> 26) in (0x23, 9) and ((x >> 21) & 31) == reg:
                        i = (hi + s16(x & 0xFFFF)) & 0xFFFFFFFF
                        while True:
                            off = i - vbase + rs
                            if off < 0 or off + 4 > len(rom):
                                break
                            e = W(i)
                            if not (vbase <= e < tend):
                                break
                            out.append(e); i += 4
                        return out
        return out

    funcs = []
    v = vbase
    while v < tend:
        # Skip nop padding between functions, but never past a call target or named
        # function: some functions start with a nop (e.g. 0x800395E8).
        while v < tend and W(v) == 0 and v not in forced:
            v += 4
        if v >= tend:
            break
        if excluded(v):
            v = next(b for a, b in EXCLUDE if a <= v < b)
            continue
        start = v; maxt = v
        while v < tend:
            w = W(v); op = w >> 26; r1 = (w >> 21) & 31; r2 = (w >> 16) & 31
            if v != start and v in forced:
                break
            end_here = False
            if op in (4, 5, 6, 7, 20, 21, 22, 23) or op == 1 or (op == 17 and r1 == 8):
                t = v + 4 + (s16(w & 0xFFFF) << 2)
                if t > maxt:
                    maxt = t
                if op == 4 and r1 == 0 and r2 == 0 and t <= v:
                    end_here = v + 4 >= maxt  # backwards b with nothing pending
            elif op == 2:
                t = (v & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if start <= t <= v + 0x4000 and t > maxt and t not in forced:
                    maxt = t
                elif v + 4 >= maxt:
                    end_here = True  # tail call
            elif op == 0 and (w & 0x3F) == 8:
                if r1 == 31:
                    end_here = v + 4 >= maxt
                else:
                    tt = [t for t in table_targets(v) if t >= start]
                    if tt:
                        maxt = max(maxt, max(tt))
                    else:
                        end_here = v + 4 >= maxt
            if end_here:
                v += 8
                break
            v += 4
        funcs.append([start, v - start])
    # Merge epilogue fragments (a split caused by a backwards branch/jump into the body):
    # a piece that starts by restoring $ra/$s regs, is not a call target or a named
    # function, and directly follows the previous piece belongs to that piece.
    jal_targets = set()
    for v in range(vbase, tend, 4):
        if not excluded(v) and W(v) >> 26 == 3:
            jal_targets.add((v & 0xF0000000) | ((W(v) & 0x3FFFFFF) << 2))
    merged = []
    for s, sz in funcs:
        w = W(s)
        restore = (w >> 26) == 0x23 and ((w >> 21) & 31) == 29 and ((w >> 16) & 31) in (31, 16, 17, 18, 19, 20, 21, 22, 23, 30)
        if merged and restore and s not in jal_targets and s not in names and merged[-1][0] + merged[-1][1] <= s <= merged[-1][0] + merged[-1][1] + 0x10:
            merged[-1][1] = s + sz - merged[-1][0]
        else:
            merged.append([s, sz])
    print("merged %d epilogue fragments" % (len(funcs) - len(merged)))
    funcs = merged
    # Shared tails (hand-written asm helpers): a branch from one function into the middle
    # of another. N64Recomp turns a branch to a function *start* into a tail call, so give
    # every such target its own entry: split the containing function there when the code
    # before it can't fall through, otherwise add an overlapping entry to the same end.
    def is_term(a):
        w = W(a); op = w >> 26
        return (op == 0 and (w & 0x3F) == 8) or op == 2 or (op == 4 and (w >> 16) & 0x3FF == 0)
    extra = 0
    for _ in range(10):
        starts = {s for s, _ in funcs}
        new_t = set()
        for s, sz in funcs:
            for a in range(s, s + sz, 4):
                w = W(a); op = w >> 26; r1 = (w >> 21) & 31
                t = None
                if op in (4, 5, 6, 7, 20, 21, 22, 23) or op == 1 or (op == 17 and r1 == 8):
                    if op == 1 and ((w >> 16) & 31) in (16, 17, 18, 19):
                        continue  # bltzal/bgezal are calls
                    t = a + 4 + (s16(w & 0xFFFF) << 2)
                elif op == 2:
                    t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if t is not None and not (s <= t < s + sz) and t not in starts and vbase <= t < tend:
                    new_t.add(t)
        if not new_t:
            break
        for t in sorted(new_t):
            for f in funcs:
                if f[0] < t < f[0] + f[1]:
                    end = f[0] + f[1]
                    if is_term(t - 8):
                        f[1] = t - f[0]
                    funcs.append([t, end - t])
                    extra += 1
                    break
        funcs.sort()
    print("added %d shared-tail entries" % extra)

    # Coverage pass. N64Recomp keeps HI/LO local to each function and can't fall through from
    # one function into the next, so a function must contain every instruction it can reach
    # without a call or return: the hand-written 64-bit divide entries (0x800141F0, 0x8001422C,
    # 0x80014240) set a flag in HI and branch into a shared core at 0x800142C8, and small
    # pieces such as 0x800144B4 fall through into the next entry. Extend each function over
    # (a) the code it falls into when its last instruction pair isn't a jump/return and
    # (b) the code its forward branches reach in another function. Overlapping functions
    # are fine: each is recompiled on its own.
    def ends_cleanly(e):
        return is_term(e - 8) or (W(e - 8) >> 26 == 4 and ((W(e - 8) >> 16) & 0x3FF) == 0)
    def end_of(addr):
        best = None
        for s, sz in funcs:
            if s <= addr < s + sz:
                best = max(best or 0, s + sz)
        return best
    extended = 0
    for _ in range(20):
        changed = False
        for f in funcs:
            s, e = f[0], f[0] + f[1]
            new_e = e
            if not ends_cleanly(e) and e < tend:
                n = end_of(e)
                if n and n > new_e:
                    new_e = n
            for a in range(s, e, 4):
                w = W(a); op = w >> 26; r1 = (w >> 21) & 31
                if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((w >> 16) & 31) not in (16, 17, 18, 19)) or (op == 17 and r1 == 8):
                    t = a + 4 + (s16(w & 0xFFFF) << 2)
                    if e <= t < tend and t - e < 0x1000 and not excluded(t):
                        n = end_of(t)
                        if n and n > new_e:
                            new_e = n
            if new_e != e:
                f[1] = new_e - s
                changed = True
                extended += 1
        if not changed:
            break
    print("extended %d functions over fall-through / shared code" % extended)
    # Function pointers (thread entries, callbacks): lui/addiu address pairs and data
    # words that point at an instruction right after a function-ending jump.
    ptr_t = set()
    for s, sz in funcs:
        for a in range(s, s + sz, 4):
            w = W(a)
            if w >> 26 == 0xF:
                reg = (w >> 16) & 31; hi = (w & 0xFFFF) << 16
                for f in range(a + 4, min(a + 48, s + sz), 4):
                    x = W(f)
                    if (x >> 26) == 9 and ((x >> 21) & 31) == reg:
                        ptr_t.add((hi + s16(x & 0xFFFF)) & 0xFFFFFFFF)
                        break
    data_end = int(sys.argv[7], 16) if len(sys.argv) > 7 else tend
    for v in range(tend, data_end, 4):
        ptr_t.add(W(v))
    # Jump-table case labels are pointers too, but into the middle of their function.
    case_labels = set()
    for v in range(vbase, tend, 4):
        w = W(v)
        if not excluded(v) and w >> 26 == 0 and (w & 0x3F) == 8 and ((w >> 21) & 31) != 31:
            case_labels.update(table_targets(v))
    def ends_func(a):
        w = W(a)
        if w == 0x03E00008:
            return True  # jr $ra
        if w >> 26 == 4 and (w >> 16) & 0x3FF == 0 and s16(w & 0xFFFF) == -1:
            return True  # b . (idle loop)
        return False
    starts = {s for s, _ in funcs}
    added = 0
    for t in sorted(ptr_t):
        if t % 4 or not (vbase <= t < tend) or t in starts or excluded(t) or t in case_labels:
            continue
        for f in funcs:
            if f[0] < t < f[0] + f[1]:
                p = t - 8
                while p > f[0] and W(p + 4) == 0 and W(p) == 0:
                    p -= 4
                if ends_func(p):
                    end = f[0] + f[1]
                    f[1] = t - f[0]
                    funcs.append([t, end - t])
                    added += 1
                break
    funcs.sort()
    print("added %d pointer-target entries" % added)
    out = open(sys.argv[6], "w")
    out.write("# WildWaters (Jun 15 1999 proto) code for N64Recomp. Generated by tools/funcfind.py.\n\n")
    out.write("[[section]]\nname = \".text\"\nrom = 0x%08X\nvram = 0x%08X\nsize = 0x%X\n\nfunctions = [\n" % (rs, vbase, tend - vbase))
    used = set()
    for s, sz in funcs:
        n = names.get(s, "func_%08X" % s)
        if n in used:
            n = "%s_%08X" % (n, s)
        used.add(n)
        out.write("    { name = \"%s\", vram = 0x%08X, size = 0x%X },\n" % (n, s, sz))
    out.write("]\n")
    print("functions %d (named %d)" % (len(funcs), sum(1 for s, _ in funcs if s in names)))
    missing = [hex(v) for v in names if vbase <= v < tend and v not in {s for s, _ in funcs}]
    print("named starts not split:", missing[:20])

if __name__ == "__main__":
    main()
