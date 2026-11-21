// Asset capture (headless test build): WW_ASSET_DIR=<folder> walks every F3DEX2 display list the
// game submits and records what it draws, with textures:
//   textures/tex_<addr>_<fmt><bits>_<w>x<h>.rgba   raw RGBA8 (converted to PNG by tools/assets_post.py)
//   textures.txt                                   addr fmt siz w h tlut file
//   frame_<vi>.obj / .mtl                           everything drawn in one frame (WW_ASSET_FRAMES=vi,vi,...),
//                                                   one OBJ object per matrix (terrain, kayak, water pieces)
//   objects/obj_<n>_tris_<static|animated>_<hash>   each of those objects alone, in model coordinates
// "Animated" = the vertex buffer's bytes changed between frames (the water surface).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "ww.hpp"

namespace {
    struct Mat { float m[4][4]; };
    struct Vtx { float x, y, z, s, t; uint32_t src; float lx, ly, lz; uint32_t mtx; float r, g, b; };
    struct Tile { int fmt = 0, siz = 0, line = 0, tmem = 0, pal = 0, shifts = 0, shiftt = 0; int uls = 0, ult = 0, lrs = 0, lrt = 0; };
    struct TexKey {
        uint32_t addr; int fmt, siz, w, h, stride; uint32_t tlut;
        bool operator<(const TexKey& o) const { return std::tie(addr, fmt, siz, w, h, stride, tlut) < std::tie(o.addr, o.fmt, o.siz, o.w, o.h, o.stride, o.tlut); }
    };
    struct Tri { float p[3][3]; float l[3][3]; float uv[3][2]; float c[3][3]; int tex; uint32_t mtx; bool dyn; };

    std::mutex mutex;
    uint8_t* rdram = nullptr;
    std::filesystem::path dir;
    std::set<uint32_t> frames_wanted;
    std::map<TexKey, int> textures;
    std::vector<TexKey> texture_list;
    std::map<uint32_t, uint64_t> vbuf_hash;      // vertex buffer address -> content hash (first seen)
    std::set<uint32_t> vbuf_dynamic;             // buffers whose bytes changed
    bool first_snapshot = true;
    std::set<uint64_t> objects_written;          // content hashes of objects already exported

    uint32_t r32(uint32_t a) { return *(uint32_t*)(rdram + (a & 0xFFFFFC)); }
    int16_t r16(uint32_t a) { return *(int16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint16_t ru16(uint32_t a) { return *(uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint8_t r8(uint32_t a) { return rdram[(a & 0xFFFFFF) ^ 3]; }

    Mat load_mtx(uint32_t a) {
        Mat r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                int k = i * 4 + j;
                r.m[i][j] = (float)(((int64_t)r16(a + k * 2) << 16) | ru16(a + 32 + k * 2)) / 65536.0f;
            }
        return r;
    }
    Mat mul(const Mat& a, const Mat& b) {
        Mat r{};
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }

    // Decodes a texture from RDRAM to RGBA8.
    std::vector<uint8_t> decode(const TexKey& k) {
        std::vector<uint8_t> out((size_t)k.w * k.h * 4, 0);
        auto pal = [&](int idx, uint8_t* px) {
            uint16_t c = ru16(k.tlut + idx * 2);
            px[0] = ((c >> 11) & 31) * 255 / 31; px[1] = ((c >> 6) & 31) * 255 / 31; px[2] = ((c >> 1) & 31) * 255 / 31; px[3] = (c & 1) ? 255 : 0;
        };
        for (int y = 0; y < k.h; y++) {
            for (int x = 0; x < k.w; x++) {
                uint8_t* px = &out[((size_t)y * k.w + x) * 4];
                int n = y * k.stride + x;   // texel index
                switch (k.siz) {
                case 3: { // 32-bit
                    uint32_t a = k.addr + n * 4;
                    px[0] = r8(a); px[1] = r8(a + 1); px[2] = r8(a + 2); px[3] = r8(a + 3);
                    break;
                }
                case 2: { // 16-bit
                    uint16_t c = ru16(k.addr + n * 2);
                    if (k.fmt == 3) { px[0] = px[1] = px[2] = c >> 8; px[3] = c & 0xFF; }           // IA16
                    else { px[0] = ((c >> 11) & 31) * 255 / 31; px[1] = ((c >> 6) & 31) * 255 / 31; px[2] = ((c >> 1) & 31) * 255 / 31; px[3] = (c & 1) ? 255 : 0; }
                    break;
                }
                case 1: { // 8-bit
                    uint8_t c = r8(k.addr + n);
                    if (k.fmt == 2) pal(c, px);                                                    // CI8
                    else if (k.fmt == 3) { px[0] = px[1] = px[2] = (c >> 4) * 17; px[3] = (c & 15) * 17; } // IA8
                    else { px[0] = px[1] = px[2] = px[3] = c; }                                    // I8
                    break;
                }
                default: { // 4-bit
                    uint8_t b = r8(k.addr + n / 2);
                    int c = (n & 1) ? (b & 15) : (b >> 4);
                    if (k.fmt == 2) pal(c, px);                                                    // CI4 (palette offset in tlut)
                    else if (k.fmt == 3) { int i = (c >> 1) * 255 / 7; px[0] = px[1] = px[2] = i; px[3] = (c & 1) ? 255 : 0; } // IA4
                    else { px[0] = px[1] = px[2] = px[3] = c * 17; }                               // I4
                    break;
                }
                }
            }
        }
        return out;
    }

    int texture_id(const TexKey& k) {
        auto it = textures.find(k);
        if (it != textures.end()) return it->second;
        int id = (int)texture_list.size();
        textures[k] = id;
        texture_list.push_back(k);
        std::vector<uint8_t> px = decode(k);
        char name[96];
        std::snprintf(name, sizeof(name), "tex_%06X_%d%d_%dx%d.rgba", k.addr, k.fmt, k.siz, k.w, k.h);
        std::filesystem::create_directories(dir / "textures");
        if (FILE* f = std::fopen((dir / "textures" / name).string().c_str(), "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
        if (FILE* f = std::fopen((dir / "textures.txt").string().c_str(), "a")) {
            std::fprintf(f, "%06X %d %d %d %d %06X %s\n", k.addr, k.fmt, k.siz, k.w, k.h, k.tlut, name);
            std::fclose(f);
        }
        return id;
    }

    void write_obj(const std::filesystem::path& base, const std::vector<Tri>& tris, bool local = false, const char* tex_prefix = "textures/") {
        std::string mtl = base.filename().string() + ".mtl";
        FILE* m = std::fopen((base.string() + ".mtl").c_str(), "w");
        FILE* o = std::fopen((base.string() + ".obj").c_str(), "w");
        if (!m || !o) return;
        std::set<int> used;
        for (const Tri& t : tris) used.insert(t.tex);
        for (int id : used) {
            if (id < 0) { std::fprintf(m, "newmtl untextured\nKd 0.7 0.7 0.7\n\n"); continue; }
            const TexKey& k = texture_list[id];
            std::fprintf(m, "newmtl t%06X_%dx%d\nKd 1 1 1\nmap_Kd %stex_%06X_%d%d_%dx%d.png\n\n", k.addr, k.w, k.h, tex_prefix, k.addr, k.fmt, k.siz, k.w, k.h);
        }
        std::fprintf(o, "mtllib %s\n", mtl.c_str());
        size_t v = 1;
        int cur = -2;
        uint64_t cur_group = ~0ULL;
        for (const Tri& t : tris) {
            uint64_t group = ((uint64_t)t.mtx << 1) | (t.dyn ? 1 : 0);
            if (!local && group != cur_group) {
                cur_group = group;
                std::fprintf(o, "o obj_%06X_%s\n", t.mtx, t.dyn ? "animated" : "static");
                cur = -2;
            }
            if (t.tex != cur) {
                cur = t.tex;
                if (cur < 0) std::fprintf(o, "usemtl untextured\n");
                else { const TexKey& k = texture_list[cur]; std::fprintf(o, "usemtl t%06X_%dx%d\n", k.addr, k.w, k.h); }
            }
            const float (*pp)[3] = local ? t.l : t.p;
            for (int i = 0; i < 3; i++) std::fprintf(o, "v %.3f %.3f %.3f %.3f %.3f %.3f\nvt %.5f %.5f\n", pp[i][0], pp[i][1], pp[i][2], t.c[i][0], t.c[i][1], t.c[i][2], t.uv[i][0], 1.0f - t.uv[i][1]);
            std::fprintf(o, "f %zu/%zu %zu/%zu %zu/%zu\n", v, v, v + 1, v + 1, v + 2, v + 2);
            v += 3;
        }
        std::fclose(m);
        std::fclose(o);
    }

    void walk(uint32_t dl, std::vector<Tri>* frame) {
        uint32_t seg[16] = {};
        std::vector<Mat> stack(1);
        std::vector<uint32_t> mtx_addr(1, 0);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) stack[0].m[i][j] = i == j;
        Vtx vb[64] = {};
        Tile tiles[8];
        uint32_t timg = 0, timg_w = 1, tlut = 0;
        int timg_siz = 2;
        std::map<int, std::pair<uint32_t, uint32_t>> tmem_img;   // TMEM address -> (image address, stride)
        int tex_tile = 0;
        float tex_s = 1, tex_t = 1;
        bool tex_on = false;
        uint32_t geom = 0;   // geometry mode (G_LIGHTING 0x20000: vertex bytes 12-14 are a normal, not a colour)
        std::vector<uint32_t> calls;
        auto addr = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
        uint32_t pc = dl & 0xFFFFFF;
        for (int guard = 0; guard < 600000; guard++) {
            uint32_t w0 = r32(pc), w1 = r32(pc + 4);
            pc += 8;
            switch (w0 >> 24) {
            case 0xDB: if (((w0 >> 16) & 0xFF) == 0x06) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF; break;
            case 0xDA: {
                uint32_t p = (w0 & 0xFF) ^ 1;
                if (p & 4) break;
                Mat m = load_mtx(addr(w1));
                Mat res = (p & 2) ? m : mul(m, stack.back());
                if (p & 1) { stack.push_back(res); mtx_addr.push_back(addr(w1)); } else { stack.back() = res; mtx_addr.back() = addr(w1); }
                break;
            }
            case 0xD8: { uint32_t n = w1 / 64; while (n-- && stack.size() > 1) { stack.pop_back(); mtx_addr.pop_back(); } break; }
            case 0xD9: geom = (geom & (w0 & 0xFFFFFF)) | w1; break;
            case 0xD7: tex_on = (w0 & 0xFF) != 0; tex_tile = (w0 >> 8) & 7; tex_s = (w1 >> 16) / 65536.0f; tex_t = (w1 & 0xFFFF) / 65536.0f; break;
            case 0x01: {
                uint32_t n = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - n, a = addr(w1);
                // static/dynamic tracking of the vertex buffer
                uint64_t h = 1469598103934665603ULL;
                for (uint32_t i = 0; i < n * 4; i++) h = (h ^ r32(a + i * 4)) * 1099511628211ULL;
                auto it = vbuf_hash.find(a);
                if (it == vbuf_hash.end()) vbuf_hash[a] = h; else if (it->second != h) vbuf_dynamic.insert(a);
                const Mat& m = stack.back();
                for (uint32_t i = 0; i < n && v0 + i < 64; i++) {
                    float x = r16(a + i * 16), y = r16(a + i * 16 + 2), z = r16(a + i * 16 + 4);
                    vb[v0 + i] = { x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0],
                                   x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1],
                                   x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2],
                                   r16(a + i * 16 + 8) / 32.0f, r16(a + i * 16 + 10) / 32.0f, a, x, y, z, mtx_addr.back(), 1, 1, 1 };
                    if (!(geom & 0x20000)) {
                        vb[v0 + i].r = r8(a + i * 16 + 12) / 255.0f; vb[v0 + i].g = r8(a + i * 16 + 13) / 255.0f; vb[v0 + i].b = r8(a + i * 16 + 14) / 255.0f;
                    }
                }
                break;
            }
            case 0xFD: timg = addr(w1); timg_siz = (w0 >> 19) & 3; timg_w = (w0 & 0xFFF) + 1; break;
            case 0xF5: { int t = (w1 >> 24) & 7; Tile& T = tiles[t];
                T.fmt = (w0 >> 21) & 7; T.siz = (w0 >> 19) & 3; T.line = (w0 >> 9) & 0x1FF; T.tmem = w0 & 0x1FF; T.pal = (w1 >> 20) & 15;
                T.shiftt = (w1 >> 10) & 15; T.shifts = w1 & 15; break; }
            case 0xF2: { int t = (w1 >> 24) & 7; Tile& T = tiles[t];
                T.uls = (w0 >> 12) & 0xFFF; T.ult = w0 & 0xFFF; T.lrs = (w1 >> 12) & 0xFFF; T.lrt = w1 & 0xFFF; break; }
            case 0xF3: { int t = (w1 >> 24) & 7; tmem_img[tiles[t].tmem] = { timg, 0 }; break; }  // G_LOADBLOCK
            case 0xF4: { int t = (w1 >> 24) & 7; uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
                int bpp = timg_siz == 3 ? 4 : timg_siz == 2 ? 2 : 1;
                tmem_img[tiles[t].tmem] = { timg + (ult * timg_w + uls) * bpp, timg_w }; break; }
            case 0xF0: tlut = timg; break;  // G_LOADTLUT
            case 0x05: case 0x06: case 0x07: {
                Tile& T = tiles[tex_tile];
                // find the load that filled this tile's TMEM (same tmem address)
                uint32_t img = 0, stride = 0;
                if (auto it = tmem_img.find(T.tmem); it != tmem_img.end()) { img = it->second.first; stride = it->second.second; }
                int w = ((T.lrs - T.uls) >> 2) + 1, hgt = ((T.lrt - T.ult) >> 2) + 1;
                int tex = -1;
                if (tex_on && img && w > 0 && hgt > 0 && w <= 1024 && hgt <= 1024) {
                    uint32_t pal_addr = tlut + (T.siz == 0 ? T.pal * 16 * 2 : 0);
                    TexKey k{ img, T.fmt, T.siz, w, hgt, stride ? (int)stride : w, (T.fmt == 2) ? pal_addr : 0 };
                    tex = texture_id(k);
                }
                auto shift = [](float c, int s) { return s == 0 ? c : (s <= 10 ? c / (float)(1 << s) : c * (float)(1 << (16 - s))); };
                auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
                    a /= 2; b /= 2; c /= 2;
                    if (a >= 64 || b >= 64 || c >= 64) return;
                    Tri tri{};
                    const Vtx* vv[3] = { &vb[a], &vb[b], &vb[c] };
                    for (int i = 0; i < 3; i++) {
                        tri.p[i][0] = vv[i]->x; tri.p[i][1] = vv[i]->y; tri.p[i][2] = vv[i]->z;
                        tri.l[i][0] = vv[i]->lx; tri.l[i][1] = vv[i]->ly; tri.l[i][2] = vv[i]->lz;
                        tri.c[i][0] = vv[i]->r; tri.c[i][1] = vv[i]->g; tri.c[i][2] = vv[i]->b;
                        float s = shift(vv[i]->s * tex_s, T.shifts) - T.uls / 4.0f, t = shift(vv[i]->t * tex_t, T.shiftt) - T.ult / 4.0f;
                        tri.uv[i][0] = w > 0 ? s / w : 0; tri.uv[i][1] = hgt > 0 ? t / hgt : 0;
                    }
                    tri.tex = tex;
                    tri.mtx = vv[0]->mtx;
                    bool dyn = vbuf_dynamic.count(vv[0]->src) || vbuf_dynamic.count(vv[1]->src) || vbuf_dynamic.count(vv[2]->src);
                    tri.dyn = dyn;
                    if (frame) frame->push_back(tri);
                };
                emit((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF);
                if ((w0 >> 24) != 0x05) emit((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
                break;
            }
            case 0xDE: if (((w0 >> 16) & 0xFF) == 0) calls.push_back(pc); pc = addr(w1); break;
            case 0xDF: if (calls.empty()) return; pc = calls.back(); calls.pop_back(); break;
            case 0xDD: return;
            default: break;
            }
        }
    }
}

bool ww::asset_capture_enabled() {
    static const bool on = std::getenv("WW_ASSET_DIR") != nullptr;
    return on;
}

void ww::asset_capture_init(uint8_t* rd) {
    rdram = rd;
    if (!asset_capture_enabled()) return;
    dir = std::getenv("WW_ASSET_DIR");
    std::filesystem::create_directories(dir);
    std::remove((dir / "textures.txt").string().c_str());
    if (const char* f = std::getenv("WW_ASSET_FRAMES")) {
        std::string s = f;
        size_t p = 0;
        while (p < s.size()) { frames_wanted.insert((uint32_t)std::strtoul(s.c_str() + p, nullptr, 10)); p = s.find(',', p); if (p == std::string::npos) break; p++; }
    }
}

void ww::asset_capture_dl(uint32_t data_ptr) {
    if (!asset_capture_enabled() || !rdram) return;
    std::lock_guard lock(mutex);
    uint32_t vi = ww::current_vi();
    std::vector<Tri> frame;
    bool snap = false;
    for (uint32_t f : frames_wanted) if (vi >= f) { snap = true; frames_wanted.erase(f); break; }
    walk(data_ptr, snap ? &frame : nullptr);
    if (snap) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05u", vi);
        std::stable_sort(frame.begin(), frame.end(), [](const Tri& a, const Tri& b) { return std::tie(a.mtx, a.dyn) < std::tie(b.mtx, b.dyn); });
        write_obj(dir / name, frame);
        // each object (triangles drawn under one matrix) on its own, in its model coordinates
        std::filesystem::create_directories(dir / "objects");
        size_t written = 0;
        for (size_t i = 0; i < frame.size();) {
            size_t j = i;
            while (j < frame.size() && frame[j].mtx == frame[i].mtx && frame[j].dyn == frame[i].dyn) j++;
            std::vector<Tri> obj(frame.begin() + i, frame.begin() + j);
            uint64_t h = 1469598103934665603ULL;
            for (const Tri& t : obj) {
                for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) h = (h ^ (uint64_t)(int64_t)std::lround(t.l[a][b] * 8)) * 1099511628211ULL;
                h = (h ^ (uint64_t)(t.tex + 1)) * 1099511628211ULL;
            }
            // animated objects (water) change every frame: export them from the first snapshot only
            if ((!frame[i].dyn || first_snapshot) && objects_written.insert(h).second) {
                char on[96];
                std::snprintf(on, sizeof(on), "obj_%05zu_tris_%s_%016llX", obj.size(), frame[i].dyn ? "animated" : "static", (unsigned long long)h);
                write_obj(dir / "objects" / on, obj, true, "../textures/");
                written++;
            }
            i = j;
        }
        first_snapshot = false;
        std::printf("[assets] frame snapshot %s: %zu triangles, %zu new objects\n", name, frame.size(), written);
    }
}

void ww::asset_capture_finish() {
    if (!asset_capture_enabled()) return;
    std::lock_guard lock(mutex);
    std::printf("[assets] %zu textures\n", texture_list.size());
}
