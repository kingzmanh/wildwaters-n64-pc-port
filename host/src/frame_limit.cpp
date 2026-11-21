// Game frame limiter.
//
// WildWaters' kayak physics moves a fixed amount per game frame; it does not scale by the
// elapsed time the game measures (func_80035A80 computes it, clamps it, and falls back to
// 2 fields - 30 fps - after a stall). On the N64 the heavy race scene ran at that rate;
// on PC the renderer keeps up with every field, so the race ran at 60 fps: everything
// twice as fast. Holding each display list until 1/30 s has passed since the previous
// one paces the game at the rate it was made for (the game waits for each graphics task
// to finish before starting the next frame). Runs on the graphics thread, so no game
// thread is blocked.
//
// WW_FPS=<n> overrides the rate for tests (0 = unlimited).

#include <chrono>
#include <cstdlib>
#include <thread>

#include "ww.hpp"

namespace {
    using clock_type = std::chrono::steady_clock;
    clock_type::time_point next{};
    bool started = false;

    double target_fps() {
        static const double fps = [] {
            const char* e = std::getenv("WW_FPS");
            return e ? std::atof(e) : 30.0;
        }();
        return fps;
    }
}

void ww::frame_limit() {
    const double fps = target_fps();
    if (fps <= 0.0) {
        return;
    }
    const auto period = std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(1.0 / fps));
    auto now = clock_type::now();
    if (!started || now - next > period * 4) {
        // First frame, or the game stalled (loading): restart the pacing from now.
        started = true;
        next = now + period;
        return;
    }
    // Sleep most of the way, then spin the last ~1.5 ms for an even frame rhythm.
    if (next - now > std::chrono::microseconds(2000)) {
        std::this_thread::sleep_until(next - std::chrono::microseconds(1500));
    }
    while (clock_type::now() < next) {
        std::this_thread::yield();
    }
    next += period;
}
