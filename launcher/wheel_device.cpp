#include "wheel_device.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace wheeldev {
namespace {
// SDL's windows belong to the thread that created them: every SDL call of the wheel runs on this one thread
struct Shared {
    std::mutex m;
    std::condition_variable wake, done;   // the thread has work / finished a round
    unsigned wake_n = 0, want_n = 0;      // (want_n: Want changes)
    State st;
    Want want;
    std::vector<std::function<void()>> jobs;
};
Shared& g_sh = *new Shared;   // (never destroyed: the thread runs until the process ends)
bool g_started = false;

void post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        g_sh.jobs.push_back(std::move(job));
        g_sh.wake_n++;
    }
    g_sh.wake.notify_one();
}

// the thread's own
SDL_Joystick* w_joy = nullptr;
SDL_Haptic* w_haptic = nullptr;
bool w_haptic_sys = false;              // SDL_INIT_HAPTIC done
int w_test_ids[4] = {-1, -1, -1, -1};   // the test effects (constant, spring, sine)
std::string w_last_status;

void w_status(const std::string& s) {
    std::lock_guard<std::mutex> l(g_sh.m);
    g_sh.st.status = w_last_status = s;
    g_sh.st.status_gen++;
}
bool is_wheel(SDL_JoystickID id) {
    return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || wheelcfg::vendor_name(SDL_GetJoystickVendorForID(id));
}
std::string guid_of(SDL_JoystickID id) {
    char g[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), g, sizeof(g));
    return g;
}

void w_close_haptic() {
    if (w_haptic) {
        SDL_StopHapticEffects(w_haptic);
        SDL_CloseHaptic(w_haptic);
    }
    w_haptic = nullptr;
    std::fill(w_test_ids, w_test_ids + 4, -1);
}
void w_close() {
    w_close_haptic();
    if (w_joy) SDL_CloseJoystick(w_joy);
    w_joy = nullptr;
    std::lock_guard<std::mutex> l(g_sh.m);
    g_sh.st.open = false;
    g_sh.st.guid.clear();
    g_sh.st.open_gen++;
}

std::string w_label(const char* vendor, const char* name) {
    std::string label = name ? name : "Joystick";
    if (vendor && label.find(vendor) == std::string::npos) label = std::string(vendor) + " " + label;
    return label;
}
void w_describe() {
    char b[300];
    snprintf(b, sizeof(b), "%s  -  %d axes, %d buttons, %d hats%s",
             w_label(wheelcfg::vendor_name(SDL_GetJoystickVendor(w_joy)), SDL_GetJoystickName(w_joy)).c_str(),
             SDL_GetNumJoystickAxes(w_joy), SDL_GetNumJoystickButtons(w_joy), SDL_GetNumJoystickHats(w_joy),
             !w_haptic_sys ? "" : SDL_IsJoystickHaptic(w_joy) ? ", force feedback yes" : ", force feedback no");
    w_status(b);
}

// the device list, when it changed
void w_list() {
    std::vector<Device> devs;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < n; i++) {
        std::string label = w_label(wheelcfg::vendor_name(SDL_GetJoystickVendorForID(ids[i])), SDL_GetJoystickNameForID(ids[i]));
        const bool wheel = is_wheel(ids[i]);
        if (!wheel) label += " (not a wheel)";
        devs.push_back({guid_of(ids[i]), label, wheel});
    }
    SDL_free(ids);
    std::lock_guard<std::mutex> l(g_sh.m);
    auto same = [](const Device& a, const Device& b) { return a.guid == b.guid && a.label == b.label; };
    if (devs.size() == g_sh.st.devices.size() && std::equal(devs.begin(), devs.end(), g_sh.st.devices.begin(), same))
        return;
    g_sh.st.devices = std::move(devs);
    g_sh.st.devices_gen++;
}

// opens the wanted wheel (or the first one), as the game picks it
void w_open(const Want& want) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    SDL_JoystickID pick = 0;
    for (int i = 0; i < n && !pick; i++)
        if (want.guid.empty() ? is_wheel(ids[i]) : guid_of(ids[i]) == want.guid) pick = ids[i];
    SDL_free(ids);
    if (pick && w_joy && SDL_GetJoystickID(w_joy) == pick) return;
    if (w_joy) w_close();
    if (!pick) {
        const std::string s = want.guid.empty() ? "No wheel found. Connect it (and install its driver): it shows up here."
                                                : "The selected wheel is not connected.";
        if (s != w_last_status) w_status(s);
        return;
    }
    w_joy = SDL_OpenJoystick(pick);
    if (!w_joy) return;
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        State& st = g_sh.st;
        st.open = true;
        st.guid = guid_of(pick);
        st.name = w_label(wheelcfg::vendor_name(SDL_GetJoystickVendor(w_joy)), SDL_GetJoystickName(w_joy));
        st.vid = SDL_GetJoystickVendor(w_joy);
        st.pid = SDL_GetJoystickProduct(w_joy);
        st.axes = SDL_GetNumJoystickAxes(w_joy);
        st.buttons = SDL_GetNumJoystickButtons(w_joy);
        st.hats = SDL_GetNumJoystickHats(w_joy);
        st.open_gen++;
    }
    w_describe();
}

// effects need the haptic device, opened on the first test (only one process can own it)
SDL_Haptic* w_open_haptic(int gain) {
    if (!w_haptic_sys) w_haptic_sys = SDL_InitSubSystem(SDL_INIT_HAPTIC);
    if (!w_haptic && w_joy) w_haptic = SDL_OpenHapticFromJoystick(w_joy);
    if (w_haptic && (SDL_GetHapticFeatures(w_haptic) & SDL_HAPTIC_GAIN)) SDL_SetHapticGain(w_haptic, gain);
    return w_haptic;
}

void w_test(const wheelcfg::Config& cfg, int t) {
    SDL_Haptic* h = w_open_haptic(cfg.gain);
    if (!h) {
        w_status("This wheel has no force feedback SDL can use (or another program holds it).");
        return;
    }
    SDL_StopHapticEffects(h);
    if (t == 4) return;
    const float dir = (cfg.steer_invert ? -1.0f : 1.0f) * (cfg.ffb_invert ? -1.0f : 1.0f);
    const float gain = (SDL_GetHapticFeatures(h) & SDL_HAPTIC_GAIN) ? 1.0f : cfg.gain / 100.0f;
    SDL_HapticEffect e{};
    if (t <= 1) {   // (a positive level turns the wheel towards negative axis values)
        e.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e.constant.direction.dir[0] = 1;
        e.constant.length = 1200;
        e.constant.level = (Sint16)std::lround((t == 0 ? 1 : -1) * dir * 0.45f * cfg.constant / 100.0f * gain * 32767);
    } else if (t == 2) {
        e.type = SDL_HAPTIC_SPRING;
        e.condition.direction.type = SDL_HAPTIC_CARTESIAN;
        e.condition.direction.dir[0] = 1;
        e.condition.length = 3000;
        const Sint16 k = (Sint16)std::clamp((int)std::lround(0.6f * cfg.spring / 100.0f * gain * 32767), 0, 32767);
        e.condition.left_coeff[0] = e.condition.right_coeff[0] = k;
        e.condition.left_sat[0] = e.condition.right_sat[0] = 0xffff;
        e.condition.center[0] = (Sint16)cfg.steer_center;
    } else {
        e.type = SDL_HAPTIC_SINE;
        e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
        e.periodic.direction.dir[0] = 1;
        e.periodic.length = 2000;
        e.periodic.period = 70;
        e.periodic.magnitude = (Sint16)std::lround(0.25f * cfg.constant / 100.0f * gain * 32767);
    }
    const int slot = t <= 1 ? 0 : t - 1;
    // (a slot keeps its type: updated in place; a destroyed effect created again at once did not play on a TX)
    if (w_test_ids[slot] >= 0 && !SDL_UpdateHapticEffect(h, w_test_ids[slot], &e)) {
        SDL_DestroyHapticEffect(h, w_test_ids[slot]);
        w_test_ids[slot] = -1;
    }
    if (w_test_ids[slot] < 0) w_test_ids[slot] = SDL_CreateHapticEffect(h, &e);
    if (w_test_ids[slot] < 0) {
        w_status(std::string("The wheel refused the effect: ") + SDL_GetError());
        return;
    }
    SDL_RunHapticEffect(h, w_test_ids[slot], 1);
}

void worker() {
    const bool ok = SDL_InitSubSystem(SDL_INIT_JOYSTICK);   // (a reference: the window's thread started it)
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        g_sh.st.ready = true;
        g_sh.st.sdl = ok;
    }
    g_sh.done.notify_all();
    if (!ok) {
        w_status(std::string("The wheel support could not start: ") + SDL_GetError());
        return;
    }
    Uint64 next_scan = 0;
    unsigned seen = 0, want_seen = ~0u;
    for (;;) {
        Want want;
        std::vector<std::function<void()>> jobs;
        bool changed;
        {
            std::lock_guard<std::mutex> l(g_sh.m);
            want = g_sh.want;
            jobs.swap(g_sh.jobs);
            seen = g_sh.wake_n;
            changed = want_seen != g_sh.want_n;
            want_seen = g_sh.want_n;
        }
        if (w_joy && !SDL_JoystickConnected(w_joy)) w_close();
        if (SDL_GetTicks() >= next_scan || changed) {   // plugging and unplugging, another wheel picked
            next_scan = SDL_GetTicks() + 1000;
            w_list();
            if (want.open) w_open(want);
        }
        if (!want.open && w_joy) w_close();
        if (!want.haptic && w_haptic) w_close_haptic();
        for (auto& j : jobs) j();
        if (w_joy && !w_haptic_sys) {   // (the slowest part last: the wheel's input shows first)
            w_haptic_sys = SDL_InitSubSystem(SDL_INIT_HAPTIC);
            w_describe();
        }
        {
            std::lock_guard<std::mutex> l(g_sh.m);
            State& st = g_sh.st;
            st.haptic = w_haptic != nullptr;
            st.pressed[0] = st.pressed[1] = 0;
            st.hat = 0;
            if (w_joy) {
                for (int a = 0; a < AXES; a++) st.axis[a] = a < st.axes ? SDL_GetJoystickAxis(w_joy, a) : 0;
                for (int b = 0; b < std::min(st.buttons, 64); b++)
                    if (SDL_GetJoystickButton(w_joy, b)) st.pressed[b / 32] |= 1u << (b % 32);
                if (st.hats > 0) st.hat = SDL_GetJoystickHat(w_joy, 0);
            }
        }
        g_sh.done.notify_all();
        std::unique_lock<std::mutex> l(g_sh.m);
        g_sh.wake.wait_for(l, std::chrono::milliseconds(want.fast ? 8 : 250), [&] { return g_sh.wake_n != seen; });
    }
}
}  // namespace

void start(const std::string& guid) {
    if (g_started) return;
    g_started = true;
    want([&](Want& w) { w.guid = guid; });
    std::thread(worker).detach();
}

void want(const std::function<void(Want&)>& change) {
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        change(g_sh.want);
        g_sh.want_n++;
        g_sh.wake_n++;
    }
    g_sh.wake.notify_one();
}

State state() {
    std::lock_guard<std::mutex> l(g_sh.m);
    return g_sh.st;
}

void close_and_wait() {
    if (!g_started) return;
    want([](Want& w) {
        w.open = false;
        w.haptic = false;
    });
    // (the thread may still be opening the wheel: at most a few seconds)
    std::unique_lock<std::mutex> l(g_sh.m);
    g_sh.done.wait_for(l, std::chrono::seconds(3), [] { return g_sh.st.ready && !g_sh.st.open && !g_sh.st.haptic; });
}

void test(const wheelcfg::Config& cfg, int t) {
    post([cfg, t] { w_test(cfg, t); });
}
void set_gain(int gain) {
    post([gain] {
        if (w_haptic && (SDL_GetHapticFeatures(w_haptic) & SDL_HAPTIC_GAIN)) SDL_SetHapticGain(w_haptic, gain);
    });
}
}  // namespace wheeldev
