// KEYBOARD and GAMEPAD sections: each pad control (and on the keyboard the TV camera tuning keys and F8) with its
// keys or pad input. Select a row and press the key / pad input to bind it (Esc cancels); Delete or the pad's left
// face button clears it. Keyboard: two keys per control, the new one first; a key moved from another control leaves
// it. Gamepad: one input per control, also stepped with left / right; an analog input (a trigger, a stick) on a
// button gives its pressure, e.g. the throttle (cross) on the right trigger.
#include "input_page.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

#include "../runtime/input_config.h"
#include "ini.h"
#include "ui.h"

namespace input {
namespace {
using namespace inputcfg;
Config g_cfg;
bool g_kb_saved = false, g_pad_saved = false;   // the ini has the section / it was changed: written back
int g_index;

// the gamepad inputs a binding steps through with left / right
const int PAD_INPUTS[] = {NONE, B_SOUTH, B_EAST, B_WEST, B_NORTH, B_LSHOULDER, B_RSHOULDER, A_LT, A_RT, B_LSTICK,
                          B_RSTICK, B_START, B_BACK, B_DPAD_UP, B_DPAD_DOWN, B_DPAD_LEFT, B_DPAD_RIGHT, A_LX_NEG,
                          A_LX_POS, A_LY_NEG, A_LY_POS, A_RX_NEG, A_RX_POS, A_RY_NEG, A_RY_POS, B_GUIDE};
constexpr int PAD_INPUT_N = sizeof(PAD_INPUTS) / sizeof(PAD_INPUTS[0]);

std::string pad_input_name(int in) {
    switch (in) {
        case NONE: return "-";
        case B_SOUTH: return "A / Cross";
        case B_EAST: return "B / Circle";
        case B_WEST: return "X / Square";
        case B_NORTH: return "Y / Triangle";
        case B_BACK: return "Back / Select";
        case B_GUIDE: return "Guide / PS";
        case B_START: return "Start / Options";
        case B_LSTICK: return "Left stick press";
        case B_RSTICK: return "Right stick press";
        case B_LSHOULDER: return "LB / L1";
        case B_RSHOULDER: return "RB / R1";
        case B_DPAD_UP: return "D-pad up";
        case B_DPAD_DOWN: return "D-pad down";
        case B_DPAD_LEFT: return "D-pad left";
        case B_DPAD_RIGHT: return "D-pad right";
        case A_LX_NEG: return "Left stick left";
        case A_LX_POS: return "Left stick right";
        case A_LY_NEG: return "Left stick up";
        case A_LY_POS: return "Left stick down";
        case A_RX_NEG: return "Right stick left";
        case A_RX_POS: return "Right stick right";
        case A_RY_NEG: return "Right stick up";
        case A_RY_POS: return "Right stick down";
        case A_LT: return "LT / L2 (analog)";
        case A_RT: return "RT / R2 (analog)";
        default: return is_axis(in) ? "Axis " + std::to_string((in - AXIS) / 2) : "Button " + std::to_string(in - 1);
    }
}
std::string key_name(int sc) {
    const char* n = sc > 0 ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    return n && *n ? n : "Key " + std::to_string(sc);
}
std::string keys_label(const int* k) {
    if (!k[0] && !k[1]) return "-";
    if (!k[1]) return key_name(k[0]);
    if (!k[0]) return key_name(k[1]);
    return key_name(k[0]) + "  /  " + key_name(k[1]);
}
// the pad's left face button or Delete, on the focused row: clears its binding
bool clear_pressed(bool focused) {
    return focused && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false));
}

ui::RowOut row(const std::string& label, const std::string& value, bool arrows, const char* help, bool* focused = nullptr) {
    ImGui::PushID(g_index);
    const ui::RowOut r = ui::row_widget("r", label, value, g_index++, arrows, false, help, focused);
    ImGui::PopID();
    return r;
}
bool slider(const std::string& label, int& v, int lo, int hi, int step, const std::string& value, const char* help) {
    const ui::RowOut r = row(label, value, true, help);
    if (!r.delta) return false;
    v = std::clamp(v + r.delta * step, lo, hi);
    return true;
}
std::string ms_label(int ms) { return ms ? std::to_string(ms) + " ms" : "Instant"; }

void key_row(int c) {
    bool focused = false;
    const bool waiting = ui::capturing() && ui::capture_tag() == c;
    const ui::RowOut r = row(INFO[c].name, waiting ? "Press a key... (Esc cancels)" : keys_label(g_cfg.keys[c]), false,
                             INFO[c].help, &focused);
    if (r.activated)
        ui::start_capture(ui::Capture::KEY, c, [c](int sc) {
            if (!sc) return;
            for (int o = 0; o < CONTROLS; o++)   // (a key drives one control)
                for (int& k : g_cfg.keys[o])
                    if (k == sc && o != c) k = 0;
            int* k = g_cfg.keys[c];
            if (k[0] != sc) {
                k[1] = k[0];
                k[0] = sc;
            }
            g_kb_saved = true;
        });
    if (clear_pressed(focused)) {
        g_cfg.keys[c][0] = g_cfg.keys[c][1] = 0;
        g_kb_saved = true;
    }
}

void pad_row(int c) {
    bool focused = false;
    const bool waiting = ui::capturing() && ui::capture_tag() == c;
    const ui::RowOut r = row(INFO[c].name, waiting ? "Press a pad input... (Esc cancels)" : pad_input_name(g_cfg.pad[c]),
                             !waiting, INFO[c].help, &focused);
    if (r.delta) {
        int i = 0;
        for (int k = 0; k < PAD_INPUT_N; k++)
            if (PAD_INPUTS[k] == g_cfg.pad[c]) i = k;
        g_cfg.pad[c] = PAD_INPUTS[(i + r.delta + PAD_INPUT_N) % PAD_INPUT_N];
        g_pad_saved = true;
    }
    if (r.activated)
        ui::start_capture(ui::Capture::PAD, c, [c](int in) {
            if (!in) return;
            g_cfg.pad[c] = in;
            g_pad_saved = true;
        });
    if (clear_pressed(focused)) {
        g_cfg.pad[c] = NONE;
        g_pad_saved = true;
    }
}
}  // namespace

void load(const Ini& ini) {
    g_cfg = {};
    inputcfg::load(g_cfg, [&](const char* s, const char* k) -> const char* {
        const std::string* v = ini.get(s, k);
        return v ? v->c_str() : nullptr;
    });
    g_kb_saved = !ini.keys("keyboard").empty();
    g_pad_saved = !ini.keys("gamepad").empty();
}

void save(Ini& ini) {
    inputcfg::save(g_cfg, [&](const char* s, const char* k, const std::string& v) {
        if (!strcmp(s, "keyboard") ? g_kb_saved : g_pad_saved) ini.set(s, k, v);
    });
}

int keyboard_rows(int first) {
    g_index = first;
    ui::header_widget("Keyboard steering (Steer left / right below)");
    if (slider("Steering speed", g_cfg.steer_ms, 0, 1000, 50, ms_label(g_cfg.steer_ms),
               "Time from the centre to full lock while a steering key is held (Instant: at once, as a pad's d-pad)."))
        g_kb_saved = true;
    if (slider("Centring speed", g_cfg.centre_ms, 0, 1000, 50, ms_label(g_cfg.centre_ms),
               "Time back to the centre when the steering keys are let go."))
        g_kb_saved = true;
    ui::header_widget("Driving and pad buttons (select a row, then press the key; Delete clears)");
    for (int c : {LS_LEFT, LS_RIGHT, CROSS, SQUARE, TRIANGLE, CIRCLE, L1, R1, L2, R2, L3, R3, START, SELECT}) key_row(c);
    ui::header_widget("Menus (d-pad)");
    for (int c : {UP, DOWN, LEFT, RIGHT}) key_row(c);
    ui::header_widget("Cameras (right stick) and left stick");
    for (int c : {RS_LEFT, RS_RIGHT, RS_UP, RS_DOWN, LS_UP, LS_DOWN}) key_row(c);
    ui::header_widget("TV camera tuning (Onboard TV camera: tune)");
    for (int c = TUNE_UP; c <= TUNE_PRINT; c++) key_row(c);
    ui::header_widget("Other");
    key_row(AI_TOGGLE);
    if (row("Reset keyboard", "Defaults", false, "All keys back to the defaults (arrows, Z/X/A/S, Q/W, 1/2...).").activated) {
        g_cfg.set_defaults_keyboard();
        g_kb_saved = true;
    }
    return g_index;
}

int gamepad_rows(int first) {
    g_index = first;
    {
        std::string pads;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; i++) {
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            pads += (pads.empty() ? "" : ", ") + std::string(name ? name : "Gamepad");
        }
        SDL_free(ids);
        ui::header_widget(pads.empty() ? "Sticks and triggers (no gamepad connected)" : "Sticks and triggers: " + pads);
    }
    if (slider("Stick dead zone", g_cfg.deadzone, 0, 30, 1, std::to_string(g_cfg.deadzone) + "%",
               "Stick movement around the centre that does nothing (worn sticks that do not centre)."))
        g_pad_saved = true;
    {
        char b[16];
        snprintf(b, sizeof(b), g_cfg.curve == 100 ? "Linear" : "x%.1f", g_cfg.curve / 100.0f);
        if (slider("Steering response", g_cfg.curve, 100, 300, 10, b,
                   "Linear, or finer steering near the centre and quicker at the end of the stick's travel."))
            g_pad_saved = true;
    }
    if (slider("Trigger threshold", g_cfg.trigger, 5, 95, 5, std::to_string(g_cfg.trigger) + "%",
               "How far an analog input (trigger, stick) moves before it presses its button."))
        g_pad_saved = true;
    {
        const ui::RowOut r = row("Invert look up / down", g_cfg.invert_look ? "On" : "Off", true, "The right stick's up and down swapped.");
        if (r.delta || r.activated) {
            g_cfg.invert_look = !g_cfg.invert_look;
            g_pad_saved = true;
        }
    }
    ui::header_widget("Driving and pad buttons (select a row, then press the pad input; left face button clears)");
    for (int c : {LS_LEFT, LS_RIGHT, CROSS, SQUARE, TRIANGLE, CIRCLE, L1, R1, L2, R2, L3, R3, START, SELECT}) pad_row(c);
    ui::header_widget("Menus (d-pad)");
    for (int c : {UP, DOWN, LEFT, RIGHT}) pad_row(c);
    ui::header_widget("Cameras (right stick) and left stick");
    for (int c : {RS_LEFT, RS_RIGHT, RS_UP, RS_DOWN, LS_UP, LS_DOWN}) pad_row(c);
    if (row("Reset gamepad", "Defaults", false, "All pad inputs and the settings above back to the defaults.").activated) {
        g_cfg.set_defaults_gamepad();
        g_pad_saved = true;
    }
    return g_index;
}
}  // namespace input
