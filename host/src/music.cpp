// Optional unused songs (Sound > Music; the default, Original, leaves the game alone).
//
// The prototype's song assets (ids 0x759E, 0x75A3, 0x75A8 and the per-course song table)
// all point at one small song (ROM 0x701AA0): a single channel holding one endless note
// on wave 0 of the effects bank, i.e. a looping sampled track. That is the original race
// music. The ROM also holds full songs that nothing plays: four 24-channel songs and a 16-channel one
// right after the stub, and 13 Destruction Derby 64 songs left on the dev cartridge past 8 MB.
// They are in an older libmus binary format:
//   +0x00 num_channels, +0x04 num_waves, +0x08 volume_list, +0x0C pbend_list,
//   +0x10 env_table, +0x14 drum_table, +0x18 data_list[num_channels] (offsets from +0)
// The game's libmus (func_8001BE50) wants the later song_t layout (version, num_channels,
// num_waves, 7 offsets, flags...). convert() builds that: a new 0x38-byte header, the old
// song body after it (channel jumps are relative to each channel's start, so moving the
// body is safe), an identity wave table and an empty master track.
//
// Hooks (wildwaters.toml): ww_music_request() records which song asset was asked for,
// ww_music_loaded() runs once func_80031034 has DMAed the stub into the song buffer and
// replaces it with the chosen restored song.
//
// WW_MUSIC=unused|old picks the extra songs (default original); WW_MUSIC_SONG=<n> forces song n for every
// request (tests); WW_TRACE_MUSIC=1 logs requests.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"

#include "ww.hpp"

namespace {
    struct Song {
        uint32_t rom;   // old-format header (num_channels word)
        uint32_t end;
        const char* name;
    };

    // This build's unplayed songs, then Destruction Derby 64's (ROM 8-16 MB).
    const Song songs[] = {
        { 0x701C20, 0x707F7C, "song 1 (this build)" },
        { 0x707F80, 0x70B45C, "song 2 (this build)" },
        { 0x70B460, 0x70E31C, "song 3 (this build)" },
        { 0x70E320, 0x713CC0, "song 4 (this build)" },
        { 0x701B10, 0x701C1C, "short song (this build, 16 channels)" },
        { 0xF7F6A0, 0xF8758C, "Destruction Derby 64 song 1" },
        { 0xF87590, 0xF8D92C, "Destruction Derby 64 song 2" },
        { 0xF8D930, 0xF90E3C, "Destruction Derby 64 song 3" },
        { 0xF90E40, 0xF9680C, "Destruction Derby 64 song 4" },
        { 0xF96810, 0xF9C9BC, "Destruction Derby 64 song 5" },
        { 0xF9C9C0, 0xFA29AC, "Destruction Derby 64 song 6" },
        { 0xFA29B0, 0xFA5F9C, "Destruction Derby 64 song 7" },
        { 0xFA5FA0, 0xFA9F0C, "Destruction Derby 64 song 8" },
        { 0xFA9F10, 0xFADCDC, "Destruction Derby 64 song 9" },
        { 0xFADCE0, 0xFB1FEC, "Destruction Derby 64 song 10" },
        { 0xFB1FF0, 0xFB55BC, "Destruction Derby 64 song 11" },
        { 0xFB55C0, 0xFBB63C, "Destruction Derby 64 song 12" },
        { 0xFBB640, 0xFC2000, "Destruction Derby 64 song 13" },
    };
    constexpr int song_count = sizeof(songs) / sizeof(songs[0]);

    uint32_t requested_id = 0;

    // The songs index instrument waves up to 110: they were written for the 129-wave
    // music bank left in the stale area (ptr bank 0xDCB230, wave data 0xDD1300), not the
    // 38-wave effects bank this build loads into both bank slots. Its header is copied
    // to N64 address 0x80F00000 (unused; below 16 MB so the audio microcode can DMA the
    // ADPCM codebooks) and initialised with the game's MusPtrBankInitialize.
    struct Bank { uint32_t ptr_rom, wave_rom; };
    const Bank bank129 = { 0xDCB230, 0xDD1300 };
    const Bank bank80 = { 0xBF0170, 0xBF3E20 };
    constexpr uint32_t bank_ram = 0x80F00000;
    bool bank_ready = false;
    uint32_t bank_override = 0;   // one-shot, consumed by the next MusPtrBankSetSingle

    uint32_t be32(std::span<const uint8_t> rom, uint32_t off) {
        return ((uint32_t)rom[off] << 24) | ((uint32_t)rom[off + 1] << 16) | ((uint32_t)rom[off + 2] << 8) | rom[off + 3];
    }

    // Builds a song in the game's libmus format from an old-format song in the ROM.
    std::vector<uint8_t> convert(const Song& s) {
        std::span<const uint8_t> rom = recomp::get_rom();
        std::vector<uint8_t> out;
        if (s.end > rom.size()) {
            return out;
        }
        const uint32_t nch = be32(rom, s.rom + 0x00);
        const uint32_t nwaves = be32(rom, s.rom + 0x04);
        const uint32_t vol = be32(rom, s.rom + 0x08);
        const uint32_t pbend = be32(rom, s.rom + 0x0C);
        const uint32_t env = be32(rom, s.rom + 0x10);
        const uint32_t drum = be32(rom, s.rom + 0x14);
        const uint32_t body = s.end - s.rom;
        constexpr uint32_t hdr = 0x38;
        uint32_t wave_off = (hdr + body + 1) & ~1u;
        uint32_t master_off = wave_off + 256 * 2;
        out.assign(master_off + 4, 0);
        auto put32 = [&](uint32_t off, uint32_t v) {
            out[off] = (uint8_t)(v >> 24); out[off + 1] = (uint8_t)(v >> 16);
            out[off + 2] = (uint8_t)(v >> 8); out[off + 3] = (uint8_t)v;
        };
        std::memcpy(out.data() + hdr, rom.data() + s.rom, body);
        put32(0x00, 0x215);              // version, as the game's own stub song
        put32(0x04, nch);
        put32(0x08, nwaves);
        put32(0x0C, hdr + 0x18);         // data_list (inline in the old header)
        put32(0x10, hdr + vol);
        put32(0x14, hdr + pbend);
        put32(0x18, hdr + env);
        put32(0x1C, hdr + drum);
        put32(0x20, wave_off);
        put32(0x24, master_off);
        // 0x28 flags = 0 (not yet initialised), 0x2C-0x37 reserved.
        // The channel lists hold offsets from the old base: rebase the non-zero ones.
        for (uint32_t list : { 0x18u, vol, pbend }) {
            for (uint32_t i = 0; i < nch; i++) {
                uint32_t at = hdr + list + i * 4;
                uint32_t v = be32(std::span<const uint8_t>(out), at);
                if (v != 0) {
                    put32(at, v + hdr);
                }
            }
        }
        // Old songs name the bank's waves directly: identity table.
        for (uint32_t i = 0; i < 256; i++) {
            out[wave_off + i * 2] = (uint8_t)(i >> 8);
            out[wave_off + i * 2 + 1] = (uint8_t)i;
        }
        out[master_off] = 0x80; // Fstop: the master track has nothing to do
        return out;
    }

    // Which restored song plays for a song asset the game asks for. The race asks for
    // 0x75A3 in single player and 0x759E in multiplayer (func_80036A98); the course is
    // the byte at 0x8015912F (0 Devil's Elbow, 1 Woodchuck Ridge).
    // 0 = Original (the prototype's own looping track), 1 = unused songs, 2 = Destruction Derby 64 songs.
    int mode_setting() {
        if (const char* m = std::getenv("WW_MUSIC")) {
            if (std::strcmp(m, "unused") == 0) return 1;
            if (std::strcmp(m, "old") == 0) return 2;
            return 0;
        }
#if defined(WW_RT64)
        return ww::music_mode();
#else
        return 0;
#endif
    }

    int song_for_request(uint8_t* rdram, uint32_t id) {
        if (const char* force = std::getenv("WW_MUSIC_SONG")) {
            return std::atoi(force) - 1;
        }
        if (id != 0x75A3 && id != 0x759E && id != 0x75A8) {
            return -1;
        }
        const int course = MEM_BU(0, (gpr)(int32_t)0x8015912F) & 1;
        const bool multi = id == 0x759E;
        //                      single DE, single WR, multi DE, multi WR
        static const int restored[4] = { 0, 3, 1, 2 };     // this build's songs 1, 4, 2, 3
        static const int older[4] = { 13, 8, 15, 16 };    // DD64 songs 9, 4, 11, 12
        const int slot = (multi ? 2 : 0) + course;
        switch (mode_setting()) {
        case 1: return restored[slot];
        case 2: return older[slot];
        default: return -1;   // Original: leave the game's own song alone
        }
    }
}

extern "C" void ww_music_request(uint8_t*, uint32_t id) {
    requested_id = id;
    if (std::getenv("WW_TRACE_MUSIC")) {
        std::printf("[music] request asset 0x%X at VI %u\n", id, ww::current_vi());
    }
}

extern "C" void func_80019ED4(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Copies the chosen instrument bank header into RDRAM and initialises it (once).
    bool prepare_bank(uint8_t* rdram, recomp_context* ctx) {
        if (bank_ready) {
            return true;
        }
        const char* which = std::getenv("WW_MUSIC_BANK");
        const Bank& b = (which && std::strcmp(which, "80") == 0) ? bank80 : bank129;
        std::span<const uint8_t> rom = recomp::get_rom();
        if (b.wave_rom > rom.size()) {
            return false;
        }
        gpr base = (gpr)(int32_t)bank_ram;
        for (uint32_t i = 0; i < b.wave_rom - b.ptr_rom; i++) {
            MEM_B((int32_t)i, base) = (int8_t)rom[b.ptr_rom + i];
        }
        // Call the game's MusPtrBankInitialize(bank, wave_rom) without disturbing the
        // hooked function's registers.
        recomp_context saved = *ctx;
        ctx->r4 = base;
        ctx->r5 = (gpr)(int32_t)b.wave_rom;
        func_80019ED4(rdram, ctx);
        *ctx = saved;
        bank_ready = true;
        return true;
    }
}

extern "C" uint32_t ww_music_bank(uint32_t bank) {
    if (bank_override != 0) {
        bank = bank_override;
        bank_override = 0;
    }
    return bank;
}

extern "C" void ww_music_loaded(uint8_t* rdram, recomp_context* ctx, uint32_t buffer) {
    int n = song_for_request(rdram, requested_id);
    if (n < 0 || n >= song_count) {
        return;
    }
    std::vector<uint8_t> data = convert(songs[n]);
    if (data.empty() || !prepare_bank(rdram, ctx)) {
        return;
    }
    bank_override = bank_ram;
    gpr base = (gpr)(int32_t)buffer;
    for (size_t i = 0; i < data.size(); i++) {
        MEM_B((int32_t)i, base) = (int8_t)data[i];
    }
    if (std::getenv("WW_TRACE_MUSIC")) {
        std::printf("[music] asset 0x%X -> %s (%zu bytes at 0x%08X)\n", requested_id, songs[n].name, data.size(), buffer);
    }
}
