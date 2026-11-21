// SDL2 sound output. The game's audio thread hands over each buffer the audio
// microcode mixed (osAiSetNextBuffer) and paces itself by how much is still queued
// (osAiGetLength), so the SDL device runs in queue mode.

#include <cstdio>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "recompui/config.h"

#include "ww.hpp"

namespace {
    constexpr int channels = 2;
    std::mutex audio_mutex;
    SDL_AudioDeviceID device = 0;
    std::vector<float> convert_buffer;
    uint32_t last_buffer_frames = 0;

    void open_device(uint32_t frequency) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
            return;
        }
        if (device != 0) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        SDL_AudioSpec want{};
        want.freq = (int)frequency;
        want.format = AUDIO_F32SYS;
        want.channels = channels;
        want.samples = 256;
        device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (device == 0) {
            std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(device, 0);
    }
}

void ww::audio::set_frequency(uint32_t frequency) {
    std::lock_guard lock{audio_mutex};
    std::printf("[audio] output frequency %u Hz\n", frequency);
    open_device(frequency);
}

void ww::audio::queue_samples(int16_t* samples, size_t sample_count) {
    ww::audio_dump(samples, sample_count);
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return;
    }
    // RDRAM holds native-endian 32-bit words, so each stereo frame's two 16-bit samples
    // read back swapped: [i + 1] is the left channel and [i] the right.
    const float scale = static_cast<float>(recompui::config::sound::get_main_volume()) / (100.0f * 32768.0f);
    convert_buffer.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += channels) {
        convert_buffer[i + 0] = samples[i + 1] * scale;
        convert_buffer[i + 1] = samples[i + 0] * scale;
    }
    last_buffer_frames = (uint32_t)(sample_count / channels);
    SDL_QueueAudio(device, convert_buffer.data(), (Uint32)(sample_count * sizeof(float)));
    // WW_TRACE_AUDIO=1: frames produced per second and what is still queued.
    static const bool trace = SDL_getenv("WW_TRACE_AUDIO") != nullptr;
    static uint64_t window = 0;
    static size_t produced = 0, buffers = 0;
    if (trace) {
        produced += sample_count / channels;
        buffers++;
        uint64_t now = SDL_GetTicks64();
        if (window == 0) window = now;
        if (now - window >= 1000) {
            std::printf("[audio] %zu frames/s in %zu buffers, queued %u frames\n", produced, buffers,
                SDL_GetQueuedAudioSize(device) / (unsigned)(channels * sizeof(float)));
            produced = buffers = 0;
            window = now;
        }
    }
}

size_t ww::audio::get_frames_remaining() {
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return 0;
    }
    // On hardware AI_LEN counts only what is left of the buffer playing now, not the one
    // queued behind it: leave one buffer out of the SDL queue's total.
    uint32_t queued = SDL_GetQueuedAudioSize(device) / (channels * sizeof(float));
    return queued > last_buffer_frames ? queued - last_buffer_frames : 0;
}
