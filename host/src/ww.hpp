// WildWaters PC: shared declarations for the host program.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "librecomp/game.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"

namespace ww {
    inline constexpr const char* program_name = "WildWaters PC";
    inline constexpr const char8_t* program_id = u8"WildWatersPC";
    inline constexpr const char8_t* game_id = u8"wildwaters.n64.proto.19990615";
    // XXH3-64 of the Jun 15 1999 prototype (Team CARROT dump, big-endian .z64,
    // MD5 b867880c96c9e8d3130dde5526c95439).
    inline constexpr uint64_t rom_hash = 0x3EF9EA187C5BBEDEULL;

    // null_renderer.cpp: draws nothing (headless test runs).
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // asset_capture.cpp: WW_ASSET_DIR=<folder> records textures and textured geometry (headless).
    bool asset_capture_enabled();
    void asset_capture_init(uint8_t* rdram);
    void asset_capture_dl(uint32_t data_ptr);
    void asset_capture_finish();

    const char* rom_error_text(recomp::RomValidationError error);

    // restore.cpp: applies assets/restore/*.patch to the in-memory ROM (Woodchuck Ridge).
    void apply_restore_patches(const std::filesystem::path& dir);

    // frame_limit.cpp: paces game frames to 30 fps (the race physics is per frame).
    void frame_limit();

    // overlays.cpp: the recompiled code section, for function lookups.
    void register_overlays();

    // input_script.cpp: WW_INPUT_SCRIPT test input, ORed into port 1. True if a script is active.
    bool script_input(int port, uint32_t vi, uint16_t* buttons, float* x, float* y);
    // VI of player 1's last new button press / stick movement (attract demo idle timer).
    uint32_t last_activity_vi();
    // The test input script drives this controller (it is then reported as plugged in).
    bool script_uses_port(int port);
    // restored_events.cpp: attract demo + "You survived!" at the end of the course.
    void note_kayak(uint8_t* rdram, uint32_t obj);
    // Menu presses the attract demo injects into player 1 (not counted as activity).
    bool attract_input(uint32_t vi, uint16_t* buttons, float* y);
    // main.cpp: VIs since boot.
    uint32_t current_vi();

    // audio_dump.cpp: WW_AUDIO_DUMP=<file.wav> records the game's audio output.
    void audio_dump(const int16_t* samples, size_t sample_count);
    void audio_dump_frequency(uint32_t frequency);

#if defined(WW_RT64)
    namespace frontend {
        inline int window_width = 0;
        inline int window_height = 0;
        void init(recomp::GameEntry& game);
        void set_callbacks(recomp::Configuration& cfg);
        ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
    }
    namespace audio {
        void queue_samples(int16_t* samples, size_t sample_count);
        size_t get_frames_remaining();
        void set_frequency(uint32_t frequency);
    }
    void init_config();
    // profiler.cpp: WW_PROFILE_GFX=1 samples the renderer thread inside send_dl.
    void profiler_enter();
    void profiler_leave();
    // config.cpp: Sound > Music (0 Original, 1 Unused Songs, 2 Destruction Derby 64 Songs).
    int music_mode();
    // config.cpp: General > Two-player mode.
    bool two_player_mode();
#endif
}
