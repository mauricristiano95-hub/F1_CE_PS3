// Steering wheel settings: the [wheel] section of f1ce_launcher.ini, shared by the game (wheel_sdl.cpp) and the
// launcher's Steering Wheel tab (launcher/f1ce_launcher.cpp). Header only, standard C++ (the launcher builds on its own).
//
// Axes and buttons are SDL joystick indices (-1: none). A pedal is calibrated by its raw values at rest and fully
// pressed (any order, so no invert flag; combined pedals: throttle and brake on one axis, each with its own half), and
// the part of that travel the game uses (min..max percent; older inis: a dead zone at each end). With a separate pedal
// set (pedals_separate: its own USB device, e.g. Fanatec/MOZA/Simucube/Heusinkveld pedals) the three pedal axes are
// that device's; steering, buttons and force feedback stay on the wheel.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace wheelcfg {
// logical buttons (host::WheelButton order), the key suffixes of btn_<name>, and their names in the launcher
constexpr int BUTTONS = 16;
inline const char* const BUTTON_KEYS[BUTTONS] = {"cross", "square", "circle", "triangle", "shift_up", "shift_down",
                                                 "r2", "l2", "select", "start", "r3", "l3", "up", "down", "left", "right"};
// (what each does, measured on the controller's action flags 2026-11-11: the game's wheel table puts gear up/down,
// look back and camera on these G25 buttons; the d-pad works the in-race menus)
inline const char* const BUTTON_NAMES[BUTTONS] = {
    "Cross (accept)", "Square",       "Circle (back)", "Triangle",   "Gear up",       "Gear down",
    "Look back",      "Change camera", "Select",       "Start (pause)", "R3",          "L3",
    "D-pad up",       "D-pad down",   "D-pad left",    "D-pad right"};
constexpr int GEARS = 7;   // H-shifter buttons: reverse, 1st..6th (gear_r, gear_1..gear_6)

struct Pedal {
    int axis = -1;
    int rest = 32767, full = -32768;   // raw values
    int min = 2, max = 98;             // percent of the travel: released below min, fully pressed from max
    // the pedal's travel, 0 at rest .. 1 fully pressed
    float travel(int raw) const {
        if (axis < 0 || rest == full) return 0;
        return std::clamp(float(raw - rest) / float(full - rest), 0.0f, 1.0f) + 0.0f;   // (+0: no -0)
    }
    // what the game gets, 0 .. 1
    float value(int raw) const {
        const float lo = min / 100.0f, hi = max / 100.0f;
        return std::clamp((travel(raw) - lo) / std::max(hi - lo, 0.01f), 0.0f, 1.0f) + 0.0f;
    }
};
constexpr int PEDAL_MIN_SPAN = 10;   // max - min, percent

struct Config {
    bool enabled = true;
    std::string device;               // SDL joystick GUID; empty: the first wheel found
    int steer_axis = 0;
    bool steer_invert = false;
    int steer_center = 0;             // raw value with the wheel straight
    int range = 900;                  // degrees lock to lock, as set in the wheel's own driver / control panel
    int lock = 0;                     // degrees lock to lock in the game; 0: the game's choice (its range command)
    int steer_deadzone = 0;           // percent
    Pedal throttle{1}, brake{5}, clutch{6};
    bool pedals_separate = false;     // the pedals are a USB device of their own
    std::string pedals_device;        // its SDL joystick GUID; empty: the first one found (see pick)
    int buttons[BUTTONS];
    bool hat_dpad = true;             // the first hat is the d-pad
    int gears[GEARS];                 // H-shifter
    // force feedback, percent
    bool ffb = true;
    bool ffb_invert = false;
    int gain = 100, constant = 100, spring = 100, damper = 100, friction = 100;
    int min_force = 0;                // added to any constant force (wheels with a dead zone in their motor)
    int weight = 25;                  // a damper that always plays (steadies strong direct-drive wheels)
    bool soft_lock = true;            // pushes back beyond the game's steering lock
    int autocenter = -1;              // -1: the game's centring spring; 0..100: a fixed centring spring of this strength

    Config() {
        std::fill(buttons, buttons + BUTTONS, -1);
        std::fill(gears, gears + GEARS, -1);
    }
    // (F1 CE asks for the Driving Force Pro's 900 degrees when a race starts: 900 before that too)
    int lock_deg(int game_lock) const { return lock > 0 ? lock : game_lock > 0 ? game_lock : 900; }
    // the game's steering (-1 .. 1, not clamped) from the raw steering axis
    float steer(int raw) const {
        float x = float(raw - steer_center) / 32767.0f;
        if (steer_invert) x = -x;
        return x * float(range) / float(std::max(lock_deg(game_lock_seen), 40));
    }
    int game_lock_seen = 0;           // (runtime: the lock of the game's range command)
};

// the vendors' usual layouts (DirectInput order: X, Y, Z, Rx, Ry, Rz, slider 0, slider 1; axes and paddles as in
// Assetto Corsa's controller presets, and a Thrustmaster TX's own Assetto Corsa setup); the launcher's wheel profiles
// (launcher/wheels/*.ini) and Detect buttons do the rest
inline void defaults_for(Config& c, unsigned vid) {
    c.steer_axis = 0;
    c.throttle = {1};
    c.brake = {5};
    c.clutch = {6};
    std::fill(c.buttons, c.buttons + BUTTONS, -1);
    auto set = [&](int wb, int b) { c.buttons[wb] = b; };
    switch (vid) {
        case 0x046d:   // Logitech G29 / G923 (PlayStation layout)
            set(0, 0); set(1, 1); set(2, 2); set(3, 3); set(4, 4); set(5, 5); set(6, 6); set(7, 7);
            set(8, 8); set(9, 9); set(10, 10); set(11, 11);
            c.min_force = 12;
            break;
        case 0x044f:   // Thrustmaster T300 / TX / T248 / T500: throttle Rz, brake Y; right paddle 1, left 0
            c.throttle = {5};
            c.brake = {1};
            set(4, 1); set(5, 0); set(0, 2); set(1, 3); set(2, 4); set(3, 5); set(6, 6); set(7, 7);
            set(8, 8); set(9, 9); set(10, 10); set(11, 11);
            c.min_force = 2;
            break;
        default:       // Fanatec, MOZA, PXN and others
            set(4, 4); set(5, 5); set(0, 0); set(1, 1); set(2, 2); set(3, 3); set(9, 9); set(8, 8);
            break;
    }
}

// a wheel by its USB vendor (the rest: SDL's own wheel list, SDL_JOYSTICK_TYPE_WHEEL)
inline const char* vendor_name(unsigned vid) {
    switch (vid) {
        case 0x046d: return "Logitech";
        case 0x044f: return "Thrustmaster";
        case 0x0eb7: return "Fanatec";
        case 0x346e: return "MOZA";
        case 0x11ff: case 0x36e6: return "PXN";
        case 0x16d0: return "Simucube";
        case 0x3416: return "Cammus";
        default: return nullptr;
    }
}

// ------------------------------------------------------------------ the wheel and the pedal set among the joysticks
struct Joy {
    std::string guid;
    bool wheel_type = false;   // SDL calls it a wheel
    bool vendor = false;       // from a wheel vendor (vendor_name): their pedal sets too
    bool gamepad = false;      // SDL maps it as a gamepad
};
// the game's and the launcher's choice (indices into joys, -1: none). The wheel: the device GUID, else the first wheel
// (with a separate pedal set: not the pedals' device, and SDL's wheels before the vendors' other devices, since a
// Fanatec or MOZA pedal set has the vendor's USB id too). The pedal set: its GUID, else the first other joystick that
// is not a gamepad, a wheel vendor's first.
inline void pick(const Config& c, const std::vector<Joy>& joys, int& wheel, int& pedals) {
    const int n = (int)joys.size();
    wheel = pedals = -1;
    const bool sep = c.pedals_separate;
    auto first = [&](auto ok) {
        for (int i = 0; i < n; i++)
            if (ok(joys[i])) return i;
        return -1;
    };
    if (!c.device.empty()) wheel = first([&](const Joy& j) { return j.guid == c.device; });
    else if (!sep) wheel = first([](const Joy& j) { return j.wheel_type || j.vendor; });
    else {
        auto not_pedals = [&](const Joy& j) { return c.pedals_device.empty() || j.guid != c.pedals_device; };
        wheel = first([&](const Joy& j) { return j.wheel_type && not_pedals(j); });
        if (wheel < 0) wheel = first([&](const Joy& j) { return j.vendor && not_pedals(j); });
    }
    if (!sep) return;
    auto other = [&](int i) { return i != wheel && (wheel < 0 || joys[i].guid != joys[wheel].guid); };
    for (int i = 0; i < n && pedals < 0; i++)
        if (other(i) && (!c.pedals_device.empty() ? joys[i].guid == c.pedals_device : !joys[i].gamepad && joys[i].vendor))
            pedals = i;
    for (int i = 0; i < n && pedals < 0 && c.pedals_device.empty(); i++)
        if (other(i) && !joys[i].gamepad) pedals = i;
}

// ------------------------------------------------------------------ the ini section
using Section = std::map<std::string, std::string>;

inline Section read_section(const std::string& path, const char* name = "wheel") {
    Section s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    // (WritePrivateProfileString writes UTF-16 when the file starts with a BOM; the launcher's ini is ANSI)
    bool in = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            in = line == std::string("[") + name + "]";
            continue;
        }
        const size_t eq = line.find('=');
        if (in && eq != std::string::npos) s[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return s;
}

// get(key) returns the value or nullptr
template <typename Get>
void load(Config& c, Get get) {
    auto i = [&](const char* k, int& v) { if (const char* s = get(k); s && *s) v = atoi(s); };
    auto b = [&](const char* k, bool& v) { if (const char* s = get(k); s && *s) v = atoi(s) != 0; };
    auto pedal = [&](const char* n, Pedal& p) {
        char k[32];
        snprintf(k, sizeof(k), "%s_axis", n); i(k, p.axis);
        snprintf(k, sizeof(k), "%s_rest", n); i(k, p.rest);
        snprintf(k, sizeof(k), "%s_full", n); i(k, p.full);
        int dz = -1;
        snprintf(k, sizeof(k), "%s_deadzone", n); i(k, dz);
        if (dz >= 0) { p.min = dz; p.max = 100 - dz; }
        snprintf(k, sizeof(k), "%s_min", n); i(k, p.min);
        snprintf(k, sizeof(k), "%s_max", n); i(k, p.max);
        p.min = std::clamp(p.min, 0, 100 - PEDAL_MIN_SPAN);
        p.max = std::clamp(p.max, p.min + PEDAL_MIN_SPAN, 100);
    };
    b("enabled", c.enabled);
    if (const char* s = get("device")) c.device = s;
    i("steer_axis", c.steer_axis);
    b("steer_invert", c.steer_invert);
    i("steer_center", c.steer_center);
    i("range", c.range);
    i("lock", c.lock);
    i("steer_deadzone", c.steer_deadzone);
    pedal("throttle", c.throttle);
    pedal("brake", c.brake);
    pedal("clutch", c.clutch);
    b("pedals_separate", c.pedals_separate);
    if (const char* s = get("pedals_device")) c.pedals_device = s;
    for (int n = 0; n < BUTTONS; n++) {
        char k[32];
        snprintf(k, sizeof(k), "btn_%s", BUTTON_KEYS[n]);
        i(k, c.buttons[n]);
    }
    b("hat_dpad", c.hat_dpad);
    for (int n = 0; n < GEARS; n++) {
        char k[16];
        if (n == 0) snprintf(k, sizeof(k), "gear_r");
        else snprintf(k, sizeof(k), "gear_%d", n);
        i(k, c.gears[n]);
    }
    b("ffb", c.ffb);
    b("ffb_invert", c.ffb_invert);
    i("ffb_gain", c.gain);
    i("ffb_constant", c.constant);
    i("ffb_spring", c.spring);
    i("ffb_damper", c.damper);
    i("ffb_friction", c.friction);
    i("ffb_min_force", c.min_force);
    i("ffb_weight", c.weight);
    b("soft_lock", c.soft_lock);
    i("autocenter", c.autocenter);
    c.range = std::clamp(c.range, 40, 3600);
    if (c.lock) c.lock = std::clamp(c.lock, 40, 3600);
}

// put(key, value) stores one value
template <typename Put>
void save(const Config& c, Put put) {
    auto i = [&](const char* k, int v) { put(k, std::to_string(v)); };
    auto pedal = [&](const char* n, const Pedal& p) {
        char k[32];
        snprintf(k, sizeof(k), "%s_axis", n); i(k, p.axis);
        snprintf(k, sizeof(k), "%s_rest", n); i(k, p.rest);
        snprintf(k, sizeof(k), "%s_full", n); i(k, p.full);
        snprintf(k, sizeof(k), "%s_min", n); i(k, p.min);
        snprintf(k, sizeof(k), "%s_max", n); i(k, p.max);
    };
    i("enabled", c.enabled);
    put("device", c.device);
    i("steer_axis", c.steer_axis);
    i("steer_invert", c.steer_invert);
    i("steer_center", c.steer_center);
    i("range", c.range);
    i("lock", c.lock);
    i("steer_deadzone", c.steer_deadzone);
    pedal("throttle", c.throttle);
    pedal("brake", c.brake);
    pedal("clutch", c.clutch);
    i("pedals_separate", c.pedals_separate);
    put("pedals_device", c.pedals_device);
    for (int n = 0; n < BUTTONS; n++) {
        char k[32];
        snprintf(k, sizeof(k), "btn_%s", BUTTON_KEYS[n]);
        i(k, c.buttons[n]);
    }
    i("hat_dpad", c.hat_dpad);
    for (int n = 0; n < GEARS; n++) {
        char k[16];
        if (n == 0) snprintf(k, sizeof(k), "gear_r");
        else snprintf(k, sizeof(k), "gear_%d", n);
        i(k, c.gears[n]);
    }
    i("ffb", c.ffb);
    i("ffb_invert", c.ffb_invert);
    i("ffb_gain", c.gain);
    i("ffb_constant", c.constant);
    i("ffb_spring", c.spring);
    i("ffb_damper", c.damper);
    i("ffb_friction", c.friction);
    i("ffb_min_force", c.min_force);
    i("ffb_weight", c.weight);
    i("soft_lock", c.soft_lock);
    i("autocenter", c.autocenter);
}
}  // namespace wheelcfg
