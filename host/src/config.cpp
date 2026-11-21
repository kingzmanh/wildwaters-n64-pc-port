// The settings menus (RecompFrontend): General, Controls, Graphics (RT64: internal
// resolution, aspect ratio / widescreen, MSAA, frame rate...), Sound and Mods.

#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

#include "recompinput/input_types.h"
#include "recompui/config.h"
#include "util/file.h"

#include "ww.hpp"

namespace {
    void set_control_descriptions() {
        using recompinput::GameInput;
        using recompinput::set_game_input_description;
        const char* stick = "Steers the kayak, and moves through the menus.";
        set_game_input_description(GameInput::Y_AXIS_POS, stick);
        set_game_input_description(GameInput::Y_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_POS, stick);
        set_game_input_description(GameInput::A, "Paddles, and confirms in menus.");
        set_game_input_description(GameInput::B, "Back in menus.");
        set_game_input_description(GameInput::START, "Pauses the race.");
    }
}

bool ww::two_player_mode() {
    try {
        auto v = recompui::config::get_general_config().get_option_value("two_players");
        if (const uint32_t* m = std::get_if<uint32_t>(&v)) {
            return *m == 1;
        }
    }
    catch (const std::exception&) {
    }
    return false;
}

int ww::music_mode() {
    try {
        auto v = recompui::config::get_sound_config().get_option_value("music_v2");
        if (const uint32_t* m = std::get_if<uint32_t>(&v)) {
            return (int)*m;
        }
    }
    catch (const std::exception&) {
    }
    return 0;
}

void ww::init_config() {
    std::filesystem::path app_folder = recompui::file::get_app_folder_path();
    if (!app_folder.empty()) {
        std::filesystem::create_directories(app_folder);
    }
    recompui::config::GeneralTabOptions general_options{};
    recomp::config::Config& general = recompui::config::create_general_tab(general_options);
    // The game's MULTIPLAYER mode is two players with a split screen (the player count is the menu
    // choice + 1). Off: every pad and the keyboard drive player 1. On: the frontend's multiplayer
    // input, two players, assigned with Controls > Assign players.
    static const std::vector<recomp::config::ConfigOptionEnumOption> two_player_choices = {
        {0u, "Off", "Off"},
        {1u, "On", "On"},
    };
    general.add_enum_option("two_players", "Two-player mode",
        "Lets a second controller play the game's <recomp-color primary>MULTIPLAYER</recomp-color> mode "
        "(two players, split screen). After turning it on, open <recomp-color primary>Controls</recomp-color> and "
        "press <recomp-color primary>Assign players</recomp-color>, then a button on each controller. Until players "
        "are assigned, every controller still drives player 1.",
        two_player_choices, 0u);
    recompui::config::create_graphics_tab();
    set_control_descriptions();
    recompui::config::create_controls_tab();
    recomp::config::Config& sound = recompui::config::create_sound_tab();
    // "music_v2": a new id so the default (Original) applies to everyone; the first version
    // defaulted to the unused songs.
    static const std::vector<recomp::config::ConfigOptionEnumOption> music_choices = {
        {0u, "Original", "Original"},
        {1u, "UnusedSongs", "Unused Songs"},
        {2u, "OlderBuild", "Destruction Derby 64 Songs"},   // id kept: saved settings use it
    };
    sound.add_enum_option("music_v2", "Music",
        "<recomp-color primary>Original</recomp-color> is the prototype's own race music, a looping "
        "track as on the cartridge. "
        "<recomp-color primary>Unused Songs</recomp-color> plays the full songs that were left in the ROM "
        "but never used, one per course, with the instrument bank they were written for. "
        "<recomp-color primary>Destruction Derby 64 Songs</recomp-color> plays Destruction Derby 64's music, left "
        "on the development cartridge in the unused half of the ROM.",
        music_choices, 0u);
    recompui::config::create_mods_tab();
    recompui::config::finalize();

    // Frame rate: Original, set once per install. With Display/Manual, RT64 matches every draw
    // call between frames to interpolate them; the race scene draws ~22,000 triangles one by
    // one with ~27,000 G_MODIFYVTX for the water, and that matching took ~1.7 s per frame.
    std::filesystem::path marker = app_folder / "ww_framerate_v1";
    std::error_code ec;
    if (!app_folder.empty() && !std::filesystem::exists(marker, ec)) {
        recomp::config::Config& graphics = recompui::config::get_graphics_config();
        graphics.set_option_value(recompui::config::graphics::options::rr_option,
            (uint32_t)ultramodern::renderer::RefreshRate::Original);
        graphics.save_config();
        std::ofstream(marker) << "frame rate set to Original\n";
    }
}
