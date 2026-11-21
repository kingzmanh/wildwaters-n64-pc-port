"""Walk the channels of old-format libmus songs in the ROM and report how cleanly they
parse with and without per-note velocity bytes, plus the wave numbers they use.

  python tools/musparse.py baserom.z64
"""
import struct
import sys

# libmus 3.14 command argument sizes (player_commands.c). 'v' = 1 or 2 bytes (bit 7 = long).
ARGS = {0x80: 0, 0x81: "v", 0x82: 1, 0x83: 0, 0x84: 7, 0x85: 1, 0x86: 2, 0x87: 1, 0x88: 3, 0x89: 3,
        0x8a: 0, 0x8b: "v", 0x8c: 0, 0x8d: 1, 0x8e: 0, 0x8f: 1, 0x90: "v", 0x91: 0, 0x92: 0, 0x93: 0,
        0x94: 0, 0x95: 1, 0x96: 0, 0x97: 3, 0x98: 0, 0x99: 0, 0x9a: 0, 0x9b: 1, 0x9c: 1, 0x9d: 2,
        0x9e: "v", 0x9f: 0, 0xa0: 1, 0xa1: 6, 0xa2: 1, 0xa3: 2, 0xa4: 2, 0xa5: 2, 0xa6: 1, 0xa7: "v",
        0xa8: 1, 0xa9: 1, 0xaa: 1, 0xab: "m", 0xac: 0}

SONGS = [0x701B10, 0x701C20, 0x707F80, 0x70B460, 0x70E320, 0xF7F6A0, 0xF87590, 0xF8D930, 0xF90E40,
         0xF96810, 0xF9C9C0, 0xFA29B0, 0xFA5FA0, 0xFA9F10, 0xFADCE0, 0xFB1FF0, 0xFB55C0, 0xFBB640]


def walk(d, p, end, velocity):
    waves = set()
    notes = 0
    fixed = False
    vel_on = velocity
    while p < end:
        c = d[p]
        if c >= 0x80:
            if c not in ARGS:
                return "bad cmd %02X" % c, notes, waves
            p += 1
            a = ARGS[c]
            if c == 0x80:
                return "stop", notes, waves
            if c == 0x81:
                w = d[p]; p += 1
                if w & 0x80:
                    w = ((w & 0x7F) << 8) | d[p]; p += 1
                waves.add(w)
                continue
            if c == 0x8b:
                fixed = True
            if c == 0xac:
                fixed = False
            if c == 0x99:
                vel_on = True
            if c == 0xa1:
                return "goto", notes, waves
            if a == "v":
                p += 2 if d[p] & 0x80 else 1
            elif a == "m":
                p += 1
                p += 2 if d[p] & 0x80 else 1
            else:
                p += a
            continue
        # note
        p += 1
        notes += 1
        if vel_on:
            v = d[p]; p += 1
            if v >= 0x80:
                vel_on = False
        if not fixed:
            p += 2 if d[p] & 0x80 else 1
    return "ran off", notes, waves


def main():
    d = open(sys.argv[1], "rb").read()
    for i, s in enumerate(SONGS):
        nch, nw, vol, pb, env, drum = struct.unpack(">6I", d[s:s + 24])
        end = SONGS[i + 1] if i + 1 < len(SONGS) else 0xFC2000
        chans = struct.unpack(">%dI" % nch, d[s + 0x18:s + 0x18 + 4 * nch])
        res = {}
        for vel in (True, False):
            ok = 0; allw = set(); used = 0
            for c in chans:
                if c == 0:
                    continue
                used += 1
                r, n, w = walk(d, s + c, end, vel)
                if r in ("stop", "goto") and n > 0:
                    ok += 1
                allw |= w
            res[vel] = (ok, used, sorted(allw))
        print("%07X ch %2d waves %3d | with velocity: %d/%d clean, waves %s | without: %d/%d clean, waves %s" % (
            s, nch, nw, res[True][0], res[True][1], res[True][2][:12], res[False][0], res[False][1], res[False][2][:12]))


main()
