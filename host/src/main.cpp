// WildWaters PC: native Windows port of the WildWaters (Jun 15 1999) N64 prototype by
// Looking Glass Studios. The game's MIPS code is statically recompiled to C (N64Recomp);
// libultra is replaced by N64ModernRuntime, the display lists are drawn by RT64, and the
// audio microcode is recompiled by RSPRecomp.
//
// Usage: WildWatersPC [--rom <rom.z64>] [--seconds N] [--headless]
//   --rom PATH   the prototype ROM; needed once, it is then kept with the game's data
//   --seconds N  start the game directly and quit after N seconds (test runs)
//   --headless   no window, renderer, input or sound (test runs)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"

#include "ww.hpp"

#if defined(WW_RT64)
#include "nfd.h"
#include "recompui/program_config.h"
#include "util/file.h"
#endif

#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <csignal>

static uint8_t* crash_rdram = nullptr;

static void crash_write(const std::string& text) {
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
    wchar_t exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path log = (len > 0 && len < MAX_PATH) ? std::filesystem::path(exe).parent_path() / "crash.log"
                                                              : std::filesystem::path("crash.log");
    if (FILE* f = _wfopen(log.c_str(), L"a")) {
        std::fputs(text.c_str(), f);
        std::fputs("\n", f);
        std::fclose(f);
    }
}

static std::string stack_text(void* const* frames, USHORT count) {
    std::string out;
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    char line[512];
    for (USHORT i = 0; i < count; i++) {
        DWORD64 offset = 0;
        if (SymFromAddr(process, (DWORD64)frames[i], &offset, symbol)) {
            std::snprintf(line, sizeof(line), "  %s+0x%llx\n", symbol->Name, (unsigned long long)offset);
        } else {
            std::snprintf(line, sizeof(line), "  %p\n", frames[i]);
        }
        out += line;
    }
    return out;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    char head[256];
    std::snprintf(head, sizeof(head), "[host] exception 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress);
    std::string report = head;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t fault = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
        std::snprintf(head, sizeof(head), "[host] %s %p (N64 address 0x%08X)\n",
            info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)fault,
            crash_rdram ? (uint32_t)(fault - (uintptr_t)crash_rdram + 0x80000000u) : 0u);
        report += head;
    }
    void* frames[32];
    frames[0] = info->ExceptionRecord->ExceptionAddress;
    USHORT count = 1 + CaptureStackBackTrace(0, 31, frames + 1, nullptr);
    report += stack_text(frames, count);
    crash_write(report);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void abort_handler(int) {
    void* frames[40];
    USHORT count = CaptureStackBackTrace(1, 40, frames, nullptr);
    crash_write("[host] abort() (the console output just before gives the reason)\n" + stack_text(frames, count));
}

static void install_crash_handler() {
    SetUnhandledExceptionFilter(crash_handler);
    std::signal(SIGABRT, abort_handler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#else
static uint8_t* crash_rdram = nullptr;
static void install_crash_handler() {}
#endif

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
void ww_selftest(uint8_t* rdram, recomp_context* base); // ultra_extras.cpp, WW_SELFTEST=1
// libultra's stock aspMain audio microcode, recompiled by RSPRecomp (recomp/aspMain.toml).
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace {
    std::atomic<uint32_t> vi_count{0};
    bool headless = true;

    std::filesystem::path restore_dir;

    void on_init(uint8_t* rdram, recomp_context* ctx) {
        crash_rdram = rdram;
        ww::apply_restore_patches(restore_dir);
        ww_selftest(rdram, ctx);
        // osCicId as IPL3 leaves it (CIC-NUS-6102, the usual one for this era).
        constexpr int32_t osCicId = (int32_t)0x80000310;
        MEM_W(osCicId, 0) = 6102;
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            return aspMain;
        }
        std::fprintf(stderr, "[host] no RSP microcode for task type %u\n", (unsigned)task->t.type);
        return nullptr;
    }

    void null_queue_samples(int16_t* samples, size_t count) { ww::audio_dump(samples, count); }
    size_t null_frames_remaining() { return 0; }
    void null_set_frequency(uint32_t frequency) { ww::audio_dump_frequency(frequency); }
    void null_poll_input() {}
    bool null_get_input(int port, uint16_t* buttons, float* x, float* y) {
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return ww::script_input(port, vi_count.load(), buttons, x, y) || port == 0;
    }
    void null_set_rumble(int, bool) {}
    ultramodern::input::connected_device_info_t null_device_info(int port) {
        if (port == 0 || ww::script_uses_port(port)) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    ultramodern::renderer::WindowHandle null_create_window(void*) { return {}; }

    void vi_callback() { ++vi_count; }
    void message_box(const char* msg) { std::fprintf(stderr, "[host] %s\n", msg); }

    std::filesystem::path exe_directory(const char* argv0) {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(buffer).parent_path();
        }
#endif
        return std::filesystem::absolute(argv0).parent_path();
    }
}

uint32_t ww::current_vi() { return vi_count.load(); }

const char* ww::rom_error_text(recomp::RomValidationError error) {
    switch (error) {
    case recomp::RomValidationError::FailedToOpen: return "The file couldn't be opened.";
    case recomp::RomValidationError::NotARom: return "The file isn't an N64 ROM.";
    case recomp::RomValidationError::IncorrectRom:
    case recomp::RomValidationError::IncorrectVersion:
        return "This isn't the WildWaters Jun 15 1999 prototype (MD5 b867880c96c9e8d3130dde5526c95439).";
    default: return "The ROM couldn't be loaded.";
    }
}

int main(int argc, char** argv) {
#if defined(_WIN32) && defined(WW_RT64)
    // The window build has no console (a Windows program). WW_CONSOLE=1 opens one for the log.
    if (std::getenv("WW_CONSOLE") != nullptr && AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    install_crash_handler();
    std::filesystem::path rom_path;
    int seconds = 0;
#if defined(WW_RT64)
    headless = false;
#endif
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (argv[i][0] != '-' && rom_path.empty()) {
            rom_path = argv[i];
        }
    }
    // Test runs started from scripts can force headless mode without a flag.
    if (std::getenv("WW_HEADLESS") != nullptr) {
        headless = true;
    }

    std::filesystem::current_path(exe_directory(argv[0]));
    restore_dir = exe_directory(argv[0]) / "assets" / "restore";
#if defined(WW_RT64)
    if (!headless) {
        recompui::programconfig::set_program_id(ww::program_id);
        recomp::register_config_path(recompui::file::get_app_folder_path());
    } else
#endif
    {
        recomp::register_config_path(exe_directory(argv[0]) / "ww_data");
    }
    std::filesystem::create_directories(recomp::get_config_path());
    bool start_directly = headless || seconds > 0;

    recomp::GameEntry game{};
    game.rom_hash = ww::rom_hash;
    game.internal_name = std::string(20, '\0'); // the prototype's header has no name
    game.display_name = "WildWaters";
    game.game_id = ww::game_id;
    game.mod_game_id = "wildwaters";
    game.save_type = recomp::SaveType::AllowAll;
    game.is_enabled = true;
    game.entrypoint_address = (gpr)(int32_t)0x80008000u;
    game.entrypoint = recomp_entrypoint;
    game.on_init_callback = on_init;
#if defined(WW_RT64)
    if (!headless) {
        ww::frontend::init(game);
    }
#endif
    recomp::register_game(game);
    ww::register_overlays();

    if (!rom_path.empty()) {
        recomp::RomValidationError result = recomp::select_rom(rom_path, ww::game_id);
        if (result != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "[host] %s\n", ww::rom_error_text(result));
            return EXIT_FAILURE;
        }
    } else {
        recomp::check_all_stored_roms();
        if (start_directly && !recomp::is_rom_valid(ww::game_id)) {
            std::fprintf(stderr, "[host] No ROM yet: run once with --rom <path to the WildWaters ROM>.\n");
            return EXIT_FAILURE;
        }
    }

    std::vector<char*> runtime_argv{ argv[0] };
    if (start_directly) {
        runtime_argv.push_back((char*)"--game");
        runtime_argv.push_back((char*)"wildwaters");
    }

    recomp::Configuration cfg{};
    cfg.argc = (int)runtime_argv.size();
    cfg.argv = runtime_argv.data();
    cfg.project_version = recomp::Version{ 0, 1, 0 };
    cfg.rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.audio_callbacks = { null_queue_samples, null_frames_remaining, null_set_frequency };
    cfg.renderer_callbacks.create_render_context = ww::create_null_renderer;
    cfg.input_callbacks = { null_poll_input, null_get_input, null_set_rumble, null_device_info };
    cfg.gfx_callbacks = { nullptr, null_create_window, nullptr };
    cfg.events_callbacks = { vi_callback, nullptr };
    cfg.error_handling_callbacks = { message_box };
#if defined(WW_RT64)
    if (!headless) {
        cfg.audio_callbacks = { ww::audio::queue_samples, ww::audio::get_frames_remaining, ww::audio::set_frequency };
        ww::frontend::set_callbacks(cfg);
    }
#endif

    std::thread timer;
    if (seconds > 0) {
        timer = std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::printf("[host] %d seconds elapsed, %u VIs; quitting\n", seconds, vi_count.load());
            ultramodern::quit();
        });
    }

    recomp::start(cfg);

#if defined(WW_RT64)
    if (!headless) {
        NFD_Quit();
    }
#endif
    if (timer.joinable()) {
        timer.join();
    }
    return EXIT_SUCCESS;
}
