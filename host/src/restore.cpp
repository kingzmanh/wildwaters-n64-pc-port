// Restored data: assets/restore/*.patch files (made by tools/rebuild_course2.py) are applied
// to the in-memory copy of the ROM before the game starts. The ROM file is never changed.
//
// course2.patch rebuilds Woodchuck Ridge's data that was lost past the 8 MB flash cut-off of
// the prototype cartridge: water tiles 28-29 and the 30-segment collision pack, written at the
// ROM addresses the game reads them from (func_80010820).
//
// Patch format: repeated { u32 rom_offset, u32 length, bytes } (big-endian).
// WW_RESTORE=off skips it (tests).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "librecomp/game.hpp"

#include "ww.hpp"

namespace {
    uint32_t be32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
}

void ww::apply_restore_patches(const std::filesystem::path& dir) {
    const char* env = std::getenv("WW_RESTORE");
    if (env != nullptr && std::strcmp(env, "off") == 0) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    std::span<const uint8_t> current = recomp::get_rom();
    std::vector<uint8_t> rom(current.begin(), current.end());
    bool changed = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".patch") {
            continue;
        }
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t p = 0, records = 0;
        while (p + 8 <= data.size()) {
            uint32_t off = be32(&data[p]), len = be32(&data[p + 4]);
            p += 8;
            if (p + len > data.size()) {
                break;
            }
            if ((size_t)off + len > rom.size()) {
                rom.resize((size_t)off + len, 0);
            }
            std::memcpy(rom.data() + off, data.data() + p, len);
            p += len;
            records++;
        }
        std::printf("[restore] %s: %zu blocks applied\n", entry.path().filename().string().c_str(), records);
        changed = changed || records > 0;
    }
    if (changed) {
        recomp::set_rom_contents(std::move(rom));
    }
}
