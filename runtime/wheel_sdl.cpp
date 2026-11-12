// Steering wheel on SDL (joystick + haptic): input for the emulated G25 (hle_usbd.cpp) and its force feedback.
//
// Runs on the UI thread (host_sdl.cpp calls wheel_poll after each event pump): SDL's joystick state and DirectInput
// effects stay on one thread; the usbd thread only exchanges WheelInput / WheelForces under g_mutex.
// The wheel: the [wheel] device GUID, else the first joystick SDL calls a wheel or from a wheel vendor
// (wheelcfg::vendor_name). Settings: runtime/wheel_config.h (the launcher's Steering Wheel tab writes them).
// Measured on a Thrustmaster TX (2026-11-11): a positive constant force along +x turns the wheel towards negative axis
// values; only one process can own the effects (a second one waits in SDL_OpenHapticFromJoystick).
#include "host.h"
#include "kernel.h"
#include "wheel_config.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <vector>

namespace host {
const char* const WHEEL_BUTTON_KEYS[WHEEL_BUTTONS] = {
    wheelcfg::BUTTON_KEYS[0], wheelcfg::BUTTON_KEYS[1], wheelcfg::BUTTON_KEYS[2],  wheelcfg::BUTTON_KEYS[3],
    wheelcfg::BUTTON_KEYS[4], wheelcfg::BUTTON_KEYS[5], wheelcfg::BUTTON_KEYS[6],  wheelcfg::BUTTON_KEYS[7],
    wheelcfg::BUTTON_KEYS[8], wheelcfg::BUTTON_KEYS[9], wheelcfg::BUTTON_KEYS[10], wheelcfg::BUTTON_KEYS[11],
    wheelcfg::BUTTON_KEYS[12], wheelcfg::BUTTON_KEYS[13], wheelcfg::BUTTON_KEYS[14], wheelcfg::BUTTON_KEYS[15]};

namespace {
std::mutex g_mutex;
WheelInput g_input;
WheelForces g_forces;
bool g_ffb_on = false;   // the wheel's effects are open
std::atomic<int> g_game_lock{0};

wheelcfg::Config g_cfg;
SDL_Joystick* g_joy = nullptr;
SDL_Haptic* g_haptic = nullptr;
SDL_JoystickID g_joy_id = 0;
u64 g_next_scan = 0;
bool g_logged_none = false;

// the SDL effects: created on first use, updated when their parameters change
struct Effect {
    int id = -1;
    bool running = false;
    SDL_HapticEffect last{};
};
Effect g_constant, g_spring, g_damper, g_friction;

std::string ini_path() {
    if (const char* e = getenv("F1CE_WHEEL_INI")) return e;
    return "f1ce_launcher.ini";   // (the game runs in the launcher's folder)
}

bool is_wheel(SDL_JoystickID id) {
    return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || wheelcfg::vendor_name(SDL_GetJoystickVendorForID(id));
}

void close_wheel() {
    if (g_haptic) {
        SDL_StopHapticEffects(g_haptic);
        SDL_CloseHaptic(g_haptic);
    }
    if (g_joy) SDL_CloseJoystick(g_joy);
    g_haptic = nullptr;
    g_joy = nullptr;
    g_joy_id = 0;
    g_constant = g_spring = g_damper = g_friction = {};
    std::lock_guard lk(g_mutex);
    g_input = {};
    g_ffb_on = false;
}

void open_wheel() {
    const wheelcfg::Section ini = wheelcfg::read_section(ini_path());
    wheelcfg::Config cfg;
    wheelcfg::load(cfg, [&](const char* k) -> const char* {
        auto it = ini.find(k);
        return it == ini.end() ? nullptr : it->second.c_str();
    });
    if (!cfg.enabled) return;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    SDL_JoystickID pick = 0;
    for (int i = 0; i < n && !pick; i++) {
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof(guid));
        if (!cfg.device.empty() ? cfg.device == guid : is_wheel(ids[i])) pick = ids[i];
    }
    SDL_free(ids);
    if (!pick) {
        if (!g_logged_none) LOG_I("wheel: none found%s%s", cfg.device.empty() ? "" : " with GUID ", cfg.device.c_str());
        g_logged_none = true;
        return;
    }
    g_joy = SDL_OpenJoystick(pick);
    if (!g_joy) { LOG_W("wheel: %s", SDL_GetError()); return; }
    g_joy_id = pick;
    // the vendor's layout, then the saved settings over it
    g_cfg = {};
    wheelcfg::defaults_for(g_cfg, SDL_GetJoystickVendor(g_joy));
    wheelcfg::load(g_cfg, [&](const char* k) -> const char* {
        auto it = ini.find(k);
        return it == ini.end() ? nullptr : it->second.c_str();
    });
    g_haptic = g_cfg.ffb ? SDL_OpenHapticFromJoystick(g_joy) : nullptr;
    const u32 feat = g_haptic ? SDL_GetHapticFeatures(g_haptic) : 0;
    if (g_haptic && (feat & SDL_HAPTIC_GAIN)) SDL_SetHapticGain(g_haptic, std::clamp(g_cfg.gain, 0, 100));
    if (g_haptic && (feat & SDL_HAPTIC_AUTOCENTER)) SDL_SetHapticAutocenter(g_haptic, 0);   // (the game's spring instead)
    LOG_I("wheel: %s %s (%04x:%04x), %d axes, %d buttons, %d hats, force feedback %s (features %08x); range %d, lock %d, "
          "steering axis %d, pedals %d %d %d",
          wheelcfg::vendor_name(SDL_GetJoystickVendor(g_joy)) ? wheelcfg::vendor_name(SDL_GetJoystickVendor(g_joy)) : "",
          SDL_GetJoystickName(g_joy), SDL_GetJoystickVendor(g_joy), SDL_GetJoystickProduct(g_joy),
          SDL_GetNumJoystickAxes(g_joy), SDL_GetNumJoystickButtons(g_joy), SDL_GetNumJoystickHats(g_joy),
          g_haptic ? "on" : g_cfg.ffb ? "unavailable" : "off", feat, g_cfg.range, g_cfg.lock, g_cfg.steer_axis,
          g_cfg.throttle.axis, g_cfg.brake.axis, g_cfg.clutch.axis);
    {
        std::lock_guard lk(g_mutex);
        g_ffb_on = g_haptic != nullptr;
    }
    if (g_cfg.ffb && !g_haptic) LOG_W("wheel: no force feedback: %s", SDL_GetError());
    g_logged_none = false;
}

void read_input() {
    WheelInput w;
    w.connected = true;
    auto axis = [](int a) { return a >= 0 && a < SDL_GetNumJoystickAxes(g_joy) ? (int)SDL_GetJoystickAxis(g_joy, a) : 0; };
    auto button = [](int b) { return b >= 0 && b < SDL_GetNumJoystickButtons(g_joy) && SDL_GetJoystickButton(g_joy, b); };
    g_cfg.game_lock_seen = g_game_lock;
    float s = g_cfg.steer(axis(g_cfg.steer_axis));
    const float dz = g_cfg.steer_deadzone / 100.0f;
    if (dz > 0) s = std::fabs(s) <= dz ? 0 : (s - std::copysign(dz, s)) / (1 - dz);
    w.steer = std::clamp(s, -1.0f, 1.0f);
    w.throttle = g_cfg.throttle.value(axis(g_cfg.throttle.axis));
    w.brake = g_cfg.brake.value(axis(g_cfg.brake.axis));
    w.clutch = g_cfg.clutch.value(axis(g_cfg.clutch.axis));
    for (int i = 0; i < WHEEL_BUTTONS; i++)
        if (button(g_cfg.buttons[i])) w.buttons |= 1u << i;
    if (g_cfg.hat_dpad && SDL_GetNumJoystickHats(g_joy) > 0) {
        const u8 h = SDL_GetJoystickHat(g_joy, 0);
        if (h & SDL_HAT_UP) w.buttons |= WBTN_UP;
        if (h & SDL_HAT_DOWN) w.buttons |= WBTN_DOWN;
        if (h & SDL_HAT_LEFT) w.buttons |= WBTN_LEFT;
        if (h & SDL_HAT_RIGHT) w.buttons |= WBTN_RIGHT;
    }
    for (int g = 0; g < wheelcfg::GEARS; g++) {
        if (g_cfg.gears[g] < 0) continue;
        w.hshifter = true;
        if (button(g_cfg.gears[g])) w.gear = g == 0 ? -1 : g;
    }
    std::lock_guard lk(g_mutex);
    g_input = w;
}

// ------------------------------------------------------------------ force feedback
// runs (or updates) an effect; stops it when off
void put(Effect& e, bool on, const SDL_HapticEffect& want) {
    if (!g_haptic) return;
    if (!on) {
        if (e.running) SDL_StopHapticEffect(g_haptic, e.id);
        e.running = false;
        return;
    }
    if (e.id < 0) {
        e.id = SDL_CreateHapticEffect(g_haptic, &want);
        if (e.id < 0) {
            static int warned = 0;
            if (warned++ < 4) LOG_W("wheel: effect %d: %s", want.type, SDL_GetError());
            return;
        }
        e.last = want;
    } else if (memcmp(&e.last, &want, sizeof(want)) != 0) {
        SDL_UpdateHapticEffect(g_haptic, e.id, &want);
        e.last = want;
    }
    if (!e.running) {
        SDL_RunHapticEffect(g_haptic, e.id, 1);
        e.running = true;
    }
}

void apply_ffb(float steer_unclamped) {
    if (!g_haptic) return;
    WheelForces f;
    {
        std::lock_guard lk(g_mutex);
        f = g_forces;
    }
    const u32 feat = SDL_GetHapticFeatures(g_haptic);
    // raw axis units per unit of game steering, and the raw direction of positive steering
    const float raw_per = 32767.0f * float(g_cfg.lock_deg(g_game_lock)) / float(g_cfg.range);
    const float dir = (g_cfg.steer_invert ? -1.0f : 1.0f) * (g_cfg.ffb_invert ? -1.0f : 1.0f);
    const float gain = (feat & SDL_HAPTIC_GAIN) ? 1.0f : g_cfg.gain / 100.0f;   // (else SDL_SetHapticGain)
    auto q16 = [](float v) { return (Sint16)std::clamp((int)std::lround(v * 32767.0f), -32767, 32767); };
    auto u16 = [](float v) { return (Uint16)std::clamp((int)std::lround(v * 65535.0f), 0, 65535); };

    // constant: the game's, the minimum force, and the soft lock beyond the game's steering lock
    float c = f.constant * g_cfg.constant / 100.0f;
    if (std::fabs(c) > 0.002f) c = std::copysign(g_cfg.min_force / 100.0f + (1 - g_cfg.min_force / 100.0f) * std::fabs(c), c);
    if (g_cfg.soft_lock && std::fabs(steer_unclamped) > 1.0f)
        c -= std::copysign(std::min((std::fabs(steer_unclamped) - 1.0f) * 25.0f, 1.0f), steer_unclamped);
    c = std::clamp(c * gain, -1.0f, 1.0f);
    SDL_HapticEffect e{};
    e.type = SDL_HAPTIC_CONSTANT;
    e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    e.constant.direction.dir[0] = 1;
    e.constant.length = SDL_HAPTIC_INFINITY;
    e.constant.level = q16(-c * dir);   // (a positive level turns the wheel towards negative values)
    put(g_constant, (feat & SDL_HAPTIC_CONSTANT) && std::fabs(c) > 0.002f, e);
    static const bool log = getenv("F1CE_WHEEL_LOG") && atoi(getenv("F1CE_WHEEL_LOG"));
    if (log) {
        static u64 next = 0;
        if (time_usec() >= next) {
            next = time_usec() + 250000;
            LOG_I("wheel: %.2f s steer %+.3f (game %+.3f) game constant %+.3f -> %+.3f, spring %d k %.2f/%.2f sat %.2f%s, "
                  "damper %d, friction %d",
                  input_time(), steer_unclamped, wheel_input().steer, f.constant, c, f.spring, f.spring_left,
                  f.spring_right, f.spring_sat,
                  f.autocenter ? " (auto-centre)" : "", f.damper, f.friction);
        }
    }

    // spring: the game's, or the fixed centring spring of the settings in place of the wheel's default one
    if (f.autocenter && g_cfg.autocenter >= 0) {
        f.spring = g_cfg.autocenter > 0;
        f.spring_center = f.spring_deadband = 0;
        f.spring_left = f.spring_right = g_cfg.autocenter / 100.0f;
        f.spring_sat = 1;
    } else if (!f.spring && g_cfg.autocenter > 0) {
        f.spring = true;
        f.spring_left = f.spring_right = g_cfg.autocenter / 100.0f;
    }
    auto condition = [&](Uint16 type, float left, float right, float sat, float center, float deadband, float k_scale) {
        SDL_HapticEffect x{};
        x.type = type;
        x.condition.direction.type = SDL_HAPTIC_CARTESIAN;
        x.condition.direction.dir[0] = 1;
        x.condition.length = SDL_HAPTIC_INFINITY;
        if (dir < 0) std::swap(left, right);   // (positions mirror)
        x.condition.left_coeff[0] = q16(std::clamp(left * k_scale * gain, -1.0f, 1.0f));
        x.condition.right_coeff[0] = q16(std::clamp(right * k_scale * gain, -1.0f, 1.0f));
        x.condition.left_sat[0] = x.condition.right_sat[0] = u16(std::clamp(sat, 0.0f, 1.0f));
        x.condition.center[0] = q16(std::clamp((g_cfg.steer_center + center * raw_per * dir) / 32767.0f, -1.0f, 1.0f));
        x.condition.deadband[0] = u16(std::clamp(deadband * raw_per / 32767.0f, 0.0f, 1.0f));
        return x;
    };
    // (the game's spring stiffness is per unit of its steering: per raw unit it grows with range / lock)
    const float k_spring = g_cfg.spring / 100.0f * float(g_cfg.range) / float(g_cfg.lock_deg(g_game_lock));
    put(g_spring, (feat & SDL_HAPTIC_SPRING) && f.spring,
        condition(SDL_HAPTIC_SPRING, f.spring_left, f.spring_right, f.spring_sat, f.spring_center, f.spring_deadband, k_spring));
    // damper: the game's, and at least the wheel weight of the settings
    float dl = f.damper ? f.damper_left * g_cfg.damper / 100.0f : 0, dr = f.damper ? f.damper_right * g_cfg.damper / 100.0f : 0;
    const float w = g_cfg.weight / 100.0f;
    dl = std::max(dl, w);
    dr = std::max(dr, w);
    put(g_damper, (feat & SDL_HAPTIC_DAMPER) && (dl > 0 || dr > 0),
        condition(SDL_HAPTIC_DAMPER, dl, dr, f.damper ? std::max(f.damper_sat, w) : 1.0f, 0, 0, 1.0f));
    put(g_friction, (feat & SDL_HAPTIC_FRICTION) && f.friction && g_cfg.friction > 0,
        condition(SDL_HAPTIC_FRICTION, f.friction_coef, f.friction_coef, f.friction_sat, 0, 0, g_cfg.friction / 100.0f));
}
}  // namespace

// UI thread, after each event pump
void wheel_poll() {
    static const bool off = getenv("F1CE_WHEEL") && atoi(getenv("F1CE_WHEEL")) == 0;
    if (off) return;
    if (g_joy && !SDL_JoystickConnected(g_joy)) {
        LOG_I("wheel: unplugged");
        close_wheel();
    }
    if (!g_joy) {
        const u64 now = time_usec();
        if (now < g_next_scan) return;
        g_next_scan = now + 1000000;
        open_wheel();
        if (!g_joy) return;
    }
    read_input();   // (sets g_cfg.game_lock_seen)
    apply_ffb(g_cfg.steer(SDL_GetJoystickAxis(g_joy, g_cfg.steer_axis)));
}
void wheel_close() { close_wheel(); }
// the gamepad of this joystick is the wheel (host_sdl.cpp leaves it alone)
bool wheel_owns(SDL_JoystickID id) { return g_joy && id == g_joy_id; }

// Scripted wheel input for automated tests, F1CE_INPUT's clock and syntax: F1CE_WHEEL_INPUT="50:cross,55:throttle=1:60,
// 58:steer=-0.4:2,60:gear=2:5" (buttons by their [wheel] key names; axes steer/throttle/brake/clutch with a value;
// duration 0.2 s unless given). With a script the game sees a wheel even when none is plugged in.
namespace {
struct ScriptItem { double t, dur; int button; int axis; float value; };
const std::vector<ScriptItem>& wheel_script() {
    static const std::vector<ScriptItem> items = [] {
        std::vector<ScriptItem> v;
        const char* env = getenv("F1CE_WHEEL_INPUT");
        if (!env) return v;
        std::string s = env;
        for (size_t pos = 0; pos < s.size();) {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            const std::string item = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t c1 = item.find(':');
            if (c1 == std::string::npos) continue;
            ScriptItem it{atof(item.c_str()), 0.2, -1, -1, 0};
            std::string name = item.substr(c1 + 1);
            if (size_t c2 = name.find(':'); c2 != std::string::npos) {
                it.dur = atof(name.c_str() + c2 + 1);
                name.resize(c2);
            }
            if (size_t eq = name.find('='); eq != std::string::npos) {
                it.value = (float)atof(name.c_str() + eq + 1);
                name.resize(eq);
            }
            static const char* const axes[] = {"steer", "throttle", "brake", "clutch", "gear"};
            for (int a = 0; a < 5; a++)
                if (name == axes[a]) it.axis = a;
            for (int b = 0; b < WHEEL_BUTTONS; b++)
                if (name == WHEEL_BUTTON_KEYS[b]) it.button = b;
            if (it.axis >= 0 || it.button >= 0) v.push_back(it);
        }
        LOG_I("wheel: %zu scripted inputs", v.size());
        return v;
    }();
    return items;
}
}  // namespace

WheelInput wheel_input() {
    WheelInput w;
    {
        std::lock_guard lk(g_mutex);
        w = g_input;
    }
    if (wheel_script().empty()) return w;
    w.connected = true;
    const double now = input_time();
    for (const ScriptItem& it : wheel_script()) {
        if (now < it.t || now >= it.t + it.dur) continue;
        if (it.button >= 0) w.buttons |= 1u << it.button;
        switch (it.axis) {
            case 0: w.steer = std::clamp(it.value, -1.0f, 1.0f); break;
            case 1: w.throttle = it.value; break;
            case 2: w.brake = it.value; break;
            case 3: w.clutch = it.value; break;
            case 4: w.hshifter = true; w.gear = (s8)it.value; break;
        }
    }
    return w;
}
void wheel_forces(const WheelForces& f) {
    std::lock_guard lk(g_mutex);
    g_forces = f;
}
void wheel_lock_deg(int degrees) { g_game_lock = degrees; }
bool wheel_ffb() {
    if (!wheel_script().empty()) return true;   // (tests: the forces are logged without a wheel)
    std::lock_guard lk(g_mutex);
    return g_input.connected && g_ffb_on;
}
}  // namespace host
