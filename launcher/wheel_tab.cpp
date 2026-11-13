// Steering Wheel tab: picks and sets up the wheel the game drives as a Logitech G25 (runtime/hle_usbd.cpp,
// runtime/wheel_sdl.cpp), after the Thrustmaster control panel: live steering, pedal and button views, axis and button
// assignment by pressing them, the wheel's rotation and the game's steering lock, and the force-feedback gains with
// test effects. Settings: the [wheel] section of f1ce_launcher.ini (runtime/wheel_config.h).
#include "wheel_tab.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../runtime/wheel_config.h"

namespace wheeltab {
namespace {
enum : int {
    ID_DEVICE = 400, ID_ENABLED, ID_STATUS, ID_STEER_VIEW, ID_STEER_AXIS, ID_STEER_INV, ID_CENTER, ID_RANGE, ID_RANGE_LBL,
    ID_LOCK, ID_SDZ, ID_SDZ_LBL, ID_PEDAL_VIEW, ID_BTN_VIEW, ID_FFB, ID_FFB_INV, ID_SOFT_LOCK, ID_AUTOC, ID_AUTOC_STR,
    ID_AUTOC_LBL, ID_DEFAULTS,
    ID_PEDAL_AXIS = 450, ID_PEDAL_CAL = 460, ID_PEDAL_RANGE = 463, ID_PEDAL_RANGE_LBL = 466, ID_MAP = 470, ID_GEAR = 500,
    ID_GAIN = 520, ID_GAIN_LBL = 540, ID_TEST = 560,
    ID_PROFILE = 570, ID_PROF_LOAD, ID_PROF_SAVE, ID_PROF_DEL, ID_LAST,
};
constexpr int AXES = 12, BUTTON_CHOICES = 40;
const wchar_t* const PEDAL_NAMES[3] = {L"Gas", L"Brake", L"Clutch"};
const int LOCKS[] = {0, 180, 240, 270, 360, 420, 450, 540, 720, 900, 1080};
// the force-feedback sliders: label, Config member, maximum
struct Gain { const wchar_t* label; int wheelcfg::Config::*v; int max; const wchar_t* tip; };
const Gain GAINS[] = {
    {L"Overall strength", &wheelcfg::Config::gain, 100, L"All forces (the wheel's own gain when it has one)."},
    {L"Road and tyre forces", &wheelcfg::Config::constant, 150,
     L"The game's constant force: the tyres' self-aligning pull, kerbs, bumps and impacts."},
    {L"Centring spring", &wheelcfg::Config::spring, 150, L"The game's centring spring, stiffer with speed."},
    {L"Damper", &wheelcfg::Config::damper, 150, L"The game's damper effects (if any)."},
    {L"Wheel weight", &wheelcfg::Config::weight, 100,
     L"A damper that always plays: steadies strong wheels when nobody holds them."},
    {L"Minimum force", &wheelcfg::Config::min_force, 30,
     L"Added to every constant force, for wheels whose motor does not move with small forces."},
};
constexpr int GAIN_N = sizeof(GAINS) / sizeof(GAINS[0]);
const wchar_t* const TESTS[] = {L"Push left", L"Push right", L"Spring", L"Bumpy road", L"Stop"};

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ------------------------------------------------------------------ the wheel (SDL, on a thread of its own)
// DirectInput takes its time (2026-11-12, TX: joystick init 0.2 s, haptic init 0.6 s, opening the wheel 0.5 s), so
// every SDL call runs on this thread, started with the launcher: the tab says what it wants (Want), reads a copy of
// the wheel's state (State) at each repaint and queues its force-feedback commands (jobs). SDL's windows belong to
// the thread that created them, hence one thread for all of it.
struct Device { std::string guid; std::wstring label; bool wheel; };
struct State {
    bool ready = false, sdl = false;   // SDL_Init done / succeeded
    std::vector<Device> devices;
    unsigned devices_gen = 0;
    bool open = false, haptic = false;   // the wheel / its force feedback
    std::string guid;
    std::wstring name;
    unsigned vid = 0, pid = 0, open_gen = 0;
    int axes = 0, buttons = 0, hats = 0;
    int axis[AXES] = {};
    unsigned pressed[2] = {};
    Uint8 hat = 0;
    std::wstring status;   // for the status line: the wheel found, or what went wrong
    unsigned status_gen = 0;
};
struct Want { bool open = true, fast = false, haptic = true; std::string guid; };   // guid empty: the first wheel
struct Shared {
    std::mutex m;
    std::condition_variable wake, done;   // the thread has work / finished a round
    unsigned wake_n = 0, want_n = 0;   // (want_n: Want changes)
    State st;
    Want want;
    std::vector<std::function<void()>> jobs;
};
Shared& g_sh = *new Shared;   // (never destroyed: the thread runs until the process ends)
bool g_started = false;

template <typename F> void change_want(F f) {
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        f(g_sh.want);
        g_sh.want_n++;
        g_sh.wake_n++;
    }
    g_sh.wake.notify_one();
}
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
std::wstring w_last_status;

void w_status(const std::wstring& s) {
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

void w_describe() {
    const char* vendor = wheelcfg::vendor_name(SDL_GetJoystickVendor(w_joy));
    wchar_t b[300];
    swprintf(b, 300, L"%hs%hs%hs  -  %d axes, %d buttons, %d hats%ls", vendor ? vendor : "", vendor ? " " : "",
             SDL_GetJoystickName(w_joy), SDL_GetNumJoystickAxes(w_joy), SDL_GetNumJoystickButtons(w_joy),
             SDL_GetNumJoystickHats(w_joy),
             !w_haptic_sys ? L"" : SDL_IsJoystickHaptic(w_joy) ? L", force feedback yes" : L", force feedback no");
    w_status(b);
}

// the device list, when it changed
void w_list() {
    std::vector<Device> devs;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < n; i++) {
        const char* vendor = wheelcfg::vendor_name(SDL_GetJoystickVendorForID(ids[i]));
        std::string label = SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "Joystick";
        if (vendor && label.find(vendor) == std::string::npos) label = std::string(vendor) + " " + label;
        const bool wheel = is_wheel(ids[i]);
        if (!wheel) label += " (not a wheel)";
        devs.push_back({guid_of(ids[i]), widen(label), wheel});
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
        const std::wstring s = want.guid.empty() ? L"No wheel found. Connect it (and install its driver), it shows up here."
                                                 : L"The selected wheel is not connected.";
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
        st.name = widen(SDL_GetJoystickName(w_joy) ? SDL_GetJoystickName(w_joy) : "a wheel");
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
        w_status(L"This wheel has no force feedback SDL can use (or another program holds it).");
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
        w_status(L"The wheel refused the effect: " + widen(SDL_GetError()));
        return;
    }
    SDL_RunHapticEffect(h, w_test_ids[slot], 1);
}

void worker() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    const bool ok = SDL_Init(SDL_INIT_JOYSTICK);
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        g_sh.st.ready = true;
        g_sh.st.sdl = ok;
    }
    g_sh.done.notify_all();
    if (!ok) {
        w_status(L"The wheel support could not start: " + widen(SDL_GetError()));
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
        SDL_UpdateJoysticks();
        SDL_PumpEvents();
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

// ------------------------------------------------------------------ the tab (the window's thread)
HWND g_main;
HWND g_device, g_enabled, g_status, g_steer_view, g_steer_axis, g_steer_inv, g_range, g_range_lbl, g_lock, g_sdz,
    g_sdz_lbl, g_pedal_view, g_pedal_axis[3], g_pedal_cal[3], g_pedal_range[3], g_pedal_range_lbl[3], g_btn_view,
    g_map[wheelcfg::BUTTONS], g_gear[wheelcfg::GEARS], g_ffb, g_ffb_inv, g_soft_lock, g_autoc, g_autoc_str, g_autoc_lbl,
    g_gain[GAIN_N], g_gain_lbl[GAIN_N];

wheelcfg::Config g_cfg;
std::vector<std::string> g_guids;   // device combo entries after "Automatic"
bool g_fresh = false;
// wheel profiles: wheels/<name>.ini next to the launcher ([profile] name, wheels = vid:pid list, notes; [wheel] the
// settings without device and enabled); the built-in ones come from launcher/wheels/
HWND g_profile;
std::wstring g_wheels_dir;
struct Profile { std::wstring file, name, wheels, notes; };
std::vector<Profile> g_profiles;               // no saved settings: the found wheel's defaults, nothing written until changed
bool g_visible = false;
State g_st;                                    // the thread's State, copied at each timer tick
unsigned g_seen_devices = 0, g_seen_open = 0, g_seen_status = 0;
bool g_pending_layout = false;                 // another wheel was picked: its vendor's layout once it is open
unsigned g_prev_buttons[2] = {};               // (newly pressed buttons assign the focused button box)

struct Calibration { int pedal = -1; Uint64 t0 = 0; int start[AXES], ext[AXES]; } g_cal;

int sel(HWND c) { return (int)SendMessageW(c, CB_GETCURSEL, 0, 0); }
void set_sel(HWND c, int i) { SendMessageW(c, CB_SETCURSEL, i, 0); }
bool checked(HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void set_check(HWND c, bool on) { SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
int pos(HWND bar) { return (int)SendMessageW(bar, TBM_GETPOS, 0, 0); }
void set_pos(HWND bar, int v) { SendMessageW(bar, TBM_SETPOS, TRUE, v); }
void set_text(HWND c, const std::wstring& s) { SetWindowTextW(c, s.c_str()); }
wheelcfg::Pedal& pedal(int i) { return i == 0 ? g_cfg.throttle : i == 1 ? g_cfg.brake : g_cfg.clutch; }

// The button boxes (game buttons, gears): "-", "Button 1".."Button 40". A box holds only its choice until it gets the
// focus, then all of them: the 23 boxes' 943 CB_ADDSTRINGs took 0.6 s of the launcher's start (2026-11-12). The value
// (choice index) is kept in the box's user data.
bool is_box(HWND c) {
    return std::find(g_map, g_map + wheelcfg::BUTTONS, c) != g_map + wheelcfg::BUTTONS ||
           std::find(g_gear, g_gear + wheelcfg::GEARS, c) != g_gear + wheelcfg::GEARS;
}
std::wstring box_label(int v) { return v ? L"Button " + std::to_wstring(v) : L"-"; }
int box_get(HWND c) { return (int)GetWindowLongPtrW(c, GWLP_USERDATA); }
bool box_full(HWND c) { return SendMessageW(c, CB_GETCOUNT, 0, 0) > 1; }
void box_set(HWND c, int v) {
    SetWindowLongPtrW(c, GWLP_USERDATA, v);
    if (box_full(c)) {
        set_sel(c, v);
        return;
    }
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)box_label(v).c_str());
    set_sel(c, 0);
}
void box_fill(HWND c) {
    if (box_full(c)) return;
    SendMessageW(c, WM_SETREDRAW, FALSE, 0);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i <= BUTTON_CHOICES; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)box_label(i).c_str());
    set_sel(c, box_get(c));
    SendMessageW(c, WM_SETREDRAW, TRUE, 0);
}

int axis(int a) { return g_st.open && a >= 0 && a < std::min(g_st.axes, AXES) ? g_st.axis[a] : 0; }
bool button(int b) { return g_st.open && b < 64 && (g_st.pressed[b / 32] >> (b % 32) & 1); }

// the device list; keeps the selection (or the saved device)
void fill_devices(const std::string& want) {
    SendMessageW(g_device, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_device, CB_ADDSTRING, 0, (LPARAM)L"Automatic (the first wheel found)");
    g_guids.clear();
    int pick = 0;
    for (const Device& d : g_st.devices) {
        g_guids.push_back(d.guid);
        SendMessageW(g_device, CB_ADDSTRING, 0, (LPARAM)d.label.c_str());
        if (d.guid == want) pick = (int)g_guids.size();
    }
    if (!want.empty() && !pick) {   // a saved wheel that is not plugged in: kept
        g_guids.push_back(want);
        SendMessageW(g_device, CB_ADDSTRING, 0, (LPARAM)L"Saved wheel (not connected)");
        pick = (int)g_guids.size();
    }
    set_sel(g_device, pick);
}
std::string selected_guid() {
    const int i = sel(g_device);
    return i > 0 && i <= (int)g_guids.size() ? g_guids[i - 1] : std::string();
}
// the wheel the thread opens for the selection
std::string target_guid() {
    const std::string want = selected_guid();
    if (!want.empty()) return want;
    for (const Device& d : g_st.devices)
        if (d.wheel) return d.guid;
    return {};
}

// ------------------------------------------------------------------ pedal range: a slider with two handles
// (the pedal's min and max, percent of its travel; while dragging it sends WM_HSCROLL to its parent, as a trackbar)
const wchar_t* const RANGE_CLASS = L"F1CERangeSlider";
struct Range { int lo = 2, hi = 98, drag = -1, active = 0; };
Range& range_of(HWND h) { return *(Range*)GetWindowLongPtrW(h, GWLP_USERDATA); }
int range_lo(HWND h) { return range_of(h).lo; }
int range_hi(HWND h) { return range_of(h).hi; }
void range_set(HWND h, int lo, int hi) {
    Range& r = range_of(h);
    r.lo = std::clamp(lo, 0, 100 - wheelcfg::PEDAL_MIN_SPAN);
    r.hi = std::clamp(hi, r.lo + wheelcfg::PEDAL_MIN_SPAN, 100);
    InvalidateRect(h, nullptr, FALSE);
}

// (sizes in 96 dpi units, as the controls)
int px(int v) { return MulDiv(v, GetDpiForSystem(), 96); }
COLORREF accent() { return GetSysColor(COLOR_HIGHLIGHT); }
void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void frame(HDC dc, RECT r) { FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW)); }

int range_x(const RECT& rc, int v) { return rc.left + px(6) + MulDiv(v, rc.right - rc.left - 2 * px(6), 100); }
int range_v(const RECT& rc, int x) {
    const int w = std::max<int>(rc.right - rc.left - 2 * px(6), 1);
    return std::clamp((int)std::lround((x - rc.left - px(6)) * 100.0 / w), 0, 100);
}
// moves handle t (0 min, 1 max) and tells the parent
void range_move(HWND h, int t, int v) {
    Range& r = range_of(h);
    const int lo = r.lo, hi = r.hi;
    if (t == 0) range_set(h, std::min(v, r.hi - wheelcfg::PEDAL_MIN_SPAN), r.hi);
    else range_set(h, r.lo, std::max(v, r.lo + wheelcfg::PEDAL_MIN_SPAN));
    if (r.lo != lo || r.hi != hi) SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(SB_THUMBTRACK, 0), (LPARAM)h);
}
void range_paint(HWND h, HDC dc, RECT rc) {
    DrawThemeParentBackground(h, dc, &rc);
    const Range& r = range_of(h);
    const int cy = (rc.top + rc.bottom) / 2, x0 = range_x(rc, r.lo), x1 = range_x(rc, r.hi);
    fill(dc, {range_x(rc, 0), cy - px(2), range_x(rc, 100), cy + px(2)}, GetSysColor(COLOR_BTNSHADOW));
    fill(dc, {x0, cy - px(2), x1, cy + px(2)}, accent());
    const bool focus = GetFocus() == h;
    for (int t = 0; t < 2; t++) {
        const int x = t ? x1 : x0;
        RECT th{x - px(5), rc.top + px(3), x + px(5), rc.bottom - px(3)};
        fill(dc, th, focus && r.active == t ? accent() : GetSysColor(COLOR_WINDOW));
        frame(dc, th);
    }
}
LRESULT CALLBACK range_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) new Range); break;
        case WM_NCDESTROY: delete &range_of(h); SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
        case WM_ERASEBKGND: return 1;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            RECT rc;
            GetClientRect(h, &rc);
            Range& r = range_of(h);
            const int v = range_v(rc, (short)LOWORD(lp));
            // the nearer handle; between two at the same place, the side clicked
            const int d0 = std::abs(v - r.lo), d1 = std::abs(v - r.hi);
            r.drag = r.active = d0 < d1 || (d0 == d1 && v < r.lo) ? 0 : 1;
            SetCapture(h);
            range_move(h, r.drag, v);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (range_of(h).drag >= 0) {
                RECT rc;
                GetClientRect(h, &rc);
                range_move(h, range_of(h).drag, range_v(rc, (short)LOWORD(lp)));
            }
            return 0;
        case WM_LBUTTONUP: ReleaseCapture(); return 0;
        case WM_CAPTURECHANGED: range_of(h).drag = -1; return 0;
        case WM_KEYDOWN: {   // arrows move the handle last used, Home / End pick the min / max handle
            Range& r = range_of(h);
            const int step = wp == VK_LEFT || wp == VK_DOWN ? -1 : wp == VK_RIGHT || wp == VK_UP ? 1
                             : wp == VK_PRIOR ? 10 : wp == VK_NEXT ? -10 : 0;
            if (wp == VK_HOME || wp == VK_END) {
                r.active = wp == VK_END;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (step) range_move(h, r.active, (r.active ? r.hi : r.lo) + step);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC pdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC dc = CreateCompatibleDC(pdc);
            HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(dc, bmp);
            range_paint(h, dc, rc);
            BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ profiles
std::wstring profile_value(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
    wchar_t b[1024];
    GetPrivateProfileStringW(section, key, L"", b, 1024, file.c_str());
    return b;
}
void fill_profiles(const std::wstring& select) {
    g_profiles.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_wheels_dir + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Profile pr;
            pr.file = g_wheels_dir + L"\\" + fd.cFileName;
            pr.name = profile_value(pr.file, L"profile", L"name");
            if (pr.name.empty()) pr.name = std::wstring(fd.cFileName, wcslen(fd.cFileName) - 4);
            pr.wheels = profile_value(pr.file, L"profile", L"wheels");
            pr.notes = profile_value(pr.file, L"profile", L"notes");
            g_profiles.push_back(pr);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_profiles.begin(), g_profiles.end(), [](const Profile& a, const Profile& b) { return a.name < b.name; });
    SendMessageW(g_profile, CB_RESETCONTENT, 0, 0);
    for (const Profile& pr : g_profiles) SendMessageW(g_profile, CB_ADDSTRING, 0, (LPARAM)pr.name.c_str());
    SetWindowTextW(g_profile, select.c_str());
}
int find_profile(const std::wstring& name) {
    for (size_t i = 0; i < g_profiles.size(); i++)
        if (!_wcsicmp(g_profiles[i].name.c_str(), name.c_str())) return (int)i;
    return -1;
}
// the profile made for this wheel (its vid:pid in the profile's list), -1 if none
int profile_for(unsigned vid, unsigned pid) {
    wchar_t id[16];
    swprintf(id, 16, L"%04x:%04x", vid, pid);
    for (size_t i = 0; i < g_profiles.size(); i++)
        if (g_profiles[i].wheels.find(id) != std::wstring::npos) return (int)i;
    return -1;
}

void read_controls() {
    g_cfg.enabled = checked(g_enabled);
    g_cfg.device = selected_guid();
    g_cfg.steer_axis = sel(g_steer_axis) - 1;
    g_cfg.steer_invert = checked(g_steer_inv);
    g_cfg.range = pos(g_range) * 10;
    g_cfg.lock = LOCKS[std::max(sel(g_lock), 0)];
    g_cfg.steer_deadzone = pos(g_sdz);
    for (int i = 0; i < 3; i++) {
        pedal(i).axis = sel(g_pedal_axis[i]) - 1;
        pedal(i).min = range_lo(g_pedal_range[i]);
        pedal(i).max = range_hi(g_pedal_range[i]);
    }
    for (int i = 0; i < wheelcfg::BUTTONS; i++) g_cfg.buttons[i] = box_get(g_map[i]) - 1;
    for (int i = 0; i < wheelcfg::GEARS; i++) g_cfg.gears[i] = box_get(g_gear[i]) - 1;
    g_cfg.ffb = checked(g_ffb);
    g_cfg.ffb_invert = checked(g_ffb_inv);
    g_cfg.soft_lock = checked(g_soft_lock);
    for (int i = 0; i < GAIN_N; i++) g_cfg.*GAINS[i].v = pos(g_gain[i]);
    g_cfg.autocenter = sel(g_autoc) == 0 ? -1 : pos(g_autoc_str);
}

void update_labels() {
    wchar_t b[64];
    swprintf(b, 64, L"%d°", pos(g_range) * 10);
    set_text(g_range_lbl, b);
    swprintf(b, 64, L"%d%%", pos(g_sdz));
    set_text(g_sdz_lbl, b);
    for (int i = 0; i < 3; i++) {
        swprintf(b, 64, L"%d-%d%%", range_lo(g_pedal_range[i]), range_hi(g_pedal_range[i]));
        set_text(g_pedal_range_lbl[i], b);
    }
    for (int i = 0; i < GAIN_N; i++) {
        swprintf(b, 64, L"%d%%", pos(g_gain[i]));
        set_text(g_gain_lbl[i], b);
    }
    swprintf(b, 64, L"%d%%", pos(g_autoc_str));
    set_text(g_autoc_lbl, b);
    EnableWindow(g_autoc_str, sel(g_autoc) == 1);
}

void write_controls() {
    set_check(g_enabled, g_cfg.enabled);
    set_sel(g_steer_axis, std::clamp(g_cfg.steer_axis + 1, 0, AXES));
    set_check(g_steer_inv, g_cfg.steer_invert);
    set_pos(g_range, g_cfg.range / 10);
    int li = 0;
    for (int i = 0; i < (int)(sizeof(LOCKS) / sizeof(LOCKS[0])); i++)
        if (LOCKS[i] == g_cfg.lock) li = i;
    set_sel(g_lock, li);
    set_pos(g_sdz, g_cfg.steer_deadzone);
    for (int i = 0; i < 3; i++) {
        set_sel(g_pedal_axis[i], std::clamp(pedal(i).axis + 1, 0, AXES));
        range_set(g_pedal_range[i], pedal(i).min, pedal(i).max);
    }
    for (int i = 0; i < wheelcfg::BUTTONS; i++) box_set(g_map[i], std::clamp(g_cfg.buttons[i] + 1, 0, BUTTON_CHOICES));
    for (int i = 0; i < wheelcfg::GEARS; i++) box_set(g_gear[i], std::clamp(g_cfg.gears[i] + 1, 0, BUTTON_CHOICES));
    set_check(g_ffb, g_cfg.ffb);
    set_check(g_ffb_inv, g_cfg.ffb_invert);
    set_check(g_soft_lock, g_cfg.soft_lock);
    for (int i = 0; i < GAIN_N; i++) set_pos(g_gain[i], g_cfg.*GAINS[i].v);
    set_sel(g_autoc, g_cfg.autocenter < 0 ? 0 : 1);
    set_pos(g_autoc_str, g_cfg.autocenter < 0 ? 30 : g_cfg.autocenter);
    update_labels();
}

// another make of wheel: its usual axes and buttons
void vendor_layout(unsigned vid) {
    read_controls();
    wheelcfg::Config c = g_cfg;
    wheelcfg::defaults_for(c, vid);
    std::copy(c.buttons, c.buttons + wheelcfg::BUTTONS, g_cfg.buttons);
    g_cfg.steer_axis = c.steer_axis;
    g_cfg.throttle = c.throttle;
    g_cfg.brake = c.brake;
    g_cfg.clutch = c.clutch;
    write_controls();
}

void load_profile(int i) {
    if (i < 0 || i >= (int)g_profiles.size()) return;
    const Profile& pr = g_profiles[i];
    std::map<std::string, std::string> vals;
    wchar_t keys[16384];
    const DWORD n = GetPrivateProfileStringW(L"wheel", nullptr, L"", keys, 16384, pr.file.c_str());
    for (const wchar_t* k = keys; k < keys + n && *k; k += wcslen(k) + 1) {
        std::string ks, vs;
        for (const wchar_t* q = k; *q; q++) ks += (char)*q;
        for (wchar_t c : profile_value(pr.file, L"wheel", k)) vs += (char)c;
        vals[ks] = vs;
    }
    read_controls();
    wheelcfg::Config c;
    wheelcfg::load(c, [&](const char* k) -> const char* {
        auto it = vals.find(k);
        return it == vals.end() ? nullptr : it->second.c_str();
    });
    c.device = g_cfg.device;   // (a profile is the settings, not the choice of wheel)
    c.enabled = g_cfg.enabled;
    c.pedals_device = g_cfg.pedals_device;
    if (g_cfg.pedals_separate) {   // (a separate pedal set keeps its own calibration)
        c.pedals_separate = true;
        c.throttle = g_cfg.throttle;
        c.brake = g_cfg.brake;
        c.clutch = g_cfg.clutch;
    }
    g_cfg = c;
    write_controls();
    SetWindowTextW(g_profile, pr.name.c_str());
    g_fresh = false;
    set_text(g_status, L"Profile \"" + pr.name + L"\" loaded." + (pr.notes.empty() ? L"" : L" " + pr.notes));
}

void save_profile() {
    wchar_t b[256];
    GetWindowTextW(g_profile, b, 256);
    std::wstring name = b;
    while (!name.empty() && name.back() == L' ') name.pop_back();
    if (name.empty()) {
        MessageBoxW(g_main, L"Type a name for the profile in the Profile box first.", L"F1 CE", MB_ICONINFORMATION);
        return;
    }
    std::wstring file_name;
    for (wchar_t c : name) file_name += wcschr(L"\\/:*?\"<>|", c) ? L'_' : c;
    const int existing = find_profile(name);
    const std::wstring file = existing >= 0 ? g_profiles[existing].file : g_wheels_dir + L"\\" + file_name + L".ini";
    if (existing >= 0 &&
        MessageBoxW(g_main, (L"Replace the profile \"" + name + L"\" with the current settings?").c_str(), L"F1 CE",
                    MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    CreateDirectoryW(g_wheels_dir.c_str(), nullptr);
    read_controls();
    // the wheels it suits: the profile's list (if it had one) and the wheel connected now
    std::wstring wheels = existing >= 0 ? g_profiles[existing].wheels : L"";
    if (g_st.open) {
        wchar_t id[16];
        swprintf(id, 16, L"%04x:%04x", g_st.vid, g_st.pid);
        if (wheels.find(id) == std::wstring::npos) wheels += (wheels.empty() ? L"" : L",") + std::wstring(id);
    }
    WritePrivateProfileStringW(L"profile", L"name", name.c_str(), file.c_str());
    WritePrivateProfileStringW(L"profile", L"wheels", wheels.c_str(), file.c_str());
    if (existing < 0) {
        std::wstring notes = L"Saved by the launcher";
        if (g_st.open) notes += L" for " + g_st.name;
        WritePrivateProfileStringW(L"profile", L"notes", (notes + L".").c_str(), file.c_str());
    }
    wheelcfg::save(g_cfg, [&](const char* k, const std::string& v) {
        if (!strcmp(k, "device") || !strcmp(k, "enabled") || !strcmp(k, "pedals_device")) return;
        WritePrivateProfileStringW(L"wheel", widen(k).c_str(), widen(v).c_str(), file.c_str());
    });
    g_fresh = false;
    fill_profiles(name);
    set_text(g_status, L"Profile \"" + name + L"\" saved in " + file + L".");
}

void delete_profile() {
    wchar_t b[256];
    GetWindowTextW(g_profile, b, 256);
    const int i = find_profile(b);
    if (i < 0) return;
    if (MessageBoxW(g_main, (L"Delete the profile \"" + g_profiles[i].name + L"\"?").c_str(), L"F1 CE",
                    MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    DeleteFileW(g_profiles[i].file.c_str());
    fill_profiles(L"");
}

// ------------------------------------------------------------------ pedal calibration: press the pedal fully
void calibrate_start(int p) {
    if (!g_st.open) return;
    g_cal.pedal = p;
    g_cal.t0 = GetTickCount64();
    for (int a = 0; a < AXES; a++) g_cal.start[a] = g_cal.ext[a] = axis(a);
    for (int i = 0; i < 3; i++) set_text(g_pedal_cal[i], i == p ? L"Press it fully..." : L"Calibrate");
}
void calibrate_end(const wchar_t* msg) {
    for (int i = 0; i < 3; i++) set_text(g_pedal_cal[i], L"Calibrate");
    if (msg) set_text(g_status, msg);
    g_cal.pedal = -1;
}
// the axis that moved most: at rest where it started, fully pressed where it went; done when the pedal comes back
void calibrate_poll() {
    if (g_cal.pedal < 0 || !g_st.open) return;
    int best = -1, dev = 0;
    for (int a = 0; a < AXES && a < g_st.axes; a++) {
        const int v = axis(a);
        if (std::abs(v - g_cal.start[a]) > std::abs(g_cal.ext[a] - g_cal.start[a])) g_cal.ext[a] = v;
        if (std::abs(g_cal.ext[a] - g_cal.start[a]) > dev) { dev = std::abs(g_cal.ext[a] - g_cal.start[a]); best = a; }
    }
    if (best >= 0 && dev > 12000 && std::abs(axis(best) - g_cal.start[best]) < dev / 4) {
        wheelcfg::Pedal& p = pedal(g_cal.pedal);
        p.axis = best;
        p.rest = g_cal.start[best];
        p.full = g_cal.ext[best];
        set_sel(g_pedal_axis[g_cal.pedal], best + 1);
        wchar_t b[160];
        swprintf(b, 160, L"%ls: axis %d, at rest %d, fully pressed %d.", PEDAL_NAMES[g_cal.pedal], best + 1, p.rest, p.full);
        calibrate_end(b);
    } else if (GetTickCount64() - g_cal.t0 > 10000) {
        calibrate_end(L"No pedal moved: press Calibrate, then press the pedal all the way and let it go.");
    }
}

// ------------------------------------------------------------------ live views
void draw_steering(HDC dc, RECT rc) {
    fill(dc, rc, GetSysColor(COLOR_WINDOW));
    frame(dc, rc);
    const int cx = (rc.left + rc.right) / 2, w = (rc.right - rc.left) / 2 - px(2);
    // the game's lock inside the wheel's rotation
    const float lock = (float)g_cfg.lock_deg(900), range = (float)g_cfg.range;
    const int lw = (int)std::lround(w * std::min(lock / range, 1.0f));
    RECT lr{cx - lw, rc.top + px(2), cx + lw, rc.bottom - px(16)};
    fill(dc, lr, RGB(232, 240, 250));
    const float raw = (float)(axis(g_cfg.steer_axis) - g_cfg.steer_center) / 32767.0f * (g_cfg.steer_invert ? -1 : 1);
    const int x = cx + (int)std::lround(std::clamp(raw, -1.0f, 1.0f) * w);
    RECT bar{std::min(cx, x), rc.top + px(6), std::max(cx, x) + 1, rc.bottom - px(20)};
    fill(dc, bar, accent());
    RECT mid{cx, rc.top + px(2), cx + std::max(px(1), 1), rc.bottom - px(16)};
    fill(dc, mid, GetSysColor(COLOR_BTNSHADOW));
    wchar_t b[96];
    const float game = g_cfg.steer(axis(g_cfg.steer_axis));
    swprintf(b, 96, L"%+.0f°  (game %+.0f%%)", raw * range / 2, std::clamp(game, -1.0f, 1.0f) * 100);
    SetBkMode(dc, TRANSPARENT);
    RECT tr{rc.left, rc.bottom - px(16), rc.right, rc.bottom};
    DrawTextW(dc, b, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// each pedal's travel as a bar, its min and max as marks, and below it what the game gets
void draw_pedals(HDC dc, RECT rc) {
    fill(dc, rc, GetSysColor(COLOR_BTNFACE));
    const int bw = (rc.right - rc.left) / 3;
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 3; i++) {   // (the Thrustmaster order: clutch, brake, gas)
        const int p = 2 - i;
        RECT col{rc.left + i * bw + bw / 4, rc.top + px(2), rc.left + i * bw + bw * 3 / 4, rc.bottom - px(34)};
        fill(dc, col, GetSysColor(COLOR_WINDOW));
        frame(dc, col);
        const int raw = axis(pedal(p).axis);
        const int h = col.bottom - col.top;
        RECT f = col;
        f.top = col.bottom - (int)std::lround(h * pedal(p).travel(raw));
        InflateRect(&f, -px(2), 0);
        f.bottom -= 1;
        fill(dc, f, p == 1 ? RGB(200, 40, 40) : p == 0 ? RGB(40, 160, 60) : accent());
        for (int m : {pedal(p).min, pedal(p).max}) {
            const int y = col.bottom - MulDiv(h, m, 100);
            fill(dc, {col.left - px(3), y, col.right + px(3), y + std::max(px(1), 1)}, GetSysColor(COLOR_WINDOWTEXT));
        }
        wchar_t b[32];
        RECT tr{rc.left + i * bw, rc.bottom - px(32), rc.left + (i + 1) * bw, rc.bottom - px(16)};
        DrawTextW(dc, PEDAL_NAMES[p], -1, &tr, DT_CENTER | DT_SINGLELINE);
        swprintf(b, 32, pedal(p).axis < 0 ? L"-" : L"%.0f%%", pedal(p).value(raw) * 100);
        tr.top += px(16);
        tr.bottom += px(16);
        DrawTextW(dc, b, -1, &tr, DT_CENTER | DT_SINGLELINE);
    }
}

void draw_buttons(HDC dc, RECT rc) {
    fill(dc, rc, GetSysColor(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    const int n = g_st.open ? std::min(g_st.buttons, 40) : 0;
    const int per_row = 10, bw = (rc.right - rc.left - px(56)) / per_row, bh = px(19);
    HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(g_btn_view, WM_GETFONT, 0, 0));
    for (int b = 0; b < n; b++) {
        RECT r{rc.left + (b % per_row) * bw, rc.top + (b / per_row) * (bh + px(3)), 0, 0};
        r.right = r.left + bw - px(3);
        r.bottom = r.top + bh;
        const bool on = button(b);
        fill(dc, r, on ? accent() : GetSysColor(COLOR_WINDOW));
        frame(dc, r);
        SetTextColor(dc, on ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT));
        wchar_t t[8];
        swprintf(t, 8, L"%d", b + 1);
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    // the hat as a 3x3 rose
    const Uint8 h = g_st.open ? g_st.hat : 0;
    const int s = px(14), x0 = rc.right - 3 * s - px(4), y0 = rc.top;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) {
            if (x == 1 && y == 1) continue;
            const bool on = ((y == 0 && (h & SDL_HAT_UP)) || (y == 2 && (h & SDL_HAT_DOWN))) && x == 1 ? true
                            : ((x == 0 && (h & SDL_HAT_LEFT)) || (x == 2 && (h & SDL_HAT_RIGHT))) && y == 1 ? true
                            : x != 1 && y != 1 && (y == 0 ? (h & SDL_HAT_UP) : (h & SDL_HAT_DOWN)) &&
                                  (x == 0 ? (h & SDL_HAT_LEFT) : (h & SDL_HAT_RIGHT));
            RECT r{x0 + x * s, y0 + y * s, x0 + x * s + s - px(2), y0 + y * s + s - px(2)};
            fill(dc, r, on ? accent() : GetSysColor(COLOR_WINDOW));
            frame(dc, r);
        }
    if (!n) {
        RECT tr = rc;
        DrawTextW(dc, L"(no wheel)", -1, &tr, DT_SINGLELINE | DT_LEFT | DT_TOP);
    }
    SelectObject(dc, old);
}

// The live views: a window class of their own, painted in one blit from a back buffer and never erased (they are
// redrawn 30 times a second; an owner-drawn static first gets its parent's background, which flashed)
const wchar_t* const VIEW_CLASS = L"F1CEWheelView";
LRESULT CALLBACK view_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_SETFONT: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)wp); return 0;
        case WM_GETFONT: return GetWindowLongPtrW(h, GWLP_USERDATA);
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC pdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC dc = CreateCompatibleDC(pdc);
            HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(dc, bmp);
            SelectObject(dc, (HFONT)GetWindowLongPtrW(h, GWLP_USERDATA));
            const int id = GetDlgCtrlID(h);
            if (id == ID_STEER_VIEW) draw_steering(dc, rc);
            else if (id == ID_PEDAL_VIEW) draw_pedals(dc, rc);
            else draw_buttons(dc, rc);
            BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void register_classes() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = view_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = VIEW_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = range_proc;
    wc.lpszClassName = RANGE_CLASS;
    RegisterClassW(&wc);
}

HWND combo(AddFn add, int x, int y, int w, int id, int page) {
    return add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 300, id, page);
}
HWND slider(AddFn add, int x, int y, int w, int id, int page, int lo, int hi) {
    HWND s = add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, x, y, w, 24, id, page);
    SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(s, TBM_SETPAGESIZE, 0, std::max((hi - lo) / 10, 1));
    return s;
}

// the thread opened a wheel (or closed it)
void opened() {
    if (!g_st.open) return;
    if (g_fresh) {   // first look at the tab: the wheel's profile, else its vendor's usual layout
        const int pr = profile_for(g_st.vid, g_st.pid);
        if (pr >= 0) {
            load_profile(pr);
        } else {
            wheelcfg::defaults_for(g_cfg, g_st.vid);
            write_controls();
        }
    } else if (g_pending_layout && g_st.guid == target_guid() && g_st.vid) {
        vendor_layout(g_st.vid);
    }
    g_pending_layout = false;
}
}  // namespace

// ------------------------------------------------------------------ interface
int build(HWND main, int page, int x, int y, int w, AddFn add, TipFn tip) {
    g_main = main;
    register_classes();
    const int GAP = 12;
    // wheel picker
    add(L"STATIC", L"Wheel", 0, x, y + 4, 60, 18, 0, page);
    g_device = combo(add, x + 60, y, 430, ID_DEVICE, page);
    g_enabled = add(L"BUTTON", L"Use the wheel in the game", WS_TABSTOP | BS_AUTOCHECKBOX, x + 506, y + 2, 220, 22,
                    ID_ENABLED, page);
    tip(g_enabled, L"The game sees it as a Logitech G25 (its own wheel support, with force feedback). With a wheel "
                   L"the game makes it the player's controller.");
    add(L"BUTTON", L"Defaults for this wheel", WS_TABSTOP, x + w - 170, y, 170, 24, ID_DEFAULTS, page);
    g_status = add(L"STATIC", L"Looking for wheels...", SS_ENDELLIPSIS, x, y + 30, w, 18, ID_STATUS, page);
    y += 54;
    add(L"STATIC", L"Profile", 0, x, y + 4, 60, 18, 0, page);
    g_profile = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, x + 60, y, 330, 300,
                    ID_PROFILE, page);
    tip(g_profile, L"Settings for a wheel: pick one and press Load, or type a name and press Save to keep the "
                   L"current settings (files in the wheels folder next to the launcher).");
    add(L"BUTTON", L"Load", WS_TABSTOP, x + 400, y, 74, 24, ID_PROF_LOAD, page);
    add(L"BUTTON", L"Save", WS_TABSTOP, x + 480, y, 74, 24, ID_PROF_SAVE, page);
    add(L"BUTTON", L"Delete", WS_TABSTOP, x + 560, y, 74, 24, ID_PROF_DEL, page);
    y += 36;

    const int col1 = 290, col2 = 250, col3 = w - col1 - col2 - 2 * GAP;
    const int x2 = x + col1 + GAP, x3 = x2 + col2 + GAP;
    // steering
    int y1 = y;
    HWND g1 = add(L"BUTTON", L"Steering", BS_GROUPBOX, x, y1, col1, 100, 0, page);
    y1 += 22;
    g_steer_view = add(VIEW_CLASS, L"", 0, x + 10, y1, col1 - 20, 46, ID_STEER_VIEW, page);
    y1 += 54;
    add(L"STATIC", L"Axis", 0, x + 10, y1 + 4, 50, 18, 0, page);
    g_steer_axis = combo(add, x + 60, y1, 80, ID_STEER_AXIS, page);
    g_steer_inv = add(L"BUTTON", L"Invert", WS_TABSTOP | BS_AUTOCHECKBOX, x + 150, y1 + 2, 60, 22, ID_STEER_INV, page);
    HWND c = add(L"BUTTON", L"Centre", WS_TABSTOP, x + col1 - 80, y1, 70, 24, ID_CENTER, page);
    tip(c, L"Hold the wheel straight and press: its position becomes the centre.");
    y1 += 32;
    HWND l = add(L"STATIC", L"Wheel rotation", SS_NOTIFY, x + 10, y1 + 4, 100, 18, 0, page);
    g_range = slider(add, x + 106, y1, col1 - 166, ID_RANGE, page, 18, 252);
    g_range_lbl = add(L"STATIC", L"", 0, x + col1 - 56, y1 + 4, 46, 18, ID_RANGE_LBL, page);
    tip(l, L"Lock to lock, as set in the wheel's own control panel or driver (Thrustmaster, Logitech G HUB, Fanatec, "
           L"MOZA Pit House...).");
    tip(g_range, L"Lock to lock, as set in the wheel's own control panel or driver.");
    y1 += 30;
    l = add(L"STATIC", L"Steering lock", SS_NOTIFY, x + 10, y1 + 4, 100, 18, 0, page);
    g_lock = combo(add, x + 106, y1, col1 - 116, ID_LOCK, page);
    for (int lk : LOCKS) {
        wchar_t b[48];
        swprintf(b, 48, lk ? L"%d°" : L"Game's (900°)", lk);
        SendMessageW(g_lock, CB_ADDSTRING, 0, (LPARAM)b);
    }
    tip(l, L"The wheel turn that gives full steering in the game. Smaller: quicker steering (the rest of the wheel's "
           L"rotation is the soft lock).");
    y1 += 30;
    add(L"STATIC", L"Dead zone", 0, x + 10, y1 + 4, 100, 18, 0, page);
    g_sdz = slider(add, x + 106, y1, col1 - 166, ID_SDZ, page, 0, 10);
    g_sdz_lbl = add(L"STATIC", L"", 0, x + col1 - 56, y1 + 4, 46, 18, ID_SDZ_LBL, page);
    y1 += 32;
    SetWindowPos(g1, nullptr, 0, 0, px(col1), px(y1 - y), SWP_NOMOVE | SWP_NOZORDER);
    // H-shifter gears, under the steering
    y1 += GAP;
    const int y5 = y1;
    HWND g5 = add(L"BUTTON", L"H-shifter (optional; without it the paddles shift)", BS_GROUPBOX, x, y1, col1, 100, 0,
                  page);
    y1 += 22;
    const int gw = (col1 - 20) / 2;
    for (int i = 0; i < wheelcfg::GEARS; i++) {
        const int gx = x + 10 + (i % 2) * gw, gy = y1 + (i / 2) * 26;
        wchar_t b[16];
        swprintf(b, 16, i ? L"Gear %d" : L"Reverse", i);
        add(L"STATIC", b, 0, gx, gy + 4, 56, 18, 0, page);
        g_gear[i] = combo(add, gx + 56, gy, gw - 62, ID_GEAR + i, page);
    }
    y1 += 4 * 26 + 6;
    SetWindowPos(g5, nullptr, 0, 0, px(col1), px(y1 - y5), SWP_NOMOVE | SWP_NOZORDER);

    // pedals: live view, axes, and the part of each pedal's travel the game uses
    int y2 = y;
    HWND g2 = add(L"BUTTON", L"Pedals", BS_GROUPBOX, x2, y2, col2, 100, 0, page);
    y2 += 22;
    g_pedal_view = add(VIEW_CLASS, L"", 0, x2 + 10, y2, col2 - 20, 104, ID_PEDAL_VIEW, page);
    y2 += 110;
    for (int i = 0; i < 3; i++) {
        add(L"STATIC", PEDAL_NAMES[i], 0, x2 + 10, y2 + 4, 46, 18, 0, page);
        g_pedal_axis[i] = combo(add, x2 + 56, y2, 72, ID_PEDAL_AXIS + i, page);
        g_pedal_cal[i] = add(L"BUTTON", L"Calibrate", WS_TABSTOP, x2 + 134, y2, col2 - 144, 24, ID_PEDAL_CAL + i, page);
        tip(g_pedal_cal[i], L"Press, then push the pedal all the way down and let it go: finds its axis and range.");
        y2 += 30;
    }
    const wchar_t* range_tip = L"The part of the pedal's travel the game uses: up to the left handle the pedal reads as "
                               L"released, from the right handle on as fully pressed (the marks on the bars above).";
    l = add(L"STATIC", L"Travel used (min - max)", SS_NOTIFY, x2 + 10, y2 + 2, col2 - 20, 18, 0, page);
    tip(l, range_tip);
    y2 += 22;
    for (int i = 0; i < 3; i++) {
        add(L"STATIC", PEDAL_NAMES[i], 0, x2 + 10, y2 + 4, 46, 18, 0, page);
        g_pedal_range[i] = add(RANGE_CLASS, L"", WS_TABSTOP, x2 + 56, y2, col2 - 122, 24, ID_PEDAL_RANGE + i, page);
        g_pedal_range_lbl[i] = add(L"STATIC", L"", 0, x2 + col2 - 62, y2 + 4, 54, 18, ID_PEDAL_RANGE_LBL + i, page);
        tip(g_pedal_range[i], range_tip);
        y2 += 26;
    }
    y2 += 6;
    SetWindowPos(g2, nullptr, 0, 0, px(col2), px(y2 - y), SWP_NOMOVE | SWP_NOZORDER);

    // force feedback, under steering and pedals
    int y4 = std::max(y1, y2) + GAP;
    const int fw = col1 + GAP + col2;
    HWND g4 = add(L"BUTTON", L"Force Feedback", BS_GROUPBOX, x, y4, fw, 100, 0, page);
    const int y4top = y4;
    y4 += 22;
    g_ffb = add(L"BUTTON", L"Force feedback", WS_TABSTOP | BS_AUTOCHECKBOX, x + 10, y4, 130, 22, ID_FFB, page);
    g_ffb_inv = add(L"BUTTON", L"Invert forces", WS_TABSTOP | BS_AUTOCHECKBOX, x + 150, y4, 120, 22, ID_FFB_INV, page);
    g_soft_lock = add(L"BUTTON", L"Soft lock at the steering lock", WS_TABSTOP | BS_AUTOCHECKBOX, x + 280, y4, fw - 290, 22,
                      ID_SOFT_LOCK, page);
    tip(g_soft_lock, L"Pushes back when the wheel turns past the game's steering lock.");
    y4 += 28;
    for (int i = 0; i < GAIN_N; i++) {
        HWND gl = add(L"STATIC", GAINS[i].label, SS_NOTIFY, x + 10, y4 + 4, 150, 18, 0, page);
        g_gain[i] = slider(add, x + 160, y4, fw - 230, ID_GAIN + i, page, 0, GAINS[i].max);
        g_gain_lbl[i] = add(L"STATIC", L"", 0, x + fw - 64, y4 + 4, 54, 18, ID_GAIN_LBL + i, page);
        tip(gl, GAINS[i].tip);
        tip(g_gain[i], GAINS[i].tip);
        y4 += 26;
    }
    add(L"STATIC", L"Auto-centre", 0, x + 10, y4 + 4, 150, 18, 0, page);
    g_autoc = combo(add, x + 160, y4, 170, ID_AUTOC, page);
    SendMessageW(g_autoc, CB_ADDSTRING, 0, (LPARAM)L"By the game");
    SendMessageW(g_autoc, CB_ADDSTRING, 0, (LPARAM)L"Fixed spring");
    g_autoc_str = slider(add, x + 340, y4, fw - 410, ID_AUTOC_STR, page, 0, 100);
    g_autoc_lbl = add(L"STATIC", L"", 0, x + fw - 64, y4 + 4, 54, 18, ID_AUTOC_LBL, page);
    y4 += 32;
    add(L"STATIC", L"Test", 0, x + 10, y4 + 4, 40, 18, 0, page);
    const int tw = (fw - 70) / 5;
    for (int i = 0; i < 5; i++) add(L"BUTTON", TESTS[i], WS_TABSTOP, x + 60 + i * tw, y4, tw - 6, 24, ID_TEST + i, page);
    y4 += 34;
    SetWindowPos(g4, nullptr, 0, 0, px(fw), px(y4 - y4top), SWP_NOMOVE | SWP_NOZORDER);

    // buttons: live view, then the game's buttons; a box takes the next wheel button pressed while it has the focus
    int y3 = y;
    HWND g3 = add(L"BUTTON", L"Buttons", BS_GROUPBOX, x3, y3, col3, 100, 0, page);
    y3 += 22;
    g_btn_view = add(VIEW_CLASS, L"", 0, x3 + 10, y3, col3 - 20, 70, ID_BTN_VIEW, page);
    y3 += 74;
    add(L"STATIC", L"Click a box, then press the wheel's button. The wheel's hat is the d-pad.", 0, x3 + 10, y3,
        col3 - 20, 34, 0, page);
    y3 += 38;
    const int lw = 118, cw = col3 - 20 - lw;
    for (int i = 0; i < wheelcfg::BUTTONS; i++) {
        add(L"STATIC", widen(wheelcfg::BUTTON_NAMES[i]).c_str(), 0, x3 + 10, y3 + 4, lw, 18, 0, page);
        g_map[i] = combo(add, x3 + 10 + lw, y3, cw, ID_MAP + i, page);
        y3 += 26;
    }
    y3 += 6;
    SetWindowPos(g3, nullptr, 0, 0, px(col3), px(y3 - y), SWP_NOMOVE | SWP_NOZORDER);

    // choices
    for (HWND a : {g_steer_axis, g_pedal_axis[0], g_pedal_axis[1], g_pedal_axis[2]}) {
        SendMessageW(a, CB_ADDSTRING, 0, (LPARAM)L"None");
        for (int i = 1; i <= AXES; i++) {
            wchar_t b[16];
            swprintf(b, 16, L"Axis %d", i);
            SendMessageW(a, CB_ADDSTRING, 0, (LPARAM)b);
        }
    }
    for (HWND b : g_map) box_set(b, 0);
    for (HWND b : g_gear) box_set(b, 0);
    return std::max(y3, y4);
}

void load(const std::wstring& ini) {
    std::map<std::string, std::string> vals;
    wchar_t keys[16384];
    const DWORD n = GetPrivateProfileStringW(L"wheel", nullptr, L"", keys, 16384, ini.c_str());
    for (const wchar_t* k = keys; k < keys + n && *k; k += wcslen(k) + 1) {
        wchar_t v[512];
        GetPrivateProfileStringW(L"wheel", k, L"", v, 512, ini.c_str());
        std::string ks, vs;
        for (const wchar_t* p = k; *p; p++) ks += (char)*p;
        for (const wchar_t* p = v; *p; p++) vs += (char)*p;
        vals[ks] = vs;
    }
    g_cfg = {};
    g_fresh = vals.empty();
    if (g_fresh) wheelcfg::defaults_for(g_cfg, 0);
    wheelcfg::load(g_cfg, [&](const char* k) -> const char* {
        auto it = vals.find(k);
        return it == vals.end() ? nullptr : it->second.c_str();
    });
    fill_devices(g_cfg.device);
    write_controls();
    const size_t slash = ini.find_last_of(L"\\/");
    g_wheels_dir = (slash == std::wstring::npos ? L"." : ini.substr(0, slash)) + L"\\wheels";
    wchar_t b[256];
    GetPrivateProfileStringW(L"wheel", L"profile", L"", b, 256, ini.c_str());
    fill_profiles(b);
}

void start() {
    if (g_started) return;
    g_started = true;
    const std::string guid = selected_guid();
    change_want([&](Want& w) { w.guid = guid; });
    std::thread(worker).detach();
}

void save(const std::wstring& ini) {
    if (g_fresh) return;   // (the game takes the vendor's defaults itself)
    read_controls();
    wheelcfg::save(g_cfg, [&](const char* k, const std::string& v) {
        WritePrivateProfileStringW(L"wheel", widen(k).c_str(), widen(v).c_str(), ini.c_str());
    });
    for (const char* p : {"throttle", "brake", "clutch"})   // (replaced by min and max)
        WritePrivateProfileStringW(L"wheel", (widen(p) + L"_deadzone").c_str(), nullptr, ini.c_str());
    wchar_t b[256];
    GetWindowTextW(g_profile, b, 256);
    WritePrivateProfileStringW(L"wheel", L"profile", b, ini.c_str());
}

void shown(bool visible) {
    g_visible = visible;
    change_want([&](Want& w) {
        w.open = true;
        w.fast = visible;
        w.haptic = visible;
    });
    if (visible) {
        SetTimer(g_main, TIMER_ID, 33, nullptr);
        timer();
    } else {
        KillTimer(g_main, TIMER_ID);
        calibrate_end(nullptr);
    }
}

void release() {
    KillTimer(g_main, TIMER_ID);
    g_pending_layout = false;
    change_want([](Want& w) {
        w.open = false;
        w.haptic = false;
    });
    // (the thread may still be starting SDL or opening the wheel: at most a few seconds)
    std::unique_lock<std::mutex> l(g_sh.m);
    g_sh.done.wait_for(l, std::chrono::seconds(3), [] { return g_sh.st.ready && !g_sh.st.open && !g_sh.st.haptic; });
}

bool command(HWND ctl, int id, int code) {
    if (id < ID_DEVICE || id >= ID_LAST) return false;
    if (id >= ID_PROFILE) {
        if (code == BN_CLICKED && id == ID_PROF_LOAD) {
            wchar_t b[256];
            GetWindowTextW(g_profile, b, 256);
            load_profile(find_profile(b));
        } else if (code == BN_CLICKED && id == ID_PROF_SAVE) {
            save_profile();
        } else if (code == BN_CLICKED && id == ID_PROF_DEL) {
            delete_profile();
        }
        return true;
    }
    if (is_box(ctl)) {
        if (code == CBN_SETFOCUS) box_fill(ctl);
        if (code != CBN_SELCHANGE) return true;
        SetWindowLongPtrW(ctl, GWLP_USERDATA, sel(ctl));
    }
    if ((code == CBN_SELCHANGE || code == BN_CLICKED) && (id < ID_TEST || id >= ID_TEST + 5)) g_fresh = false;
    if (id == ID_DEVICE && code == CBN_SELCHANGE) {
        const std::string guid = selected_guid();
        change_want([&](Want& w) { w.guid = guid; });
        // another make of wheel: its usual layout, now if it is the open one, else once the thread opened it
        g_pending_layout = false;
        if (g_st.open && g_st.guid == target_guid()) {
            if (g_st.vid) vendor_layout(g_st.vid);
        } else {
            g_pending_layout = true;
        }
    } else if (id == ID_DEFAULTS && code == BN_CLICKED) {
        const std::string dev = selected_guid();
        const bool en = checked(g_enabled);
        g_cfg = {};
        g_cfg.device = dev;
        g_cfg.enabled = en;
        wheelcfg::defaults_for(g_cfg, g_st.open ? g_st.vid : 0x044f);
        write_controls();
    } else if (id == ID_CENTER && code == BN_CLICKED) {
        g_cfg.steer_center = axis(sel(g_steer_axis) - 1);
    } else if (id >= ID_PEDAL_CAL && id < ID_PEDAL_CAL + 3 && code == BN_CLICKED) {
        read_controls();
        calibrate_start(id - ID_PEDAL_CAL);
    } else if (id >= ID_TEST && id < ID_TEST + 5 && code == BN_CLICKED) {
        read_controls();
        const wheelcfg::Config cfg = g_cfg;
        const int t = id - ID_TEST;
        post([cfg, t] { w_test(cfg, t); });
    } else if (id == ID_AUTOC && code == CBN_SELCHANGE) {
        update_labels();
    } else if (id >= ID_PEDAL_AXIS && id < ID_PEDAL_AXIS + 3 && code == CBN_SELCHANGE) {
        // a pedal picked by hand: the usual at rest / pressed ends (released = top of the axis)
        wheelcfg::Pedal& p = pedal(id - ID_PEDAL_AXIS);
        p.axis = sel(g_pedal_axis[id - ID_PEDAL_AXIS]) - 1;
        p.rest = 32767;
        p.full = -32768;
    }
    read_controls();
    return true;
}

bool draw(const DRAWITEMSTRUCT*) { return false; }   // (the live views paint themselves: view_proc)

void hscroll(HWND bar) {
    if (bar != g_range && bar != g_sdz && bar != g_autoc_str && std::find(g_gain, g_gain + GAIN_N, bar) == g_gain + GAIN_N &&
        std::find(g_pedal_range, g_pedal_range + 3, bar) == g_pedal_range + 3)
        return;
    update_labels();
    g_fresh = false;
    read_controls();
    if (bar == g_gain[0]) {
        const int gain = g_cfg.gain;
        post([gain] {
            if (w_haptic && (SDL_GetHapticFeatures(w_haptic) & SDL_HAPTIC_GAIN)) SDL_SetHapticGain(w_haptic, gain);
        });
    }
}

void timer() {
    if (!g_visible) return;
    {
        std::lock_guard<std::mutex> l(g_sh.m);
        g_st = g_sh.st;
    }
    if (g_st.devices_gen != g_seen_devices) {   // plugging and unplugging
        g_seen_devices = g_st.devices_gen;
        fill_devices(selected_guid());
    }
    if (g_st.status_gen != g_seen_status) {
        g_seen_status = g_st.status_gen;
        set_text(g_status, g_st.status);
    }
    if (g_st.open_gen != g_seen_open) {
        g_seen_open = g_st.open_gen;
        opened();
    }
    calibrate_poll();
    // a focused button box takes the button pressed now
    if (g_st.open) {
        const HWND f = GetFocus();
        int box = -1;
        for (int i = 0; i < wheelcfg::BUTTONS; i++)
            if (f == g_map[i]) box = i;
        for (int i = 0; i < wheelcfg::GEARS; i++)
            if (f == g_gear[i]) box = 100 + i;
        for (int b = 0; b < 64 && box >= 0; b++)
            if ((g_st.pressed[b / 32] & ~g_prev_buttons[b / 32]) & (1u << (b % 32))) {
                if (b < BUTTON_CHOICES) box_set(box >= 100 ? g_gear[box - 100] : g_map[box], b + 1);
                g_fresh = false;
                read_controls();
                break;
            }
        g_prev_buttons[0] = g_st.pressed[0];
        g_prev_buttons[1] = g_st.pressed[1];
    }
    for (HWND v : {g_steer_view, g_pedal_view, g_btn_view}) InvalidateRect(v, nullptr, FALSE);
}
}  // namespace wheeltab
