"""Decodes WildWaters' libmus sample banks ("N64 PtrTablesV2" + "N64 WaveTables") to WAV files.

  python -I tools/vadpcm_banks.py baserom.z64 ../assets/sounds

ptr_bank_t (libmus.h, SDK 2.0L): name[16], flags, wbk_name[12], count, basenote*, detune*,
wave_list* (offsets from the bank start). ALWaveTable: base (offset into the wave bank), len,
type (0 VADPCM, 1 raw 16-bit), flags, loop*, book*. ALADPCMBook: order, npredictors, s16 book[].
The game's audio runs at 22050 Hz; the WAVs use that rate (a sample's own pitch comes from the
song/effect that plays it).
"""
import os
import struct
import sys
import wave

BANKS = [
    ("bank38_used", 0x31AFB0, 0x31CB70, 0x50E2F0, "used by this build (sound effects + race music)"),
    ("dd64_ddsfx_80", 0xBF0170, 0xBF3E20, 0xDCB230, "Destruction Derby 64 ddsfx.wbk (left on the cartridge), unused"),
    ("dd64_ddmusic_129", 0xDCB230, 0xDD1300, 0xF7F6A0, "Destruction Derby 64 ddmusic.wbk (left on the cartridge), instruments of the unplayed songs"),
    ("dd64_ddsfx_copy_cut_off", 0xFEE170, 0xFF1E20, 0x1000000, "second copy of Destruction Derby 64 ddsfx.wbk, cut off at 16 MB"),
]
RATE = 22050


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode_vadpcm(data, book, order, npred):
    out = []
    last = [0] * 8
    for f in range(len(data) // 9):
        fr = data[f * 9:f * 9 + 9]
        scale = 1 << (fr[0] >> 4)
        pi = fr[0] & 0xF
        if pi >= npred:
            pi = 0
        coef = book[pi * order * 8:(pi + 1) * order * 8]
        c0 = coef[0:8] if order >= 2 else [0] * 8
        c1 = coef[(order - 1) * 8:order * 8]
        nib = []
        for b in fr[1:]:
            for n in (b >> 4, b & 15):
                nib.append((n - 16 if n >= 8 else n) * scale)
        for half in range(2):
            ins = nib[half * 8:half * 8 + 8]
            cur = [0] * 8
            for i in range(8):
                total = c0[i] * last[6] + c1[i] * last[7]
                for j in range(i):
                    total += ins[(i - 1) - j] * c1[j]
                total += ins[i] << 11
                cur[i] = clamp16(total >> 11)
            out.extend(cur)
            last = cur
    return out


def main():
    rom = open(sys.argv[1], "rb").read()
    out_root = sys.argv[2]
    os.makedirs(out_root, exist_ok=True)
    lines = ["# WildWaters sample banks (decoded VADPCM -> 16-bit mono WAV, 22050 Hz)\n",
             "Each bank is a libmus pointer bank plus its wave data. Loops are listed in samples.\n"]
    # This build's effects bank (ROM 0x701630): 40 effects, each one note on one wave of the used bank;
    # the effect's data starts with 0x81 <wave>.
    FX = 0x701630
    n_fx = struct.unpack(">I", rom[FX:FX + 4])[0]
    fx_of_wave = {}
    for k in range(n_fx):
        off = struct.unpack(">I", rom[FX + 0x18 + 8 * k:FX + 0x1C + 8 * k])[0]
        if rom[FX + off] == 0x81:
            fx_of_wave.setdefault(rom[FX + off + 1], []).append(k)
    for name, ptr, wbk, end, what in BANKS:
        d = os.path.join(out_root, name)
        os.makedirs(d, exist_ok=True)
        count = struct.unpack(">i", rom[ptr + 0x20:ptr + 0x24])[0]
        wl = ptr + struct.unpack(">I", rom[ptr + 0x2C:ptr + 0x30])[0]
        used = name == "bank38_used"
        lines.append("\n## %s (ROM 0x%X, waves 0x%X) - %s, %d waves\n\n" % (name, ptr, wbk, what, count))
        if used:
            lines.append("The effects column is the game's sound-effect number (effects bank at ROM 0x701630) that plays "
                         "the wave. Wave 0 is also the race music (the 1-channel song 0x701AA0 holds one endless note on it).\n\n"
                         "| wave | file | length | loop | effects |\n|---|---|---|---|---|\n")
        else:
            lines.append("| wave | file | length | loop |\n|---|---|---|---|\n")
        done = 0
        for i in range(count):
            w = ptr + struct.unpack(">I", rom[wl + i * 4:wl + i * 4 + 4])[0]
            base, ln, typ, fl, loop, bookp = struct.unpack(">IiBBxxII", rom[w:w + 20])
            start = wbk + base
            if ln <= 0 or start + ln > min(end, len(rom)):
                lines.append("| %d | - | missing (past the end of the ROM data) | |\n" % i)
                continue
            data = rom[start:start + ln]
            if typ == 0:
                order, npred = struct.unpack(">ii", rom[ptr + bookp:ptr + bookp + 8])
                book = list(struct.unpack(">%dh" % (order * npred * 8), rom[ptr + bookp + 8:ptr + bookp + 8 + order * npred * 16]))
                pcm = decode_vadpcm(data, book, order, npred)
            else:
                pcm = list(struct.unpack(">%dh" % (ln // 2), data[:ln // 2 * 2]))
            loop_txt = ""
            if loop:
                ls, le, lc = struct.unpack(">IIi", rom[ptr + loop:ptr + loop + 12])
                loop_txt = "%d-%d (%s)" % (ls, le, "forever" if lc == -1 else "x%d" % lc)
            fn = "wave_%03d.wav" % i
            with wave.open(os.path.join(d, fn), "wb") as wf:
                wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(RATE)
                wf.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
            if used:
                lines.append("| %d | %s | %.2f s | %s | %s |\n" % (i, fn, len(pcm) / RATE, loop_txt,
                                                                ", ".join(str(k) for k in fx_of_wave.get(i, [])) or "-"))
            else:
                lines.append("| %d | %s | %.2f s | %s |\n" % (i, fn, len(pcm) / RATE, loop_txt))
            done += 1
        print(name, "waves", done, "/", count)
    open(os.path.join(out_root, "LIST.md"), "w", encoding="utf-8").writelines(lines)


if __name__ == "__main__":
    main()
