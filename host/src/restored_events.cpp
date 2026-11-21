// Two things the prototype has the data for but never shows, restored:
//
// 1. Attract demo ("PRESS START TO BEGIN"). The race screen layout (asset 0x11498) and its code have
//    a second race mode: race mode byte 0x8015912C = 1 (from 0x8010B318 when the race starts,
//    func_80036628) shows the WildWaters logo + "PRESS START TO BEGIN" (func_8003EB94, the text blinks
//    in func_8003ECA4, the big logo swaps for the small one after 3 s in func_8003EBF0) over a race
//    that runs without the player's input. Nothing in the prototype ever sets mode 1, it has no way out,
//    and it skips the kayak update (the camera tumbles). The kayak also needs the set-up the menus do
//    (kayaker, players). So: after WW_ATTRACT_IDLE seconds (default 30) idle on the first menu page,
//    the port presses through the menus itself (A x5, plus stick-down on the course screen for
//    every other demo). That race runs normally (the kayak drifts down the river on the current, the
//    camera follows); the game's screen loader adds the title layout's elements and the game's own
//    routines show logo + "PRESS START" and do the logo swap; Welcome/GO stay hidden. Any real button or 60 s returns to the menus the way the game's own race exit
//    does (func_8003CAAC: clear elements, menu page 2, state 1). Injected presses are not activity.
//
// 2. "YOU SURVIVED!" (element 0x15 of the race screen 0x11499, picture 0x1133C). Nothing shows it:
//    the prototype has no finish line. Here: once the kayak is 70% of the way through the course's
//    last water tile (measured from the side where the previous tile joins), Welcome/GO are hidden,
//    "YOU SURVIVED!" is shown, and 6 s later the race returns to the menus.
//
// Hooks (wildwaters.toml): ww_game_tick at the start of func_80036628 (the per-frame game state
// update; its object is 0x8010B2C8, so the state-change flag 0x8010B324 is its +0x5C), and
// ww::note_kayak from the water-tile streamer hook (func_8000BC84(kayak object)).
// WW_ATTRACT=off / WW_SURVIVED=off disable them; WW_TRACE_EVENTS=1 logs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"

#include "ww.hpp"

extern "C" void func_800348A4(uint8_t* rdram, recomp_context* ctx);  // show screen element (screen, kind, id)
extern "C" void func_8003493C(uint8_t* rdram, recomp_context* ctx);  // hide screen element (screen, id)
extern "C" void func_80034420(uint8_t* rdram, recomp_context* ctx);  // clear the screen's elements
extern "C" void func_8003579C(uint8_t* rdram, recomp_context* ctx);  // set menu page
extern "C" void func_8003684C(uint8_t* rdram, recomp_context* ctx);  // request game state (0 logos, 1 menus, 2 race)
extern "C" void func_8003EBF0(uint8_t* rdram, recomp_context* ctx);  // attract: swap the big logo for the small one after 3 s
extern "C" void func_8003EB94(uint8_t* rdram, recomp_context* ctx);  // attract: show logo + "PRESS START", start its timer
extern "C" void func_8003456C(uint8_t* rdram, recomp_context* ctx);  // add a screen layout asset's records (screen, asset id)
extern "C" void func_80035A74(uint8_t* rdram, recomp_context* ctx);  // clock: reset
extern "C" void func_80035A6C(uint8_t* rdram, recomp_context* ctx);  // clock: start

namespace {
    constexpr uint32_t STATE = 0x80159120;       // current game state
    constexpr uint32_t MODE = 0x8015912C;        // current race mode (0 race, 1 attract demo)
    constexpr uint32_t COURSE = 0x8015912F;      // 0 Devil's Elbow, 1 Woodchuck Ridge
    constexpr uint32_t NEXT_MODE = 0x8010B318;   // race mode for the next race start (u16)
    constexpr uint32_t SCREEN = 0x8010B320;      // current screen (elements)
    constexpr uint32_t NEXT_STATE = 0x8010B314;  // requested state (func_8003684C)
    constexpr uint32_t STATE_FLAG = 0x8010B324;  // state change requested
    constexpr uint32_t WATER_BLOCK[2] = { 0x713CC0, 0x7B7F30 };  // ROM: u32 count, then 0x28D0-byte tiles

    bool trace() { static const bool on = std::getenv("WW_TRACE_EVENTS") != nullptr; return on; }
    bool enabled(const char* var) { const char* v = std::getenv(var); return !(v && std::strcmp(v, "off") == 0); }

    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_BU(0, (gpr)(int32_t)a); }
    uint32_t rd32(uint8_t* rdram, uint32_t a) { return (uint32_t)MEM_W(0, (gpr)(int32_t)a); }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, (gpr)(int32_t)a) = (int8_t)v; }
    void wr16(uint8_t* rdram, uint32_t a, uint16_t v) { MEM_H(0, (gpr)(int32_t)a) = (int16_t)v; }

    void call(void (*fn)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)a0;
        ctx->r5 = (gpr)(int32_t)a1;
        ctx->r6 = (gpr)(int32_t)a2;
        fn(rdram, ctx);
        *ctx = saved;
    }

    // attract demo
    struct Press { uint32_t from, to; uint16_t buttons; float y; };
    std::vector<Press> presses;           // menu presses the demo injects
    std::mutex press_mutex;
    bool demo_pending = false;            // pressing through the menus
    uint32_t first_page = 0xFFFFFFFF;     // menu page at the first menu visit
    bool demo_running = false;
    bool demo_setup = false;              // title elements still to be added
    uint32_t demo_start_vi = 0;
    uint32_t menu_since_vi = 0;
    uint8_t last_state = 0xFF;

    // finish
    uint32_t kayak_obj = 0;
    bool survived = false;
    uint32_t survived_vi = 0;
    bool returned = false;

    float rom_f32(std::span<const uint8_t> rom, uint32_t o) {
        uint32_t v = ((uint32_t)rom[o] << 24) | ((uint32_t)rom[o + 1] << 16) | ((uint32_t)rom[o + 2] << 8) | rom[o + 3];
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    int32_t rom_s32(std::span<const uint8_t> rom, uint32_t o) {
        return (int32_t)(((uint32_t)rom[o] << 24) | ((uint32_t)rom[o + 1] << 16) | ((uint32_t)rom[o + 2] << 8) | rom[o + 3]);
    }

    // How far (0..1) the kayak is through the course's last tile, measured from where the previous
    // tile joins it; -1 when not in the last tile.
    float progress_in_last_tile(uint8_t* rdram) {
        if (kayak_obj == 0) {
            return -1;
        }
        std::span<const uint8_t> rom = recomp::get_rom();
        uint32_t block = WATER_BLOCK[rd8(rdram, COURSE) & 1];
        if (block + 4 > rom.size()) {
            return -1;
        }
        int32_t count = rom_s32(rom, block);
        // The tile the kayak is over, from its position and the tiles' bounds (the object's own tile
        // field at +0x178 only follows the streamer, every other tile).
        uint32_t pxb = rd32(rdram, kayak_obj + 0x24), pzb = rd32(rdram, kayak_obj + 0x2C);
        float px, pz;
        std::memcpy(&px, &pxb, 4);
        std::memcpy(&pz, &pzb, 4);
        int32_t tile = -1;
        for (int32_t k = 0; k < count && block + 4 + (uint32_t)(k + 1) * 0x28D0 <= rom.size(); k++) {
            uint32_t tk = block + 4 + (uint32_t)k * 0x28D0;
            if (px >= rom_f32(rom, tk + 0x18) && px < rom_f32(rom, tk + 0x1C) && pz >= rom_f32(rom, tk + 0x20) && pz < rom_f32(rom, tk + 0x24)) {
                tile = k;
                break;
            }
        }
        int32_t last = count - 1;
        if (const char* t = std::getenv("WW_FINISH_TILE")) last = std::atoi(t);   // test: an earlier "last" tile
        if (count < 2 || last < 1 || last >= count || tile != last) {
            return -1;
        }
        uint32_t t = block + 4 + (uint32_t)last * 0x28D0;
        float xmin = rom_f32(rom, t + 0x18), xmax = rom_f32(rom, t + 0x1C), zmin = rom_f32(rom, t + 0x20), zmax = rom_f32(rom, t + 0x24);
        uint32_t xb = rd32(rdram, kayak_obj + 0x24), zb = rd32(rdram, kayak_obj + 0x2C);
        float x, z;
        std::memcpy(&x, &xb, 4);
        std::memcpy(&z, &zb, 4);
        for (int side = 0; side < 4; side++) {           // link[4]: -x, +x, -z, +z
            if (rom_s32(rom, t + side * 4) != last - 1) {
                continue;
            }
            switch (side) {
            case 0: return (x - xmin) / (xmax - xmin);   // entered from -x: heading +x
            case 1: return (xmax - x) / (xmax - xmin);
            case 2: return (z - zmin) / (zmax - zmin);
            default: return (zmax - z) / (zmax - zmin);
            }
        }
        return -1;
    }

    // Is element `id` on the screen's visible list (screen+0x89C4: nodes {element*, next})? The game's
    // hide routine func_8003493C assumes it is.
    bool visible(uint8_t* rdram, uint32_t screen, uint32_t id) {
        uint32_t node = rd32(rdram, screen + 0x89C4);
        for (int guard = 0; node && guard < 256; guard++) {
            uint32_t el = rd32(rdram, node);
            if (el && rd32(rdram, el) == id) return true;
            node = rd32(rdram, node + 4);
        }
        return false;
    }

    void hide(uint8_t* rdram, recomp_context* ctx, uint32_t screen, uint32_t id) {
        if (screen && visible(rdram, screen, id)) call(func_8003493C, rdram, ctx, screen, id);
    }

    void back_to_menus(uint8_t* rdram, recomp_context* ctx) {
        uint32_t screen = rd32(rdram, SCREEN);
        call(func_80034420, rdram, ctx, screen);
        wr16(rdram, NEXT_MODE, 0);
        call(func_8003579C, rdram, ctx, 2);
        call(func_8003684C, rdram, ctx, 1);
    }
}

bool ww::attract_input(uint32_t vi, uint16_t* buttons, float* y) {
    std::lock_guard lock(press_mutex);
    for (const Press& p : presses) {
        if (vi >= p.from && vi < p.to) {
            *buttons |= p.buttons;
            if (p.y != 0) *y = p.y;
        }
    }
    return !presses.empty();
}

void ww::note_kayak(uint8_t*, uint32_t obj) {
    if (kayak_obj == 0 || obj == kayak_obj) {
        kayak_obj = obj;       // the first kayak the streamer follows: player 1
    }
}

extern "C" void ww_game_tick(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t vi = ww::current_vi();
    const uint8_t state = rd8(rdram, STATE);
    const uint8_t mode = rd8(rdram, MODE);
    const uint32_t page = rd32(rdram, 0x8010B310) ? rd32(rdram, rd32(rdram, 0x8010B310) + 0xE60) : 0;
    if (state != last_state) {
        if (trace()) std::printf("[events] VI %u state %u mode %u page %u players %u racers %u course %u\n", vi, state, mode, page,
            rd32(rdram, 0x80159124), rd32(rdram, 0x80159128), rd8(rdram, COURSE));
        if (state == 1) {
            menu_since_vi = vi;
            if (first_page == 0xFFFFFFFF) first_page = page;
        }
        if (state == 2) { survived = false; returned = false; kayak_obj = 0; }
        if (state != 2) demo_running = false;
        last_state = state;
    }

    // 1. attract demo
    static const uint32_t idle_vis = (uint32_t)(std::atof(std::getenv("WW_ATTRACT_IDLE") ? std::getenv("WW_ATTRACT_IDLE") : "30") * 60);
    if (enabled("WW_ATTRACT")) {
        // The demo's race is about to start (a normal race: the race screen and everything the race
        // code needs). The title elements are added once it runs.
        if (demo_pending && rd8(rdram, STATE_FLAG) != 0 && (uint16_t)MEM_H(0, (gpr)(int32_t)NEXT_STATE) == 2) {
            demo_setup = true;
            {
                std::lock_guard lock(press_mutex);
                presses.clear();
            }
            demo_pending = false;
            demo_running = true;
            demo_start_vi = vi;
            if (trace()) std::printf("[events] VI %u attract demo starts\n", vi);
            return;
        }
        if (rd8(rdram, STATE_FLAG) != 0) {
            return;
        }
        if (demo_pending) {
            // a real press, or the menus went somewhere unexpected: give up the demo
            if (ww::last_activity_vi() > presses.front().from || state != 1 || vi > presses.back().to + 300) {
                if (trace()) std::printf("[events] VI %u attract demo cancelled\n", vi);
                std::lock_guard lock(press_mutex);
                presses.clear();
                demo_pending = false;
            }
            return;
        }
        uint32_t since = (std::max)(menu_since_vi, ww::last_activity_vi());
        if (state == 1 && page == first_page && vi - since > idle_vis) {
            // Press through the menus like a player: A on every screen; on the course screen (the
            // second) stick down first for Woodchuck Ridge, on every other demo.
            static int demos = 0;
            bool second = (demos++ & 1) != 0;
            std::lock_guard lock(press_mutex);
            presses.clear();
            uint32_t t = vi + 30;
            const uint16_t A = 0x8000;
            presses.push_back({ t, t + 6, A, 0 });
            if (second) {
                presses.push_back({ t + 200, t + 206, 0, -1.0f });
                for (uint32_t k = 1; k <= 4; k++) presses.push_back({ t + 180 + 180 * k, t + 186 + 180 * k, A, 0 });
            }
            else {
                for (uint32_t k = 1; k <= 4; k++) presses.push_back({ t + 180 * k, t + 6 + 180 * k, A, 0 });
            }
            demo_pending = true;
            if (trace()) std::printf("[events] VI %u idle: pressing through the menus for a demo (%s)\n", vi, second ? "Woodchuck Ridge" : "Devil's Elbow");
            return;
        }
        if (state == 2 && demo_running &&
            (ww::last_activity_vi() > demo_start_vi + 30 || vi - demo_start_vi > 60 * 60)) {
            if (trace()) std::printf("[events] VI %u attract demo ends\n", vi);
            back_to_menus(rdram, ctx);
            demo_running = false;
            return;
        }
        // Mode 1 would skip the kayak update and its title layout lacks the race screen's elements the
        // race code uses, so the demo is a normal race: the game's screen loader (func_8003456C, which
        // adds a layout's records to the current screen) adds the title layout 0x11498, and the game's
        // own routines show logo + "PRESS START" (func_8003EB94) and swap the logo (func_8003EBF0).
        // Welcome/GO stay hidden. With no input the kayak drifts down the river, the camera following.
        if (state == 2 && demo_running) {
            uint32_t screen = rd32(rdram, SCREEN);
            if (demo_setup && screen && vi - demo_start_vi > 20) {
                // Skip the Welcome -> GO sequence (func_8003EE54, state 0x80159F44): mark it done and
                // do its one lasting job, starting the race clocks; hide the Welcome banner.
                uint32_t b310 = rd32(rdram, 0x8010B310);
                MEM_W(0, (gpr)(int32_t)0x80159F44) = 2;
                call(func_80035A74, rdram, ctx, b310 + 0xE08);
                call(func_80035A6C, rdram, ctx, b310 + 0xE30);
                hide(rdram, ctx, screen, 0x11);
                hide(rdram, ctx, screen, 0x12);
                call(func_8003456C, rdram, ctx, screen, 0x11498);
                call(func_8003EB94, rdram, ctx, 0);
                demo_setup = false;
            }
            if (!demo_setup) call(func_8003EBF0, rdram, ctx, 0);
        }
    }
    if (rd8(rdram, STATE_FLAG) != 0) {
        return;
    }

    // 2. "YOU SURVIVED!" at the end of the course
    if (enabled("WW_SURVIVED") && state == 2 && mode == 0 && !demo_running) {
        uint32_t screen = rd32(rdram, SCREEN);
        if (!survived) {
            float p = progress_in_last_tile(rdram);
            static const float finish = std::getenv("WW_FINISH_AT") ? (float)std::atof(std::getenv("WW_FINISH_AT")) : 0.7f;
            if (p >= finish) {
                hide(rdram, ctx, screen, 0x11);   // WELCOME
                hide(rdram, ctx, screen, 0x12);   // GO
                call(func_800348A4, rdram, ctx, screen, 2, 0x15); // show YOU SURVIVED!
                survived = true;
                survived_vi = vi;
                if (trace()) std::printf("[events] VI %u YOU SURVIVED (progress %.2f)\n", vi, p);
            }
        }
        else if (!returned && vi - survived_vi > 6 * 60) {
            if (trace()) std::printf("[events] VI %u back to the menus after the finish\n", vi);
            back_to_menus(rdram, ctx);
            returned = true;
        }
    }
}
