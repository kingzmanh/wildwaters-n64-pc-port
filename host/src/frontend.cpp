// The window build's front end: RecompFrontend's launcher and settings menus
// (recompui) and remappable keyboard/controller input (recompinput), on an SDL window
// rendered by RT64.

#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>

#include "nfd.h"

#include "librecomp/game.hpp"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "base/ui_launcher.h"
#include "util/file.h"

#include "ww.hpp"

// recompui's launcher shows the first entry (ui_launcher.cpp declares it extern).
std::vector<recomp::GameEntry> supported_games;
// The game window, which recompui also uses (ui_state.cpp declares it extern).
SDL_Window* window = nullptr;

namespace {
    std::vector<char> thumbnail;

    // A pad no database knows (a generic USB / DirectInput gamepad) is only a joystick to SDL, so the
    // game could not use it. Give it the common generic-gamepad layout: face buttons 1-4, shoulders 5-6,
    // triggers 7-8 (or select/start there on 8-button pads), select/start 9-10, stick clicks 11-12,
    // sticks on axes 0-3, D-pad on the hat. It works at once and can be rebound in Controls.
    void give_generic_mapping(int index) {
        if (SDL_IsGameController(index)) {
            return;
        }
        SDL_Joystick* js = SDL_JoystickOpen(index);
        if (js == nullptr) {
            return;
        }
        int buttons = SDL_JoystickNumButtons(js), axes = SDL_JoystickNumAxes(js), hats = SDL_JoystickNumHats(js);
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof(guid));
        const char* name = SDL_JoystickName(js);
        std::string map = std::string(guid) + "," + (name ? name : "Gamepad") + ",";
        auto add = [&](const char* key, const char* value) { map += key; map += ':'; map += value; map += ','; };
        if (buttons > 0) add("a", "b0");
        if (buttons > 1) add("b", "b1");
        if (buttons > 2) add("x", "b2");
        if (buttons > 3) add("y", "b3");
        if (buttons > 4) add("leftshoulder", "b4");
        if (buttons > 5) add("rightshoulder", "b5");
        if (buttons >= 10) {
            add("lefttrigger", "b6"); add("righttrigger", "b7"); add("back", "b8"); add("start", "b9");
            if (buttons > 10) add("leftstick", "b10");
            if (buttons > 11) add("rightstick", "b11");
        }
        else if (buttons >= 8) {
            add("back", "b6"); add("start", "b7");
        }
        else if (buttons > 6) {
            add("start", "b6");
        }
        if (axes > 1) { add("leftx", "a0"); add("lefty", "a1"); }
        if (axes > 3) { add("rightx", "a2"); add("righty", "a3"); }
        if (hats > 0) { add("dpup", "h0.1"); add("dpright", "h0.2"); add("dpdown", "h0.4"); add("dpleft", "h0.8"); }
        map += "platform:Windows,";
        SDL_JoystickClose(js);
        if (SDL_GameControllerAddMapping(map.c_str()) >= 0) {
            std::printf("[controller] generic layout for unknown pad: %s\n", map.c_str());
        }
    }

    std::mutex added_mutex;
    std::vector<int> added_pads;      // device indices plugged in since the last input poll

    int SDLCALL joystick_added_watch(void*, SDL_Event* event) {
        if (event->type == SDL_JOYDEVICEADDED) {
            std::lock_guard lock(added_mutex);
            added_pads.push_back(event->jdevice.which);
        }
        return 0;
    }

    void map_added_pads() {
        std::vector<int> pads;
        {
            std::lock_guard lock(added_mutex);
            pads.swap(added_pads);
        }
        for (int index : pads) {
            give_generic_mapping(index);
        }
    }

    void* create_gfx() {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        // Every pad SDL has a native driver for (USB and Bluetooth, with rumble): PlayStation 4/5,
        // Nintendo Switch Pro / Joy-Cons (a pair is one pad) / GameCube adapters, Steam, Stadia, Luna,
        // Shield; Xbox pads use XInput / Windows.Gaming.Input. Everything else goes by the controller
        // database (assets/controllerdb.txt), and pads in no database get a generic layout below.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
        for (const char* hint : { "SDL_JOYSTICK_HIDAPI_SWITCH", "SDL_JOYSTICK_HIDAPI_JOY_CONS", "SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS",
                                  "SDL_JOYSTICK_HIDAPI_GAMECUBE", "SDL_JOYSTICK_HIDAPI_STEAM", "SDL_JOYSTICK_HIDAPI_STADIA",
                                  "SDL_JOYSTICK_HIDAPI_LUNA", "SDL_JOYSTICK_HIDAPI_SHIELD", "SDL_JOYSTICK_HIDAPI_SWITCH_HOME_LED",
                                  "SDL_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED", "SDL_JOYSTICK_RAWINPUT", "SDL_XINPUT_ENABLED",
                                  "SDL_JOYSTICK_WGI" }) {
            SDL_SetHint(hint, "1");
        }
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
            std::fprintf(stderr, "[frontend] SDL_Init failed: %s\n", SDL_GetError());
        }
        const std::u8string db = recompui::file::get_asset_path("controllerdb.txt").u8string();
        int mapped = SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(db.c_str()));
        std::printf("[controller] %d mappings loaded\n", mapped);
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            give_generic_mapping(i);
        }
        SDL_AddEventWatch(joystick_added_watch, nullptr);
        // Which pads SDL sees, for controller reports.
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            const char* name = SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : SDL_JoystickNameForIndex(i);
            std::printf("[controller] %d: %s (%s)\n", i, name ? name : "?", SDL_IsGameController(i) ? "game controller" : "joystick only");
        }
        NFD_Init();
        return nullptr;
    }

    ultramodern::renderer::WindowHandle create_window(void*) {
        const int width = ww::frontend::window_width > 0 ? ww::frontend::window_width : 1600;
        const int height = ww::frontend::window_height > 0 ? ww::frontend::window_height : 900;
        uint32_t flags = SDL_WINDOW_RESIZABLE;
        int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
        // WW_TEST_OFFSCREEN=1 (unattended test runs): open far off-screen, without taking
        // focus or a taskbar button; tools/capture.ps1 grabs frames with PrintWindow.
        if (SDL_getenv("WW_TEST_OFFSCREEN") != nullptr) {
            SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
            flags |= SDL_WINDOW_SKIP_TASKBAR;
            x = 30000;
            y = 30000;
        }
        window = SDL_CreateWindow(ww::program_name, x, y, width, height, flags);
        if (window == nullptr) {
            std::fprintf(stderr, "[frontend] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return {};
        }
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        return ultramodern::renderer::WindowHandle{ info.info.win.window, GetCurrentThreadId() };
    }

    void update_gfx(void*) {
        recompinput::handle_events();
    }

    void init_launcher(recompui::LauncherMenu* menu) {
        const recomp::GameEntry& game = supported_games[0];
        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            game.game_id, game.mod_game_id, game.display_name, game.thumbnail_bytes);
        recompui::update_game_mod_id(game.mod_game_id);
        options->add_start_game_or_load_rom_option();
        options->add_setup_controls_option();
        options->add_settings_option();
        options->add_mods_option();
        options->add_exit_option();
    }

    // WW_TRACE_GFX=1: wraps RT64's context to log display lists per second and the time
    // RT64 spends in send_dl (diagnosing slow frames).
    class TimingContext : public ultramodern::renderer::RendererContext {
    public:
        std::unique_ptr<ultramodern::renderer::RendererContext> inner;
        uint32_t dls = 0, screens = 0;
        double dl_ms = 0, dl_max = 0, screen_ms = 0;
        uint64_t window_start = SDL_GetPerformanceCounter();
        bool profile = SDL_getenv("WW_PROFILE_GFX") != nullptr;
        bool trace = SDL_getenv("WW_TRACE_GFX") != nullptr;
        explicit TimingContext(std::unique_ptr<ultramodern::renderer::RendererContext> c) : inner(std::move(c)) {
            setup_result = inner->get_setup_result();
            chosen_api = inner->get_chosen_api();
        }
        static double ms(uint64_t a, uint64_t b) { return (double)(b - a) * 1000.0 / SDL_GetPerformanceFrequency(); }
        void report() {
            uint64_t now = SDL_GetPerformanceCounter();
            if (trace && ms(window_start, now) >= 1000.0) {
                std::printf("[gfx] VI %u: %u DLs (avg %.1f ms, max %.1f ms in send_dl), %u screens (%.1f ms)\n",
                    ww::current_vi(), dls, dls ? dl_ms / dls : 0.0, dl_max, screens, screen_ms);
                dls = screens = 0; dl_ms = dl_max = screen_ms = 0; window_start = now;
            }
        }
        bool valid() override { return inner->valid(); }
        ultramodern::renderer::SetupResult get_setup_result() const override { return inner->get_setup_result(); }
        ultramodern::renderer::GraphicsApi get_chosen_api() const override { return inner->get_chosen_api(); }
        bool update_config(const ultramodern::renderer::GraphicsConfig& a, const ultramodern::renderer::GraphicsConfig& b) override { return inner->update_config(a, b); }
        void enable_instant_present() override { inner->enable_instant_present(); }
        void send_dl(const OSTask* task) override {
            ww::frame_limit();
            uint64_t t0 = SDL_GetPerformanceCounter();
            if (profile) ww::profiler_enter();
            inner->send_dl(task);
            if (profile) ww::profiler_leave();
            double d = ms(t0, SDL_GetPerformanceCounter());
            dls++; dl_ms += d; if (d > dl_max) dl_max = d;
            report();
        }
        void send_dummy_workload(uint32_t fb) override { inner->send_dummy_workload(fb); }
        void update_screen() override {
            uint64_t t0 = SDL_GetPerformanceCounter();
            inner->update_screen();
            screens++; screen_ms += ms(t0, SDL_GetPerformanceCounter());
            report();
        }
        void shutdown() override { inner->shutdown(); }
        uint32_t get_display_framerate() const override { return inner->get_display_framerate(); }
        float get_resolution_scale() const override { return inner->get_resolution_scale(); }
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        auto ctx = recompui::renderer::create_render_context(rdram, window_handle,
            ultramodern::renderer::PresentationMode::PresentEarly,
            developer_mode || SDL_getenv("WW_DEV_MODE") != nullptr);
        // Always wrapped: it paces the game (frame_limit.cpp); tracing only with WW_TRACE_GFX.
        return std::make_unique<TimingContext>(std::move(ctx));
    }

    // General > Two-player mode. The frontend's multiplayer input (players assigned in Controls) is
    // used while the option is on and either players are assigned or a menu is open (so Controls shows
    // "Assign players"); otherwise single-player input, where every pad and the keyboard drive player 1.
    void apply_player_mode() {
        bool want = ww::two_player_mode() &&
            (recompinput::players::get_number_of_assigned_players() > 0 || recompui::is_context_capturing_input());
        if (want == recompinput::players::is_single_player_mode()) {
            if (want) {
                recompinput::players::set_player_count_range(1, 2);
            }
            recompinput::players::set_single_player_mode(!want);
        }
    }

    void poll_inputs() {
        map_added_pads();
        apply_player_mode();
        recompinput::poll_inputs();
    }

    bool get_port_input(int port, uint16_t* buttons, float* x, float* y) {
        bool ok = recompinput::profiles::get_n64_input(port, buttons, x, y);
        bool r = ww::script_input(port, ww::current_vi(), buttons, x, y) || ok;
        // WW_TRACE_INPUT=1: what port 1 reads, about once a second.
        static const bool trace = SDL_getenv("WW_TRACE_INPUT") != nullptr;
        static uint32_t last = 0;
        if (trace && port == 0 && ww::current_vi() - last >= 60) {
            last = ww::current_vi();
            std::printf("[input] VI %u buttons %04X x %.2f y %.2f menu_capture %d\n", last, *buttons, *x, *y,
                (int)recompinput::game_input_disabled());
        }
        return r;
    }

    void set_rumble(int port, bool on) {
        recompinput::set_rumble(port, on);
    }
}

ultramodern::input::connected_device_info_t ww::frontend::get_connected_device_info(int port) {
    if (port > 0 && ww::script_uses_port(port)) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };   // test script player
    }
    if (recompinput::players::is_single_player_mode()) {
        if (port == 0) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    if (recompinput::players::get_player_is_assigned(port)) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

void ww::frontend::init(recomp::GameEntry& game) {
    recompui::programconfig::set_program_name(ww::program_name);
    recompui::programconfig::set_program_id(ww::program_id);

    std::ifstream file(recompui::file::get_asset_path("thumbnail.png"), std::ios::binary);
    thumbnail.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    game.thumbnail_bytes = std::span<const char>(thumbnail);
    supported_games.push_back(game);

    recompui::register_launcher_init_callback(init_launcher);
    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompui::register_ui_exports();
    // Single player by default; General > Two-player mode switches to the frontend's multiplayer
    // input for the game's two-player mode (apply_player_mode).
    recompinput::players::set_single_player_mode(true);
    ww::init_config();
}

void ww::frontend::set_callbacks(recomp::Configuration& cfg) {
    cfg.renderer_callbacks.create_render_context = create_render_context;
    cfg.gfx_callbacks = { create_gfx, create_window, update_gfx };
    cfg.input_callbacks = { poll_inputs, get_port_input, set_rumble, ww::frontend::get_connected_device_info };
    cfg.error_handling_callbacks.message_box = recompui::message_box;
}
