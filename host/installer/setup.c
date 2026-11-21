/* WildWaters PC - Setup
 *
 * One self-contained exe: the PC program files are embedded (payload.bin, tools/make_payload.py). The
 * player points it at their own WildWaters ROM (the Jun 15 1999 prototype, .z64 / .v64 / .n64 byte
 * order all accepted); Setup checks it is that exact build (the same XXH3-64 hash the port checks),
 * writes the PC version, stores the ROM where the port reads it, and adds shortcuts and an uninstall
 * entry. Nothing from the game is shipped in here: the program icon is cut from the player's own ROM
 * at install time (the WildWaters logo, picture 0x11332).
 *
 * The install is portable (portable.txt): settings, saves, mods and the ROM copy all live in the
 * install folder, so a per-user folder that needs no admin rights works and uninstall is clean.
 *
 *   "WildWaters PC Setup.exe"                                   GUI
 *   "WildWaters PC Setup.exe" /S /rom <file> /dir <dir> [/nodesktop] [/nostartmenu]   silent (exit code 0 = ok)
 *   Uninstall.exe /uninstall [/S] [/delsaves]                    remove (re-launches itself from %TEMP%)
 */
#define UNICODE
#define _UNICODE
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

#define APP_NAME    L"WildWaters PC"
#define EXE_NAME    L"WildWatersPC.exe"
#define UNINST_EXE  L"Uninstall.exe"
#define ICON_FILE   L"WildWaters PC.ico"
#define ROM_STORED  L"wildwaters.n64.proto.19990615.z64"   /* GameEntry::stored_filename(): game id + .z64 */
#define REG_KEY     L"WildWatersPC"
#define UNINST_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" REG_KEY
#define ROM_HASH    0x3EF9EA187C5BBEDEull                  /* XXH3-64 of the big-endian ROM (host/src/ww.hpp) */
#define RES_PAYLOAD 101

/* ------------------------------------------------------------------ small helpers */
static wchar_t g_self[MAX_PATH];
static int g_silent;
static volatile LONG g_cancel;
static HWND g_wnd;

static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b) { swprintf(out, MAX_PATH * 2, L"%s\\%s", a, b); }
static int file_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int dir_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void trim_path(wchar_t *p) {
    wchar_t *s = p; while (*s == L' ' || *s == L'"') s++;
    memmove(p, s, (wcslen(s) + 1) * sizeof(wchar_t));
    size_t n = wcslen(p); while (n && (p[n - 1] == L' ' || p[n - 1] == L'"')) p[--n] = 0;
    while (n > 3 && p[n - 1] == L'\\') p[--n] = 0;
}
static int make_dirs(const wchar_t *p) { int r = SHCreateDirectoryExW(NULL, p, NULL); return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS; }

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_STATUS   (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)
static void say(const wchar_t *fmt, ...) {
    wchar_t *b = (wchar_t *)LocalAlloc(LMEM_FIXED, 1024 * sizeof(wchar_t)); va_list a; va_start(a, fmt); vswprintf(b, 1024, fmt, a); va_end(a);
    if (g_silent) { HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); char u[2048]; int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, u, sizeof u - 2, NULL, NULL); if (n > 0 && h && h != INVALID_HANDLE_VALUE) { u[n - 1] = '\n'; DWORD w; WriteFile(h, u, n, &w, NULL); } LocalFree(b); }
    else if (!PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM)b)) LocalFree(b);
}
static void progress(uint64_t done, uint64_t total) {
    static int last = -1; int pm = total ? (int)(done * 1000 / total) : 0;
    if (pm != last) { last = pm; if (!g_silent) PostMessageW(g_wnd, WM_APP_PROGRESS, pm, 0); }
}

/* ------------------------------------------------------------------ the ROM */
enum { ROM_OK, ROM_MISSING, ROM_NOT_N64, ROM_OTHER, ROM_ZIPPED };
/* Reads + checks a ROM file. On ROM_OK, *data (malloc'd, big-endian .z64 order) and *size are set. */
static int rom_load(const wchar_t *path, uint8_t **data, DWORD *size) {
    *data = NULL; *size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return ROM_MISSING;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 0x1000 || sz.QuadPart > (64 << 20)) { CloseHandle(h); return ROM_NOT_N64; }
    DWORD n = (DWORD)sz.QuadPart, padded = (n + 3) & ~3u, got = 0;
    uint8_t *b = (uint8_t *)calloc(padded, 1);
    int ok = b && ReadFile(h, b, n, &got, NULL) && got == n; CloseHandle(h);
    if (!ok) { free(b); return ROM_MISSING; }
    if (b[0] == 'P' && b[1] == 'K') { free(b); return ROM_ZIPPED; }
    /* byte order from the first word (0x80371240 in .z64) */
    if (b[0] == 0x37 && b[1] == 0x80) { for (DWORD i = 0; i + 1 < padded; i += 2) { uint8_t t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; } }        /* .v64 */
    else if (b[0] == 0x40 && b[1] == 0x12) { for (DWORD i = 0; i + 3 < padded; i += 4) { uint8_t t0 = b[i], t1 = b[i + 1]; b[i] = b[i + 3]; b[i + 1] = b[i + 2]; b[i + 2] = t1; b[i + 3] = t0; } }   /* .n64 */
    if (!(b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40)) { free(b); return ROM_NOT_N64; }
    if (XXH3_64bits(b, padded) != ROM_HASH) { free(b); return ROM_OTHER; }
    *data = b; *size = padded;
    return ROM_OK;
}
static int rom_check_file(const wchar_t *path) { uint8_t *d; DWORD n; int r = rom_load(path, &d, &n); free(d); return r; }
static int existing_rom_ok(const wchar_t *dir) { wchar_t p[MAX_PATH * 2]; path_join(p, dir, ROM_STORED); return file_exists(p) && rom_check_file(p) == ROM_OK; }
static const wchar_t *rom_error(int r) {
    switch (r) {
    case ROM_MISSING: return L"Can't open that file.";
    case ROM_ZIPPED:  return L"That's a .zip archive: unzip it first, then choose the .z64 / .v64 / .n64 file inside.";
    case ROM_NOT_N64: return L"That file isn't a Nintendo 64 ROM.";
    case ROM_OTHER:   return L"That's a different ROM. Setup needs the WildWaters prototype dated Jun 15, 1999 (16 MB).";
    }
    return L"";
}

/* ------------------------------------------------------------------ embedded program files */
static const uint8_t *payload(DWORD *size) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(RES_PAYLOAD), (LPCWSTR)RT_RCDATA);
    if (!r) return NULL;
    *size = SizeofResource(NULL, r);
    return (const uint8_t *)LockResource(LoadResource(NULL, r));
}
static int write_all(const wchar_t *dst, const void *p, DWORD n) {
    HANDLE h = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0; BOOL ok = WriteFile(h, p, n, &w, NULL) && w == n; CloseHandle(h);
    return ok;
}
/* walks the archive; fn(rel path, data, size) */
typedef int (*FileFn)(const wchar_t *rel, const uint8_t *data, uint64_t size, void *ctx);
static int payload_walk(FileFn fn, void *ctx) {
    DWORD n = 0; const uint8_t *p = payload(&n);
    if (!p || n < 12 || memcmp(p, "WWPK", 4)) return 0;
    uint32_t count; memcpy(&count, p + 8, 4);
    size_t o = 12;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t pl; uint64_t sz;
        if (o + 2 > n) return 0;
        memcpy(&pl, p + o, 2); o += 2;
        if (o + pl + 8 > n) return 0;
        wchar_t rel[MAX_PATH]; int k = MultiByteToWideChar(CP_UTF8, 0, (const char *)p + o, pl, rel, MAX_PATH - 1); rel[k] = 0; o += pl;
        memcpy(&sz, p + o, 8); o += 8;
        if (o + sz > n || !k || wcsstr(rel, L"..")) return 0;
        if (!fn(rel, p + o, sz, ctx)) return 0;
        o += (size_t)sz;
    }
    return 1;
}
typedef struct { const wchar_t *dir; uint64_t done, total; wchar_t err[MAX_PATH * 2]; } Extract;
static int size_cb(const wchar_t *rel, const uint8_t *d, uint64_t sz, void *ctx) { ((Extract *)ctx)->total += sz; return 1; }
static int extract_cb(const wchar_t *rel, const uint8_t *d, uint64_t sz, void *ctx) {
    Extract *e = (Extract *)ctx;
    if (g_cancel) return 0;
    wchar_t dst[MAX_PATH * 2], parent[MAX_PATH * 2]; path_join(dst, e->dir, rel);
    wcscpy(parent, dst); PathRemoveFileSpecW(parent); make_dirs(parent);
    if (!write_all(dst, d, (DWORD)sz)) { wcscpy(e->err, dst); return 0; }
    e->done += sz; progress(e->done, e->total);
    return 1;
}

/* ------------------------------------------------------------------ game icon, from the player's own ROM
 * The WildWaters logo (asset 0x11332: RGBA32 144x62 at ROM 0x274490, see assets/raw/LIST.md) becomes the
 * installed exe's icon (its resources rewritten) and an .ico file for the shortcuts. */
#define LOGO_ROM 0x274490
#define LOGO_W   144
#define LOGO_H   62
static void resample(const uint8_t *s, int sw, int sh, uint8_t *d, int dw, int dh) {   /* RGBA, premultiplied filtering */
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        double acc[4] = { 0 }, wsum = 0;
        if (dw >= sw) {
            double fx = (x + 0.5) * sw / dw - 0.5, fy = (y + 0.5) * sh / dh - 0.5; int x0 = (int)floor(fx), y0 = (int)floor(fy); double ax = fx - x0, ay = fy - y0;
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                int sx = x0 + i < 0 ? 0 : x0 + i >= sw ? sw - 1 : x0 + i, sy = y0 + j < 0 ? 0 : y0 + j >= sh ? sh - 1 : y0 + j;
                double wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay); const uint8_t *p = s + ((size_t)sy * sw + sx) * 4; double a = p[3] / 255.0;
                acc[0] += p[0] * a * wgt; acc[1] += p[1] * a * wgt; acc[2] += p[2] * a * wgt; acc[3] += p[3] * wgt; wsum += wgt;
            }
        } else {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (int sy = y0; sy < y1; sy++) for (int sx = x0; sx < x1; sx++) {
                const uint8_t *p = s + ((size_t)sy * sw + sx) * 4; double a = p[3] / 255.0;
                acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += p[3]; wsum += 1;
            }
        }
        uint8_t *o = d + ((size_t)y * dw + x) * 4; double a = acc[3] / wsum;
        for (int k = 0; k < 3; k++) { double v = a > 0 ? acc[k] / wsum / (a / 255.0) : 0; o[k] = (uint8_t)(v > 255 ? 255 : v + 0.5); }
        o[3] = (uint8_t)(a + 0.5);
    }
}
static uint8_t *icon_dib(const uint8_t *rgba, int n, DWORD *size) {   /* 32-bit DIB, bottom-up BGRA, empty AND mask */
    DWORD mask = ((n + 31) / 32) * 4 * n, sz = 40 + n * n * 4 + mask;
    uint8_t *b = (uint8_t *)calloc(1, sz); if (!b) return NULL;
    BITMAPINFOHEADER bi = { 40, n, n * 2, 1, 32, BI_RGB, (DWORD)(n * n * 4 + mask) }; memcpy(b, &bi, 40);
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        const uint8_t *s = rgba + ((size_t)y * n + x) * 4; uint8_t *d = b + 40 + ((size_t)(n - 1 - y) * n + x) * 4;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
    }
    *size = sz; return b;
}
#pragma pack(push, 2)
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes; WORD id; } GrpEntry;
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes, offset; } IcoEntry;
#pragma pack(pop)
typedef struct { WORD id, lang; } ResId;
typedef struct { ResId r[64]; int n; } ResList;
static BOOL CALLBACK res_lang_cb(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR p) {
    ResList *l = (ResList *)p; if (l->n < 64 && IS_INTRESOURCE(name)) { l->r[l->n].id = (WORD)(ULONG_PTR)name; l->r[l->n].lang = lang; l->n++; } return TRUE;
}
static BOOL CALLBACK res_name_cb(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p) { EnumResourceLanguagesW(m, type, name, res_lang_cb, p); return TRUE; }

/* 1 = exe icon replaced, 2 = .ico written */
static int apply_game_icon(const wchar_t *dir, const uint8_t *rom, DWORD rom_size) {
    if (!rom || rom_size < LOGO_ROM + LOGO_W * LOGO_H * 4) return 0;
    /* the logo, centred on a square transparent canvas */
    uint8_t *sq = (uint8_t *)calloc((size_t)LOGO_W * LOGO_W, 4);
    if (!sq) return 0;
    int top = (LOGO_W - LOGO_H) / 2;
    for (int y = 0; y < LOGO_H; y++) memcpy(sq + ((size_t)(top + y) * LOGO_W) * 4, rom + LOGO_ROM + (size_t)y * LOGO_W * 4, LOGO_W * 4);
    static const int sizes[] = { 256, 64, 48, 32, 24, 16 }; enum { NS = 6 };
    uint8_t *img[NS] = { 0 }; DWORD isz[NS] = { 0 };
    uint8_t *tmp = (uint8_t *)malloc(256 * 256 * 4);
    for (int i = 0; i < NS && tmp; i++) { resample(sq, LOGO_W, LOGO_W, tmp, sizes[i], sizes[i]); img[i] = icon_dib(tmp, sizes[i], &isz[i]); }
    free(tmp); free(sq);
    for (int i = 0; i < NS; i++) if (!img[i]) { for (int j = 0; j < NS; j++) free(img[j]); return 0; }

    wchar_t ico[MAX_PATH * 2]; path_join(ico, dir, ICON_FILE);
    HANDLE o = CreateFileW(ico, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int wrote_ico = 0;
    if (o != INVALID_HANDLE_VALUE) {
        WORD hd[3] = { 0, 1, NS }; DWORD wr, off = 6 + NS * sizeof(IcoEntry); WriteFile(o, hd, 6, &wr, NULL);
        for (int i = 0; i < NS; i++) { IcoEntry e = { (BYTE)(sizes[i] & 255), (BYTE)(sizes[i] & 255), 0, 0, 1, 32, isz[i], off }; WriteFile(o, &e, sizeof e, &wr, NULL); off += isz[i]; }
        for (int i = 0; i < NS; i++) WriteFile(o, img[i], isz[i], &wr, NULL);
        CloseHandle(o); wrote_ico = 1;
    }
    wchar_t exe[MAX_PATH * 2]; path_join(exe, dir, EXE_NAME);
    ResList icons = { 0 }, groups = { 0 };
    HMODULE m = LoadLibraryExW(exe, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (m) { EnumResourceNamesW(m, RT_ICON, res_name_cb, (LONG_PTR)&icons); EnumResourceNamesW(m, RT_GROUP_ICON, res_name_cb, (LONG_PTR)&groups); FreeLibrary(m); }
    WORD lang = groups.n ? groups.r[0].lang : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    int upd = 0;
    HANDLE u = BeginUpdateResourceW(exe, FALSE);
    if (u) {
        for (int i = 0; i < icons.n; i++) UpdateResourceW(u, RT_ICON, MAKEINTRESOURCEW(icons.r[i].id), icons.r[i].lang, NULL, 0);
        for (int i = 0; i < groups.n; i++) UpdateResourceW(u, RT_GROUP_ICON, MAKEINTRESOURCEW(groups.r[i].id), groups.r[i].lang, NULL, 0);
        struct { WORD res, type, count; GrpEntry e[NS]; } grp = { 0, 1, NS };
        int good = 1;
        for (int i = 0; i < NS; i++) {
            GrpEntry e = { (BYTE)(sizes[i] & 255), (BYTE)(sizes[i] & 255), 0, 0, 1, 32, isz[i], (WORD)(i + 1) }; grp.e[i] = e;
            good &= UpdateResourceW(u, RT_ICON, MAKEINTRESOURCEW(i + 1), lang, img[i], isz[i]) != 0;
        }
        good &= UpdateResourceW(u, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang, &grp, sizeof grp) != 0;
        upd = EndUpdateResourceW(u, !good) && good;
    }
    for (int i = 0; i < NS; i++) free(img[i]);
    return (upd ? 1 : 0) | (wrote_ico ? 2 : 0);
}

/* ------------------------------------------------------------------ shortcuts, registry */
static int make_shortcut(const wchar_t *lnk, const wchar_t *target, const wchar_t *workdir, const wchar_t *icon, const wchar_t *desc) {
    IShellLinkW *sl = NULL; IPersistFile *pf = NULL; int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return 0;
    IShellLinkW_SetPath(sl, target); IShellLinkW_SetWorkingDirectory(sl, workdir); IShellLinkW_SetIconLocation(sl, icon ? icon : target, 0);
    IShellLinkW_SetDescription(sl, desc);
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE)); IPersistFile_Release(pf); }
    IShellLinkW_Release(sl);
    return ok;
}
static void known_path(REFKNOWNFOLDERID id, const wchar_t *leaf, wchar_t *out, size_t n) {
    PWSTR p = NULL; out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p))) { if (swprintf(out, n, L"%s\\%s", p, leaf) < 0) out[0] = 0; CoTaskMemFree(p); }
}
static uint64_t dir_size(const wchar_t *dir) {
    wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); uint64_t t = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { wchar_t sub[MAX_PATH * 2]; path_join(sub, dir, fd.cFileName); t += dir_size(sub); }
        else t += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return t;
}
static void reg_set_sz(HKEY k, const wchar_t *name, const wchar_t *v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t))); }
static void reg_set_dw(HKEY k, const wchar_t *name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4); }

/* ------------------------------------------------------------------ install */
typedef struct { wchar_t rom[MAX_PATH], dir[MAX_PATH]; int desktop, startmenu; } Job;
static Job g_job;
static int g_result;
static int fail(const wchar_t *fmt, ...) {
    wchar_t b[1024]; va_list a; va_start(a, fmt); vswprintf(b, 1024, fmt, a); va_end(a);
    say(L"%s", b); g_result = 1; return 1;
}

static DWORD WINAPI install_thread(void *arg) {
    Job *j = &g_job; g_result = 0;
    uint8_t *rom = NULL; DWORD rom_size = 0;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    trim_path(j->rom); trim_path(j->dir);
    if (!j->dir[0]) { fail(L"Choose a folder to install to."); goto out; }
    if (PathIsRelativeW(j->dir)) { fail(L"Use a full install path (for example C:\\Games\\WildWaters PC)."); goto out; }
    int have_rom = existing_rom_ok(j->dir);
    if (j->rom[0]) {
        say(L"Checking the ROM...");
        int r = rom_load(j->rom, &rom, &rom_size);
        if (r != ROM_OK) { fail(L"%s", rom_error(r)); goto out; }
    } else if (!have_rom) { fail(L"Choose your WildWaters ROM file (.z64, .v64 or .n64)."); goto out; }

    if (!make_dirs(j->dir)) { fail(L"Can't create the folder \"%s\". Pick another place (one you can write to).", j->dir); goto out; }
    Extract e = { j->dir, 0, 0, L"" };
    payload_walk(size_cb, &e);
    ULARGE_INTEGER freeb;
    if (GetDiskFreeSpaceExW(j->dir, &freeb, NULL, NULL) && freeb.QuadPart < e.total + rom_size + (16ull << 20)) {
        fail(L"Not enough free space: needs %.0f MB.", (e.total + rom_size + (16ull << 20)) / 1048576.0); goto out;
    }

    say(L"Writing the PC version...");
    if (!payload_walk(extract_cb, &e)) {
        if (g_cancel) fail(L"Cancelled.");
        else if (e.err[0]) fail(L"Can't write \"%s\". If the game is running, close it and try again.", e.err);
        else fail(L"The setup file is damaged: download it again.");
        goto out;
    }
    wchar_t p[MAX_PATH * 2];
    path_join(p, j->dir, L"portable.txt");
    const char *portable = "Settings, saves, mods and the ROM copy are kept in this folder.\r\n";
    write_all(p, portable, (DWORD)strlen(portable));
    if (rom) {
        say(L"Storing the ROM...");
        path_join(p, j->dir, ROM_STORED);
        if (!write_all(p, rom, rom_size)) { fail(L"Can't write \"%s\".", p); goto out; }
    }
    {   /* the game icon, from the player's ROM (an update without a new ROM uses the stored copy) */
        say(L"Adding the game icon...");
        uint8_t *src = rom; DWORD src_size = rom_size;
        if (!src) { path_join(p, j->dir, ROM_STORED); rom_load(p, &src, &src_size); }
        int r = apply_game_icon(j->dir, src, src_size);
        if (src != rom) free(src);
        if (!(r & 1)) say(L"Note: the game icon could not be added to the program file (shortcuts still get it).");
    }
    path_join(p, j->dir, UNINST_EXE);
    wchar_t self_full[MAX_PATH]; GetFullPathNameW(g_self, MAX_PATH, self_full, NULL);
    if (_wcsicmp(self_full, p)) CopyFileW(g_self, p, FALSE);

    wchar_t exe[MAX_PATH * 2], lnk[MAX_PATH * 2], ico[MAX_PATH * 2]; path_join(exe, j->dir, EXE_NAME); path_join(ico, j->dir, ICON_FILE);
    const wchar_t *icon = file_exists(ico) ? ico : exe;
    if (j->desktop) { known_path(&FOLDERID_Desktop, APP_NAME L".lnk", lnk, MAX_PATH * 2); if (lnk[0]) make_shortcut(lnk, exe, j->dir, icon, APP_NAME); }
    if (j->startmenu) { known_path(&FOLDERID_Programs, APP_NAME L".lnk", lnk, MAX_PATH * 2); if (lnk[0]) make_shortcut(lnk, exe, j->dir, icon, APP_NAME); }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        wchar_t u[MAX_PATH * 2 + 32]; path_join(p, j->dir, UNINST_EXE); swprintf(u, MAX_PATH * 2 + 32, L"\"%s\" /uninstall", p);
        reg_set_sz(k, L"DisplayName", APP_NAME); reg_set_sz(k, L"DisplayIcon", icon); reg_set_sz(k, L"UninstallString", u);
        reg_set_sz(k, L"InstallLocation", j->dir); reg_set_sz(k, L"Publisher", L"WildWaters PC port"); reg_set_sz(k, L"DisplayVersion", L"1.0");
        reg_set_dw(k, L"NoModify", 1); reg_set_dw(k, L"NoRepair", 1); reg_set_dw(k, L"EstimatedSize", (DWORD)(dir_size(j->dir) >> 10));
        RegCloseKey(k);
    }
    progress(1, 1);
    say(L"Done! WildWaters PC is installed in %s", j->dir);
out:
    free(rom);
    CoUninitialize();
    if (!g_silent) PostMessageW(g_wnd, WM_APP_DONE, g_result, 0);
    return 0;
}

/* ------------------------------------------------------------------ uninstall */
static int delete_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t sub[MAX_PATH * 2]; path_join(sub, dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) delete_tree(sub);
            else { SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL); if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(sub); else DeleteFileW(sub); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir);
}
static void del_in(const wchar_t *dir, const wchar_t *leaf) { wchar_t p[MAX_PATH * 2]; path_join(p, dir, leaf); DeleteFileW(p); }
static void del_pattern(const wchar_t *dir, const wchar_t *pattern) {
    wchar_t pat[MAX_PATH * 2]; path_join(pat, dir, pattern);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) del_in(dir, fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int uninstall_run(const wchar_t *dir, int ask, int delsaves) {
    wchar_t exe[MAX_PATH * 2]; path_join(exe, dir, EXE_NAME);
    if (ask) {
        wchar_t m[MAX_PATH * 2 + 128]; swprintf(m, MAX_PATH * 2 + 128, L"Remove " APP_NAME L" from\n%s ?", dir);
        if (MessageBoxW(NULL, m, APP_NAME L" - Uninstall", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
        int r = MessageBoxW(NULL, L"Also delete your saved games and settings?\n\nChoose No to keep them in the folder (a later install there picks them up).",
                            APP_NAME L" - Uninstall", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (r == IDCANCEL) return 1;
        delsaves = r == IDYES;
    }
    wchar_t un[MAX_PATH * 2]; path_join(un, dir, UNINST_EXE);
    for (int i = 0; i < 50 && file_exists(un) && !DeleteFileW(un); i++) Sleep(100);
    for (int i = 0; i < 50; i++) { if (!file_exists(exe) || DeleteFileW(exe)) break; if (i == 49 && ask) { MessageBoxW(NULL, L"The game seems to be running. Close it and run the uninstaller again.", APP_NAME, MB_ICONWARNING); return 2; } Sleep(100); }
    del_in(dir, L"SDL2.dll"); del_in(dir, L"dxcompiler.dll"); del_in(dir, L"dxil.dll"); del_in(dir, ICON_FILE);
    del_in(dir, L"msvcp140.dll"); del_in(dir, L"msvcp140_atomic_wait.dll"); del_in(dir, L"vcruntime140.dll"); del_in(dir, L"vcruntime140_1.dll");
    del_in(dir, ROM_STORED); del_pattern(dir, L"*.log");
    wchar_t p[MAX_PATH * 2];
    path_join(p, dir, L"assets"); delete_tree(p);
    if (delsaves) {
        path_join(p, dir, L"saves"); delete_tree(p);
        path_join(p, dir, L"mods"); delete_tree(p);
        path_join(p, dir, L"mod_config"); delete_tree(p);
        del_pattern(dir, L"*.json"); del_pattern(dir, L"*.json.bak"); del_in(dir, L"ww_framerate_v1");
        del_in(dir, L"portable.txt");
    }
    /* saves/settings kept: portable.txt stays, so a later install in this folder picks them up */
    RemoveDirectoryW(dir);
    known_path(&FOLDERID_Desktop, APP_NAME L".lnk", p, MAX_PATH * 2); if (p[0]) DeleteFileW(p);
    known_path(&FOLDERID_Programs, APP_NAME L".lnk", p, MAX_PATH * 2); if (p[0]) DeleteFileW(p);
    RegDeleteKeyW(HKEY_CURRENT_USER, UNINST_KEY);
    if (ask) MessageBoxW(NULL, APP_NAME L" has been removed.", APP_NAME L" - Uninstall", MB_ICONINFORMATION);
    return 0;
}
static void self_delete_later(void) {
    wchar_t cmd[MAX_PATH * 3]; swprintf(cmd, MAX_PATH * 3, L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"%s\"", g_self);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

/* ------------------------------------------------------------------ GUI */
enum { IDC_LOGO = 99, IDC_ROM = 100, IDC_ROM_BROWSE, IDC_DIR, IDC_DIR_BROWSE, IDC_DESKTOP, IDC_STARTMENU, IDC_INFO, IDC_PROGRESS, IDC_STATUS, IDC_GO, IDC_CLOSE };
static HFONT g_font, g_font_title, g_font_bold;
static HICON g_icon;
static int g_dpi = 96, g_busy, g_done_ok;
static HBRUSH g_white;
#define S(v) MulDiv((v), g_dpi, 96)

static HWND ctl(HWND p, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}
static int pick(HWND w, int folder, wchar_t *out) {
    IFileOpenDialog *d = NULL; int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return 0;
    FILEOPENDIALOGOPTIONS o; IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC f[] = { { L"Nintendo 64 ROM (*.z64, *.v64, *.n64, *.rom, *.bin)", L"*.z64;*.v64;*.n64;*.rom;*.bin" }, { L"All files", L"*.*" } };
        IFileOpenDialog_SetFileTypes(d, 2, f);
        IFileOpenDialog_SetTitle(d, L"Choose your WildWaters ROM");
    } else IFileOpenDialog_SetTitle(d, L"Choose a folder");
    if (SUCCEEDED(IFileOpenDialog_Show(d, w))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) { wcsncpy(out, p, MAX_PATH - 1); out[MAX_PATH - 1] = 0; CoTaskMemFree(p); ok = 1; }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
    return ok;
}
static void refresh_info(HWND w) {
    if (g_busy) return;
    wchar_t rom[MAX_PATH], dir[MAX_PATH], info[512] = L"";
    GetDlgItemTextW(w, IDC_ROM, rom, MAX_PATH); GetDlgItemTextW(w, IDC_DIR, dir, MAX_PATH); trim_path(rom); trim_path(dir);
    int have = dir[0] && existing_rom_ok(dir);
    if (rom[0]) {
        int r = rom_check_file(rom);
        if (r == ROM_OK) wcscpy(info, L"\u2714  WildWaters (prototype, Jun 15 1999) found.");
        else swprintf(info, 512, L"\u2716  %s", rom_error(r));
    } else if (have) wcscpy(info, L"WildWaters is already installed in that folder: leave the ROM box empty to only update the PC version.");
    else wcscpy(info, L"Choose your own WildWaters ROM: the Jun 15, 1999 prototype (.z64, .v64 or .n64).");
    SetDlgItemTextW(w, IDC_INFO, info);
    SetDlgItemTextW(w, IDC_GO, have ? (rom[0] ? L"Reinstall" : L"Update") : L"Install");
}
static void set_busy(HWND w, int b) {
    g_busy = b;
    int ids[] = { IDC_ROM, IDC_ROM_BROWSE, IDC_DIR, IDC_DIR_BROWSE, IDC_DESKTOP, IDC_STARTMENU, IDC_GO };
    for (int i = 0; i < 7; i++) EnableWindow(GetDlgItem(w, ids[i]), !b);
    SetDlgItemTextW(w, IDC_CLOSE, b ? L"Cancel" : L"Close");
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = w;
        HFONT f = g_font, fb = g_font_bold;
        ctl(w, L"STATIC", L"", SS_ICON, 20, 16, 48, 48, IDC_LOGO, f);
        SendDlgItemMessageW(w, IDC_LOGO, STM_SETICON, (WPARAM)g_icon, 0);
        ctl(w, L"STATIC", APP_NAME, 0, 84, 16, 456, 30, 0, g_font_title);
        ctl(w, L"STATIC", L"Setup: makes the PC version from your own WildWaters ROM.", 0, 84, 48, 456, 20, 0, f);
        ctl(w, L"STATIC", L"1.  Your ROM", 0, 20, 88, 300, 20, 0, fb);
        HWND e = ctl(w, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 20, 110, 442, 26, IDC_ROM, f);
        SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)L"WildWaters ROM file (.z64, .v64 or .n64)");
        ctl(w, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 466, 110, 74, 26, IDC_ROM_BROWSE, f);
        ctl(w, L"STATIC", L"2.  Install to", 0, 20, 150, 300, 20, 0, fb);
        ctl(w, L"EDIT", g_job.dir, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 20, 172, 442, 26, IDC_DIR, f);
        ctl(w, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 466, 172, 74, 26, IDC_DIR_BROWSE, f);
        ctl(w, L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 210, 170, 22, IDC_DESKTOP, f);
        ctl(w, L"BUTTON", L"Start menu shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 200, 210, 200, 22, IDC_STARTMENU, f);
        CheckDlgButton(w, IDC_DESKTOP, BST_CHECKED); CheckDlgButton(w, IDC_STARTMENU, BST_CHECKED);
        ctl(w, L"STATIC", L"", 0, 20, 242, 520, 40, IDC_INFO, f);
        ctl(w, PROGRESS_CLASSW, L"", PBS_SMOOTH, 20, 290, 520, 16, IDC_PROGRESS, f);
        SendDlgItemMessageW(w, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
        ctl(w, L"STATIC", L"Needs about 70 MB. Settings, saves and the ROM copy stay in the install folder.", SS_ENDELLIPSIS, 20, 312, 520, 20, IDC_STATUS, f);
        ctl(w, L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 340, 346, 96, 30, IDC_GO, fb);
        ctl(w, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 444, 346, 96, 30, IDC_CLOSE, f);
        DragAcceptFiles(w, TRUE);
        refresh_info(w);
        return 0;
    }
    case WM_DROPFILES: {   /* drop the ROM on the window */
        wchar_t p[MAX_PATH];
        if (!g_busy && DragQueryFileW((HDROP)wp, 0, p, MAX_PATH)) { SetDlgItemTextW(w, IDC_ROM, p); refresh_info(w); }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == GetDlgItem(w, IDC_INFO)) {
            wchar_t t[8]; GetWindowTextW((HWND)lp, t, 8);
            SetTextColor(dc, t[0] == 0x2716 ? RGB(190, 30, 30) : t[0] == 0x2714 ? RGB(20, 130, 40) : RGB(70, 70, 70));
        }
        return (LRESULT)g_white;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        wchar_t p[MAX_PATH];
        if ((id == IDC_ROM || id == IDC_DIR) && code == EN_CHANGE) { SetTimer(w, 1, 350, NULL); return 0; }
        if (id == IDC_ROM_BROWSE && pick(w, 0, p)) { SetDlgItemTextW(w, IDC_ROM, p); refresh_info(w); }
        if (id == IDC_DIR_BROWSE && pick(w, 1, p)) {
            wchar_t q[MAX_PATH * 2]; const wchar_t *leaf = PathFindFileNameW(p);
            if (_wcsicmp(leaf, APP_NAME) && !existing_rom_ok(p)) { path_join(q, p, APP_NAME); SetDlgItemTextW(w, IDC_DIR, q); } else SetDlgItemTextW(w, IDC_DIR, p);
            refresh_info(w);
        }
        if (id == IDC_GO) {
            if (g_done_ok) {
                wchar_t exe[MAX_PATH * 2]; path_join(exe, g_job.dir, EXE_NAME);
                ShellExecuteW(w, L"open", exe, NULL, g_job.dir, SW_SHOWNORMAL); DestroyWindow(w); return 0;
            }
            GetDlgItemTextW(w, IDC_ROM, g_job.rom, MAX_PATH); GetDlgItemTextW(w, IDC_DIR, g_job.dir, MAX_PATH);
            g_job.desktop = IsDlgButtonChecked(w, IDC_DESKTOP) == BST_CHECKED; g_job.startmenu = IsDlgButtonChecked(w, IDC_STARTMENU) == BST_CHECKED;
            g_cancel = 0; set_busy(w, 1); SendDlgItemMessageW(w, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            HANDLE t = CreateThread(NULL, 0, install_thread, NULL, 0, NULL); if (t) CloseHandle(t);
        }
        if (id == IDC_CLOSE || id == IDCANCEL) {
            if (g_busy) { if (MessageBoxW(w, L"Stop the installation?", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) InterlockedExchange(&g_cancel, 1); }
            else DestroyWindow(w);
        }
        return 0;
    }
    case WM_TIMER: KillTimer(w, 1); refresh_info(w); return 0;
    case WM_APP_PROGRESS: SendDlgItemMessageW(w, IDC_PROGRESS, PBM_SETPOS, wp, 0); return 0;
    case WM_APP_STATUS: SetDlgItemTextW(w, IDC_STATUS, (wchar_t *)lp); LocalFree((void *)lp); return 0;
    case WM_APP_DONE:
        set_busy(w, 0);
        if (wp == 0) {
            g_done_ok = 1; SetDlgItemTextW(w, IDC_GO, L"Play");
            SetDlgItemTextW(w, IDC_INFO, L"\u2714  All set. Use the desktop / Start menu shortcut to play. Press Esc in the game for its settings.");
            int ids[] = { IDC_ROM, IDC_ROM_BROWSE, IDC_DIR, IDC_DIR_BROWSE, IDC_DESKTOP, IDC_STARTMENU };
            for (int i = 0; i < 6; i++) EnableWindow(GetDlgItem(w, ids[i]), FALSE);
        } else SendDlgItemMessageW(w, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        return 0;
    case WM_CLOSE: if (g_busy) { SendMessageW(w, WM_COMMAND, IDC_CLOSE, 0); return 0; } break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* ------------------------------------------------------------------ entry */
static const wchar_t *arg_after(int argc, wchar_t **argv, const wchar_t *k) { for (int i = 1; i + 1 < argc; i++) if (!_wcsicmp(argv[i], k)) return argv[i + 1]; return NULL; }
static int has_arg(int argc, wchar_t **argv, const wchar_t *k) { for (int i = 1; i < argc; i++) if (!_wcsicmp(argv[i], k)) return 1; return 0; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    GetModuleFileNameW(NULL, g_self, MAX_PATH);
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_silent = has_arg(argc, argv, L"/S");
    if (g_silent) AttachConsole(ATTACH_PARENT_PROCESS);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (has_arg(argc, argv, L"/uninstall")) {
        wchar_t dir[MAX_PATH]; wcscpy(dir, g_self); PathRemoveFileSpecW(dir);
        wchar_t tmp[MAX_PATH], cp[MAX_PATH * 2]; GetTempPathW(MAX_PATH, tmp); swprintf(cp, MAX_PATH * 2, L"%sUninstall_" REG_KEY L"_%lu.exe", tmp, GetCurrentProcessId());
        if (!CopyFileW(g_self, cp, FALSE)) return uninstall_run(dir, !g_silent, has_arg(argc, argv, L"/delsaves"));
        wchar_t cl[MAX_PATH * 3]; swprintf(cl, MAX_PATH * 3, L"\"%s\" /uninstall2 \"%s\"%s%s", cp, dir, g_silent ? L" /S" : L"", has_arg(argc, argv, L"/delsaves") ? L" /delsaves" : L"");
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
        if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return uninstall_run(dir, !g_silent, has_arg(argc, argv, L"/delsaves"));
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    if (has_arg(argc, argv, L"/uninstall2")) {
        const wchar_t *dir = arg_after(argc, argv, L"/uninstall2");
        int r = dir ? uninstall_run(dir, !g_silent, has_arg(argc, argv, L"/delsaves")) : 1;
        self_delete_later();
        return r;
    }

    /* default install folder: the previous install, else %USERPROFILE%\Games\WildWaters PC */
    HKEY k; DWORD sz = sizeof g_job.dir, type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"InstallLocation", NULL, &type, (BYTE *)g_job.dir, &sz) != ERROR_SUCCESS || type != REG_SZ) g_job.dir[0] = 0;
        RegCloseKey(k);
    }
    if (!g_job.dir[0]) known_path(&FOLDERID_Profile, L"Games\\" APP_NAME, g_job.dir, MAX_PATH);

    if (g_silent) {
        const wchar_t *r = arg_after(argc, argv, L"/rom"), *d = arg_after(argc, argv, L"/dir");
        if (r) wcsncpy(g_job.rom, r, MAX_PATH - 1);
        if (d) wcsncpy(g_job.dir, d, MAX_PATH - 1);
        g_job.desktop = !has_arg(argc, argv, L"/nodesktop"); g_job.startmenu = !has_arg(argc, argv, L"/nostartmenu");
        install_thread(NULL);
        return g_result;
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc);
    NONCLIENTMETRICSW ncm = { sizeof ncm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_SEMIBOLD; g_font_bold = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(22); lf.lfWeight = FW_BOLD; g_font_title = CreateFontIndirectW(&lf);
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0);
    g_white = (HBRUSH)GetStockObject(WHITE_BRUSH);

    WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_white; wc.lpszClassName = L"WildWatersPCSetup"; wc.hIcon = g_icon;
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(560), S(396) }; AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    const wchar_t *shot = arg_after(argc, argv, L"/snapshot");   /* dev check: draw the window off-screen into a .bmp, never shown */
    HWND w = CreateWindowExW(shot ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, wc.lpszClassName, APP_NAME L" - Setup", style,
                             shot ? -10000 : (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, shot ? -10000 : (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    if (shot) {
        const wchar_t *rom = arg_after(argc, argv, L"/rom"), *dst = arg_after(argc, argv, L"/dir");
        if (dst) SetDlgItemTextW(w, IDC_DIR, dst);
        if (rom) SetDlgItemTextW(w, IDC_ROM, rom);
        refresh_info(w);
        ShowWindow(w, SW_SHOWNOACTIVATE); UpdateWindow(w);
        MSG msg; DWORD t0 = GetTickCount(); while (GetTickCount() - t0 < 600) { while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); Sleep(10); }
        HDC wdc = GetWindowDC(w), mdc = CreateCompatibleDC(wdc); HBITMAP bm = CreateCompatibleBitmap(wdc, ww, wh); SelectObject(mdc, bm);
        PrintWindow(w, mdc, 2);
        BITMAPINFOHEADER bi = { sizeof bi, ww, wh, 1, 24, BI_RGB }; DWORD stride = (ww * 3 + 3) & ~3u, bsz = stride * wh;
        uint8_t *px = (uint8_t *)malloc(bsz); GetDIBits(mdc, bm, 0, wh, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
        BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof bi + bsz, 0, 0, sizeof fh + sizeof bi };
        HANDLE f = CreateFileW(shot, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); DWORD wr;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &bi, sizeof bi, &wr, NULL); WriteFile(f, px, bsz, &wr, NULL); CloseHandle(f); }
        return 0;
    }
    ShowWindow(w, show); UpdateWindow(w);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    CoUninitialize();
    return 0;
}
