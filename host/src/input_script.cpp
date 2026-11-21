// Scripted controller input for unattended test runs.
//
// WW_INPUT_SCRIPT="120:START,200:SDOWN,260:A,400-900:A+SUP"
//   <vi>:<buttons>          press for 6 VIs starting at that VI
//   <vi>-<vi>:<buttons>     hold over that range
// Buttons: A B Z L R START DU DD DL DR CU CD CL CR, stick: SUP SDOWN SLEFT SRIGHT.
// The script drives port 1 (added to any real input); "P2+A" etc. drive controllers 2-4.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ww.hpp"

namespace {
    struct Step {
        uint32_t from, to;
        uint16_t buttons;
        float x, y;
        int port;        // 0 = controller 1; "P2" in the names = controller 2, "P3", "P4"
    };

    std::vector<Step> steps;
    bool loaded = false;

    void load() {
        loaded = true;
        const char* env = std::getenv("WW_INPUT_SCRIPT");
        if (env == nullptr) {
            return;
        }
        std::string text = env;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(',', pos);
            if (end == std::string::npos) end = text.size();
            std::string item = text.substr(pos, end - pos);
            pos = end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            Step s{};
            std::string range = item.substr(0, colon);
            size_t dash = range.find('-');
            s.from = (uint32_t)std::strtoul(range.c_str(), nullptr, 10);
            s.to = dash == std::string::npos ? s.from + 6 : (uint32_t)std::strtoul(range.c_str() + dash + 1, nullptr, 10);
            std::string names = item.substr(colon + 1);
            size_t p = 0;
            while (p < names.size()) {
                size_t plus = names.find('+', p);
                if (plus == std::string::npos) plus = names.size();
                std::string n = names.substr(p, plus - p);
                p = plus + 1;
                if (n.size() == 2 && n[0] == 'P' && n[1] >= '1' && n[1] <= '4') s.port = n[1] - '1';
                else if (n == "A") s.buttons |= 0x8000;
                else if (n == "B") s.buttons |= 0x4000;
                else if (n == "Z") s.buttons |= 0x2000;
                else if (n == "START") s.buttons |= 0x1000;
                else if (n == "DU") s.buttons |= 0x0800;
                else if (n == "DD") s.buttons |= 0x0400;
                else if (n == "DL") s.buttons |= 0x0200;
                else if (n == "DR") s.buttons |= 0x0100;
                else if (n == "L") s.buttons |= 0x0020;
                else if (n == "R") s.buttons |= 0x0010;
                else if (n == "CU") s.buttons |= 0x0008;
                else if (n == "CD") s.buttons |= 0x0004;
                else if (n == "CL") s.buttons |= 0x0002;
                else if (n == "CR") s.buttons |= 0x0001;
                else if (n == "SUP") s.y = 1.0f;
                else if (n == "SDOWN") s.y = -1.0f;
                else if (n == "SLEFT") s.x = -1.0f;
                else if (n == "SRIGHT") s.x = 1.0f;
            }
            steps.push_back(s);
        }
    }
}

namespace {
    std::atomic<uint32_t> last_activity{ 0 };
    uint16_t prev_buttons = 0;
    bool stick_was_neutral = true;

    // Player 1 activity for the attract demo: a newly pressed button or the stick leaving neutral.
    void note_activity(uint32_t vi, uint16_t buttons, float x, float y) {
        bool neutral = x * x + y * y < 0.25f;
        if ((buttons & ~prev_buttons) != 0 || (!neutral && stick_was_neutral)) {
            last_activity = vi;
        }
        prev_buttons = buttons;
        stick_was_neutral = neutral;
    }
}

uint32_t ww::last_activity_vi() { return last_activity.load(); }

bool ww::script_uses_port(int port) {
    if (!loaded) {
        load();
    }
    for (const Step& s : steps) {
        if (s.port == port) return true;
    }
    return false;
}

bool ww::script_input(int port, uint32_t vi, uint16_t* buttons, float* x, float* y) {
    if (!loaded) {
        load();
    }
    bool scripted = false;
    for (const Step& s : steps) {
        if (s.port != port) continue;
        scripted = true;
        if (vi >= s.from && vi < s.to) {
            *buttons |= s.buttons;
            if (s.x != 0.0f) *x = s.x;
            if (s.y != 0.0f) *y = s.y;
        }
    }
    if (port != 0) {
        return scripted;
    }
    note_activity(vi, *buttons, *x, *y);
    bool injected = ww::attract_input(vi, buttons, y);   // after the activity check: the demo's own presses
    return scripted || injected;
}
