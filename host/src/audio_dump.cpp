// WW_AUDIO_DUMP=<file.wav>: writes everything the game's audio thread outputs to a
// 16-bit stereo WAV, so music can be checked in silent test runs.

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "ww.hpp"

namespace {
    std::mutex dump_mutex;
    FILE* file = nullptr;
    bool tried = false;
    uint32_t rate = 22050;
    uint32_t bytes = 0;

    void write_header() {
        auto u32 = [](uint32_t v) { std::fwrite(&v, 4, 1, file); };
        auto u16 = [](uint16_t v) { std::fwrite(&v, 2, 1, file); };
        std::fseek(file, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, file); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, file);
        u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
        std::fwrite("data", 1, 4, file); u32(bytes);
        std::fseek(file, 0, SEEK_END);
    }

    bool open() {
        if (!tried) {
            tried = true;
            if (const char* path = std::getenv("WW_AUDIO_DUMP")) {
                file = std::fopen(path, "wb");
                if (file) {
                    write_header();
                }
            }
        }
        return file != nullptr;
    }
}

void ww::audio_dump_frequency(uint32_t frequency) {
    std::lock_guard lock{dump_mutex};
    rate = frequency;
    if (open()) {
        write_header();
    }
}

void ww::audio_dump(const int16_t* samples, size_t sample_count) {
    std::lock_guard lock{dump_mutex};
    if (!open()) {
        return;
    }
    // RDRAM holds native-endian 32-bit words: each frame's two samples read back swapped.
    std::vector<int16_t> out(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        out[i] = samples[i + 1];
        out[i + 1] = samples[i];
    }
    std::fwrite(out.data(), 2, out.size(), file);
    bytes += (uint32_t)(out.size() * 2);
    write_header();
    std::fflush(file);
}
