// Keyboard and gamepad bindings: the [keyboard] and [gamepad] sections of f1ce_launcher.ini, shared by the game
// (host_sdl.cpp) and the launcher's KEYBOARD and GAMEPAD sections (launcher/input_page.cpp). Header only, standard
// C++ (no SDL): keys are SDL scancodes, gamepad inputs SDL button / axis numbers, as numbers.
//
// The bindings drive the emulated PS3 pad: what each pad control does in a race is the game's (steering is the left
// stick, the throttle the pressure of cross, circle backs out of menus, start pauses, the right stick turns the chase
// cameras); the TV camera tuning keys and F8 are the launcher's own (host::debug_keys).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace inputcfg {
// the pad controls, then the keyboard-only keys (DK_* bits of host.h, in order)
enum Control {
    UP, DOWN, LEFT, RIGHT, CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, START, SELECT,
    LS_LEFT, LS_RIGHT, LS_UP, LS_DOWN, RS_LEFT, RS_RIGHT, RS_UP, RS_DOWN,
    PAD_CONTROLS,
    TUNE_UP = PAD_CONTROLS, TUNE_DOWN, TUNE_LEFT, TUNE_RIGHT, TUNE_BACK, TUNE_FWD, TUNE_PITCH_UP, TUNE_PITCH_DOWN,
    TUNE_FOV_UP, TUNE_FOV_DOWN, TUNE_RESET, TUNE_PRINT, AI_TOGGLE,
    CONTROLS
};
struct Info { const char* key; const char* name; const char* help; };
inline const Info INFO[CONTROLS] = {
    {"dpad_up", "D-pad up", "Menus; the pad's d-pad."},
    {"dpad_down", "D-pad down", "Menus; the pad's d-pad."},
    {"dpad_left", "D-pad left", "Menus; the pad's d-pad (it also steers)."},
    {"dpad_right", "D-pad right", "Menus; the pad's d-pad (it also steers)."},
    {"cross", "Throttle / accept (cross)", "The pad's cross: accepts in menus; its pressure is the throttle in a race."},
    {"circle", "Back (circle)", "The pad's circle: backs out of menus."},
    {"square", "Square", "The pad's square (what it does in a race: the game's control settings)."},
    {"triangle", "Triangle", "The pad's triangle (what it does in a race: the game's control settings)."},
    {"l1", "L1", "The pad's L1."},
    {"r1", "R1", "The pad's R1."},
    {"l2", "L2", "The pad's L2 (pressure from an analog trigger)."},
    {"r2", "R2", "The pad's R2 (pressure from an analog trigger)."},
    {"l3", "L3", "Pressing the left stick."},
    {"r3", "R3", "Pressing the right stick."},
    {"start", "Start (pause)", "The pad's start: pauses the race."},
    {"select", "Select", "The pad's select."},
    {"steer_left", "Steer left", "The left stick to the left: steering."},
    {"steer_right", "Steer right", "The left stick to the right: steering."},
    {"lstick_up", "Left stick up", "The left stick up."},
    {"lstick_down", "Left stick down", "The left stick down."},
    {"look_left", "Look left", "The right stick to the left: turns the chase cameras (Chase cam look-around)."},
    {"look_right", "Look right", "The right stick to the right: turns the chase cameras."},
    {"look_up", "Look up", "The right stick up."},
    {"look_down", "Look down", "The right stick down."},
    {"tvcam_up", "TV camera up", "With Onboard TV camera on \"tune\": moves the camera up."},
    {"tvcam_down", "TV camera down", "With the TV camera tuning: moves it down."},
    {"tvcam_left", "TV camera left", "With the TV camera tuning: moves it left."},
    {"tvcam_right", "TV camera right", "With the TV camera tuning: moves it right."},
    {"tvcam_back", "TV camera back", "With the TV camera tuning: moves it back."},
    {"tvcam_forward", "TV camera forward", "With the TV camera tuning: moves it forward."},
    {"tvcam_tilt_up", "TV camera tilt up", "With the TV camera tuning: tilts it up."},
    {"tvcam_tilt_down", "TV camera tilt down", "With the TV camera tuning: tilts it down."},
    {"tvcam_fov_up", "TV camera wider", "With the TV camera tuning: a wider field of view."},
    {"tvcam_fov_down", "TV camera narrower", "With the TV camera tuning: a narrower field of view."},
    {"tvcam_reset", "TV camera reset", "With the TV camera tuning: back to the default position."},
    {"tvcam_print", "TV camera print", "With the TV camera tuning: writes its values to the log."},
    {"ai_toggle", "AI driving toggle", "With Developer F1CE_AI_DRIVE: the AI drives the player's car."},
};
constexpr int KEYS_PER_CONTROL = 2;

// gamepad inputs: 0 none, 1 + SDL_GamepadButton, AXIS + 2 * SDL_GamepadAxis (+1: the positive half; triggers: +1)
constexpr int NONE = 0, AXIS = 100;
enum : int { B_SOUTH = 1, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LSHOULDER, B_RSHOULDER,
             B_DPAD_UP, B_DPAD_DOWN, B_DPAD_LEFT, B_DPAD_RIGHT };
enum : int { A_LX_NEG = AXIS, A_LX_POS, A_LY_NEG, A_LY_POS, A_RX_NEG, A_RX_POS, A_RY_NEG, A_RY_POS, A_LT = AXIS + 9,
             A_RT = AXIS + 11 };
inline bool is_axis(int input) { return input >= AXIS; }

struct Config {
    int keys[CONTROLS][KEYS_PER_CONTROL] = {};   // SDL scancodes, 0: none
    int pad[PAD_CONTROLS] = {};                  // gamepad inputs
    // keyboard: steering on the stick moves to full lock / back to the centre in this many ms (0: at once)
    int steer_ms = 0, centre_ms = 0;
    // gamepad: percent
    int deadzone = 0;      // of the sticks
    int curve = 100;       // steering response: 100 linear, more: finer near the centre
    int trigger = 25;      // an axis presses a digital button beyond this
    bool invert_look = false;

    Config() { set_defaults(); }
    // as before the bindings (2026-11-12): arrows d-pad, Z/X/A/S cross/circle/square/triangle, Q/W L1/R1, 1/2 L2/R2,
    // Enter start, Backspace select, I/J/K/L right stick, numpad TV camera tuning, F8 AI; the pad as SDL maps it
    void set_defaults_keyboard() {
        for (auto& k : keys) k[0] = k[1] = 0;
        auto k1 = [&](int c, int sc) { keys[c][0] = sc; };
        k1(UP, 82); k1(DOWN, 81); k1(LEFT, 80); k1(RIGHT, 79);
        k1(CROSS, 29); k1(CIRCLE, 27); k1(SQUARE, 4); k1(TRIANGLE, 22);
        k1(L1, 20); k1(R1, 26); k1(L2, 30); k1(R2, 31); k1(START, 40); k1(SELECT, 42);
        k1(RS_LEFT, 13); k1(RS_RIGHT, 15); k1(RS_UP, 12); k1(RS_DOWN, 14);
        k1(TUNE_UP, 96); k1(TUNE_DOWN, 90); k1(TUNE_LEFT, 92); k1(TUNE_RIGHT, 94); k1(TUNE_BACK, 97); k1(TUNE_FWD, 91);
        k1(TUNE_PITCH_UP, 95); k1(TUNE_PITCH_DOWN, 89); k1(TUNE_FOV_UP, 87); k1(TUNE_FOV_DOWN, 86); k1(TUNE_RESET, 98);
        k1(TUNE_PRINT, 93); k1(AI_TOGGLE, 65);
        steer_ms = centre_ms = 0;
    }
    void set_defaults_gamepad() {
        const int d[PAD_CONTROLS] = {B_DPAD_UP, B_DPAD_DOWN, B_DPAD_LEFT, B_DPAD_RIGHT, B_SOUTH, B_EAST, B_WEST, B_NORTH,
                                     B_LSHOULDER, B_RSHOULDER, A_LT, A_RT, B_LSTICK, B_RSTICK, B_START, B_BACK,
                                     A_LX_NEG, A_LX_POS, A_LY_NEG, A_LY_POS, A_RX_NEG, A_RX_POS, A_RY_NEG, A_RY_POS};
        std::copy(d, d + PAD_CONTROLS, pad);
        deadzone = 0;
        curve = 100;
        trigger = 25;
        invert_look = false;
    }
    void set_defaults() {
        set_defaults_keyboard();
        set_defaults_gamepad();
    }
};

// get(section, key) returns the value or nullptr; keys "a,b" (scancodes), gamepad inputs as numbers
template <typename Get>
void load(Config& c, Get get) {
    auto num = [&](const char* s, const char* k, int& v) { if (const char* x = get(s, k); x && *x) v = atoi(x); };
    for (int i = 0; i < CONTROLS; i++)
        if (const char* x = get("keyboard", INFO[i].key)) {
            c.keys[i][0] = c.keys[i][1] = 0;
            sscanf(x, "%d,%d", &c.keys[i][0], &c.keys[i][1]);
        }
    for (int i = 0; i < PAD_CONTROLS; i++) num("gamepad", INFO[i].key, c.pad[i]);
    num("keyboard", "steer_ms", c.steer_ms);
    num("keyboard", "centre_ms", c.centre_ms);
    num("gamepad", "deadzone", c.deadzone);
    num("gamepad", "curve", c.curve);
    num("gamepad", "trigger", c.trigger);
    int inv = c.invert_look;
    num("gamepad", "invert_look", inv);
    c.invert_look = inv != 0;
    c.steer_ms = std::clamp(c.steer_ms, 0, 2000);
    c.centre_ms = std::clamp(c.centre_ms, 0, 2000);
    c.deadzone = std::clamp(c.deadzone, 0, 50);
    c.curve = std::clamp(c.curve, 100, 300);
    c.trigger = std::clamp(c.trigger, 5, 95);
}

// put(section, key, value)
template <typename Put>
void save(const Config& c, Put put) {
    for (int i = 0; i < CONTROLS; i++) {
        char b[32];
        snprintf(b, sizeof(b), "%d,%d", c.keys[i][0], c.keys[i][1]);
        put("keyboard", INFO[i].key, b);
    }
    for (int i = 0; i < PAD_CONTROLS; i++) put("gamepad", INFO[i].key, std::to_string(c.pad[i]));
    put("keyboard", "steer_ms", std::to_string(c.steer_ms));
    put("keyboard", "centre_ms", std::to_string(c.centre_ms));
    put("gamepad", "deadzone", std::to_string(c.deadzone));
    put("gamepad", "curve", std::to_string(c.curve));
    put("gamepad", "trigger", std::to_string(c.trigger));
    put("gamepad", "invert_look", c.invert_look ? "1" : "0");
}

// a stick or trigger value (-1..1, 0..1) after the dead zone, and for steering the response curve
inline float shape(float v, int deadzone, int curve) {
    const float dz = deadzone / 100.0f, a = std::abs(v);
    if (a <= dz) return 0;
    float t = (a - dz) / (1 - dz);
    if (curve > 100) t = std::pow(t, curve / 100.0f);
    return v < 0 ? -t : t;
}
}  // namespace inputcfg
