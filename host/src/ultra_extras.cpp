// libultra functions WildWaters calls that N64ModernRuntime skips without providing.

#include <cstdint>
#include <cstring>

#include "recomp.h"

// osDpGetCounters(u32 counters[4]): RDP clock, command-busy, pipe-busy and TMEM-busy
// counters, read by the game's debug performance display. There is no RDP on PC:
// report an idle RDP. The clock counter must not be 0: the HUD divides the busy
// counters by it (func_80009470), which on x86 is a divide-by-zero trap.
extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr counters = ctx->r4;
    MEM_W(0, counters) = 0x100000; // clock
    MEM_W(4, counters) = 0;        // command busy
    MEM_W(8, counters) = 0;        // pipe busy
    MEM_W(12, counters) = 0;       // TMEM busy
}

#include <cstdio>
#include <cstdlib>

#include "ww.hpp"

// WW_TRACE_DMA=1: log the game's ROM reads (destination, ROM offset, size).
extern "C" void ww_trace_dma(uint32_t dst, uint32_t rom, uint32_t size) {
    static const bool on = std::getenv("WW_TRACE_DMA") != nullptr;
    if (on) {
        std::printf("[dma] rom 0x%07X size 0x%06X -> 0x%08X (VI %u)\n", rom, size, dst, ww::current_vi());
    }
}

// WW_TRACE_POS=1: kayak position about twice a second (from the tile streamer's object).
extern "C" void ww_trace_pos(uint8_t* rdram, uint32_t obj) {
    ww::note_kayak(rdram, obj);   // restored_events.cpp: finish detection
    static const bool on = std::getenv("WW_TRACE_POS") != nullptr;
    static uint32_t last_vi = 0;
    if (!on) {
        return;
    }
    uint32_t vi = ww::current_vi();
    if (vi - last_vi < 30) {
        return;
    }
    last_vi = vi;
    gpr base = (gpr)(int32_t)obj;
    uint32_t xb = (uint32_t)MEM_W(0x24, base), zb = (uint32_t)MEM_W(0x2C, base);
    int32_t t = MEM_W(0x178, base);
    float x, z;
    std::memcpy(&x, &xb, 4);
    std::memcpy(&z, &zb, 4);
    // Game clocks (ns, 64-bit): 0x8010B2E8 and the frame delta at 0x8010B2A0, VIs per frame at 0x8010B2AC.
    uint64_t clk = ((uint64_t)(uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B2E8) << 32) | (uint32_t)MEM_W(4, (gpr)(int32_t)0x8010B2E8);
    uint64_t dlt = ((uint64_t)(uint32_t)MEM_W(4, (gpr)(int32_t)0x8010B2A0) << 32) | (uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B2A0);
    int32_t vis = MEM_W(0, (gpr)(int32_t)0x8010B2AC);
    std::printf("[pos] VI %u x %.1f z %.1f tile %d clock %.3f s delta %.4f s vis %d\n", vi, x, z, t,
        clk / 1e9, dlt / 1e9, vis);
}

// WW_SELFTEST=1: checks the game's hand-written 64-bit math helpers (recompiled) against
// native results, at startup.
extern "C" void func_80014240(uint8_t* rdram, recomp_context* ctx);  // signed 64-bit divide
extern "C" void func_8001422C(uint8_t* rdram, recomp_context* ctx);  // signed 64-bit remainder
extern "C" void func_800141F0(uint8_t* rdram, recomp_context* ctx);  // (other divide entry)
extern "C" void func_800144B8(uint8_t* rdram, recomp_context* ctx);  // s32 -> float

void ww_selftest(uint8_t* rdram, recomp_context* base) {
    if (std::getenv("WW_SELFTEST") == nullptr) {
        return;
    }
    struct Case { int64_t a, b; };
    const Case cases[] = { { 125000000000LL, 1000000000LL }, { 125678901234LL, 1000000LL }, { -7000000000LL, 3 },
                           { 59000000000LL, 60 }, { 3725, 3600 }, { 100, 7 } };
    auto call2 = [&](void (*f)(uint8_t*, recomp_context*), int64_t a, int64_t b) {
        recomp_context c = *base;
        c.r29 = (gpr)(int32_t)0x80700000; // scratch stack (unused this early)
        c.r4 = (gpr)(int32_t)(uint32_t)((uint64_t)a >> 32); c.r5 = (gpr)(int32_t)(uint32_t)a;
        c.r6 = (gpr)(int32_t)(uint32_t)((uint64_t)b >> 32); c.r7 = (gpr)(int32_t)(uint32_t)b;
        f(rdram, &c);
        return (int64_t)(((uint64_t)(uint32_t)c.r2 << 32) | (uint32_t)c.r3);
    };
    for (const Case& k : cases) {
        int64_t q = call2(func_80014240, k.a, k.b), r = call2(func_8001422C, k.a, k.b), u = call2(func_800141F0, k.a, k.b);
        std::printf("[selftest] %lld / %lld: div %lld (want %lld)  rem %lld (want %lld)  800141F0 -> %lld\n",
            (long long)k.a, (long long)k.b, (long long)q, (long long)(k.a / k.b), (long long)r, (long long)(k.a % k.b), (long long)u);
    }
    recomp_context c = *base;
    c.r4 = 12345;
    func_800144B8(rdram, &c);
    std::printf("[selftest] s32->float 12345 -> %f\n", c.f0.fl);
}
