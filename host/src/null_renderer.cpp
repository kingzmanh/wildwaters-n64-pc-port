// A renderer that draws nothing, for headless test runs: it only counts and logs the
// graphics tasks the game sends.
//
// WW_CAPTURE_OBJ=<file.obj> also walks every F3DEX2 display list (segments, modelview
// matrix stack, vertex loads, triangles, texture binds) and writes the unique world-space
// triangles it saw to an OBJ file at shutdown, grouped by texture address. Used to recover
// course geometry (tools/rebuild_course2.py).

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "ww.hpp"

namespace {
    std::atomic<uint32_t> display_lists{0};
    std::atomic<uint32_t> screen_updates{0};

    struct Mat { float m[4][4]; };
    struct V3 { float x, y, z; };

    class Capture {
    public:
        uint8_t* rdram = nullptr;
        std::string path;
        std::mutex mutex;
        std::vector<V3> verts;
        std::map<std::tuple<int, int, int>, int> vindex;
        std::map<uint64_t, std::set<std::tuple<int, int, int>>> tris;   // (texture, vertex buffer) -> tris

        uint32_t r32(uint32_t a) { return *(uint32_t*)(rdram + (a & 0xFFFFFC)); }
        int16_t r16(uint32_t a) { return *(int16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
        uint16_t ru16(uint32_t a) { return *(uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }

        int vid(const V3& v) {
            auto key = std::make_tuple((int)std::lround(v.x * 10), (int)std::lround(v.y * 10), (int)std::lround(v.z * 10));
            auto it = vindex.find(key);
            if (it != vindex.end()) return it->second;
            int id = (int)verts.size();
            verts.push_back(v);
            vindex[key] = id;
            return id;
        }

        Mat load_mtx(uint32_t a) {
            Mat r;
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    int k = i * 4 + j;
                    int32_t hi = r16(a + k * 2);
                    uint32_t lo = ru16(a + 32 + k * 2);
                    r.m[i][j] = (float)(((int64_t)hi << 16) | lo) / 65536.0f;
                }
            }
            return r;
        }

        static Mat mul(const Mat& a, const Mat& b) {
            Mat r{};
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
            return r;
        }

        void walk(uint32_t dl) {
            uint32_t seg[16] = {};
            std::vector<Mat> stack(1);
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) stack[0].m[i][j] = i == j;
            V3 vbuf[64] = {};
            uint32_t vsrc[64] = {};
            uint32_t timg = 0;
            std::vector<uint32_t> calls;
            auto addr = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
            uint32_t pc = dl & 0xFFFFFF;
            for (int guard = 0; guard < 400000; guard++) {
                uint32_t w0 = r32(pc), w1 = r32(pc + 4);
                pc += 8;
                uint8_t op = w0 >> 24;
                switch (op) {
                case 0xDB: // G_MOVEWORD
                    if (((w0 >> 16) & 0xFF) == 0x06) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                    break;
                case 0xDA: { // G_MTX
                    uint32_t p = (w0 & 0xFF) ^ 1;
                    if (p & 4) break; // projection
                    Mat m = load_mtx(addr(w1));
                    Mat top = stack.back();
                    Mat res = (p & 2) ? m : mul(m, top);
                    if (p & 1) stack.push_back(res); else stack.back() = res;
                    break;
                }
                case 0xD8: { // G_POPMTX
                    uint32_t n = w1 / 64;
                    while (n-- && stack.size() > 1) stack.pop_back();
                    break;
                }
                case 0x01: { // G_VTX
                    uint32_t n = (w0 >> 12) & 0xFF;
                    uint32_t v0 = ((w0 >> 1) & 0x7F) - n;
                    uint32_t a = addr(w1);
                    const Mat& m = stack.back();
                    for (uint32_t i = 0; i < n && v0 + i < 64; i++) {
                        float x = r16(a + i * 16), y = r16(a + i * 16 + 2), z = r16(a + i * 16 + 4);
                        vsrc[v0 + i] = a;
                        vbuf[v0 + i] = { x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0],
                                         x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1],
                                         x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2] };
                    }
                    break;
                }
                case 0x05: tri(vbuf, vsrc, timg, (w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF); break;
                case 0x06: case 0x07:
                    tri(vbuf, vsrc, timg, (w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF);
                    tri(vbuf, vsrc, timg, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
                    break;
                case 0xFD: timg = addr(w1); break;
                case 0xDE: // G_DL
                    if (((w0 >> 16) & 0xFF) == 0) calls.push_back(pc);
                    pc = addr(w1);
                    break;
                case 0xDF: // G_ENDDL
                    if (calls.empty()) return;
                    pc = calls.back(); calls.pop_back();
                    break;
                case 0xDD: return; // G_LOAD_UCODE (S2DEX 2D drawing follows)
                default: break;
                }
            }
        }

        void tri(const V3* vb, const uint32_t* vs, uint32_t timg, uint32_t a, uint32_t b, uint32_t c) {
            a /= 2; b /= 2; c /= 2;
            if (a >= 64 || b >= 64 || c >= 64) return;
            int ia = vid(vb[a]), ib = vid(vb[b]), ic = vid(vb[c]);
            if (ia == ib || ib == ic || ia == ic) return;
            tris[((uint64_t)timg << 32) | vs[a]].insert(std::make_tuple(ia, ib, ic));
        }

        // WW_DL_STATS=<vi>: once, for the first display list after that VI, count every command
        // by opcode (following G_DL calls), plus render-target and texture-image addresses.
        void stats(uint32_t dl) {
            std::map<int, int> ops;
            std::map<uint32_t, int> cimg, timg;
            uint32_t seg[16] = {};
            std::vector<uint32_t> calls;
            auto addr = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
            uint32_t pc = dl & 0xFFFFFF;
            int n = 0;
            for (; n < 2000000; n++) {
                uint32_t w0 = r32(pc), w1 = r32(pc + 4);
                pc += 8;
                uint8_t op = w0 >> 24;
                ops[op]++;
                if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                else if (op == 0xFF) cimg[addr(w1)]++;
                else if (op == 0xFD) timg[addr(w1)]++;
                else if (op == 0xDE) { if (((w0 >> 16) & 0xFF) == 0) calls.push_back(pc); pc = addr(w1); }
                else if (op == 0xDF) { if (calls.empty()) break; pc = calls.back(); calls.pop_back(); }
            }
            std::printf("[dlstats] %d commands\n", n);
            for (auto& [op, c] : ops) std::printf("[dlstats]   op %02X x %d\n", op, c);
            std::printf("[dlstats] %zu render targets:", cimg.size());
            for (auto& [a, c] : cimg) std::printf(" %06X(x%d)", a, c);
            std::printf("\n[dlstats] %zu distinct texture images\n", timg.size());
        }

        void dl(uint32_t data_ptr) {
            static const char* st = std::getenv("WW_DL_STATS");
            static bool done = false;
            if (st && !done && rdram && ww::current_vi() >= (uint32_t)std::atoi(st)) {
                done = true;
                stats(data_ptr);
            }
            if (path.empty() || rdram == nullptr) return;
            std::lock_guard lock(mutex);
            walk(data_ptr);
        }

        void write() {
            if (path.empty()) return;
            std::lock_guard lock(mutex);
            FILE* f = std::fopen(path.c_str(), "w");
            if (!f) return;
            for (const V3& v : verts) std::fprintf(f, "v %.3f %.3f %.3f\n", v.x, v.y, v.z);
            size_t n = 0;
            for (auto& [key, set] : tris) {
                std::fprintf(f, "g tex_%06X_vtx_%06X\n", (uint32_t)(key >> 32), (uint32_t)key);
                for (auto& [a, b, c] : set) { std::fprintf(f, "f %d %d %d\n", a + 1, b + 1, c + 1); n++; }
            }
            std::fclose(f);
            std::printf("[capture] %zu vertices, %zu triangles -> %s\n", verts.size(), n, path.c_str());
        }
    } capture;

    class NullRenderer : public ultramodern::renderer::RendererContext {
    public:
        NullRenderer() {
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        }
        bool valid() override { return true; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dl(const OSTask* task) override {
            ww::frame_limit();
            uint32_t n = ++display_lists;
            if (n <= 3 || n % 120 == 0) {
                std::printf("[null renderer] display list #%u: ucode=0x%08X data_ptr=0x%08X size=0x%X\n",
                    n, (uint32_t)task->t.ucode, (uint32_t)task->t.data_ptr, (uint32_t)task->t.data_size);
            }
            capture.rdram = capture.rdram ? capture.rdram : nullptr;
            capture.dl((uint32_t)task->t.data_ptr);
            ww::asset_capture_dl((uint32_t)task->t.data_ptr);
        }
        void send_dummy_workload(uint32_t) override {}
        void update_screen() override { ++screen_updates; }
        void shutdown() override {
            std::printf("[null renderer] shutdown after %u display lists, %u screen updates\n",
                display_lists.load(), screen_updates.load());
            capture.write();
            ww::asset_capture_finish();
        }
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return 1.0f; }
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> ww::create_null_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    capture.rdram = rdram;
    ww::asset_capture_init(rdram);
    if (const char* p = std::getenv("WW_CAPTURE_OBJ")) {
        capture.path = p;
    }
    return std::make_unique<NullRenderer>();
}
