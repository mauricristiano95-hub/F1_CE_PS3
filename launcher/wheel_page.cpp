// Steering Wheel section: picks and sets up the wheel the game drives as a Logitech G25 (runtime/hle_usbd.cpp,
// runtime/wheel_sdl.cpp), after the Thrustmaster control panel: live steering, pedal and button views, axes found by
// pressing the pedal, buttons assigned by pressing them on the wheel, the wheel's rotation and the game's steering
// lock, and the force-feedback gains with test effects. Settings: the [wheel] section of f1ce_launcher.ini; wheel
// profiles: wheels/<name>.ini next to the launcher ([profile] name, wheels = vid:pid list, notes; [wheel] settings).
#include "wheel_page.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "ini.h"
#include "options.h"
#include "ui.h"
#include "wheel_device.h"

namespace wheel {
namespace {
using wheeldev::AXES;
constexpr int BUTTON_CHOICES = 40;
const char* const PEDAL_NAMES[3] = {"Gas", "Brake", "Clutch"};
const int LOCKS[] = {0, 180, 240, 270, 360, 420, 450, 540, 720, 900, 1080};
constexpr int LOCK_N = sizeof(LOCKS) / sizeof(LOCKS[0]);
struct Gain { const char* label; int wheelcfg::Config::*v; int max; const char* help; };
const Gain GAINS[] = {
    {"Overall strength", &wheelcfg::Config::gain, 100, "All forces (the wheel's own gain when it has one)."},
    {"Road and tyre forces", &wheelcfg::Config::constant, 150,
     "The game's constant force: the tyres' self-aligning pull, kerbs, bumps and impacts."},
    {"Centring spring", &wheelcfg::Config::spring, 150, "The game's centring spring, stiffer with speed."},
    {"Damper", &wheelcfg::Config::damper, 150, "The game's damper effects (if any)."},
    {"Wheel weight", &wheelcfg::Config::weight, 100, "A damper that always plays: steadies strong wheels when nobody holds them."},
    {"Minimum force", &wheelcfg::Config::min_force, 30,
     "Added to every constant force, for wheels whose motor does not move with small forces."},
};
const char* const TESTS[] = {"Push left", "Push right", "Spring", "Bumpy road", "Stop"};

namespace fs = std::filesystem;
fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

wheelcfg::Config g_cfg;
bool g_loaded = false, g_fresh = false, g_visible = false, g_pending_layout = false;
wheeldev::State g_st;
unsigned g_seen_open = 0, g_seen_status = 0;
std::string g_status = "Looking for wheels...", g_profile;
unsigned g_prev_buttons[2] = {};
int g_test = 0;
int g_index = 0;   // the row being drawn

struct Profile { std::string file, name, wheels, notes; };
std::vector<Profile> g_profiles;
std::string wheels_dir() { return S8(P(opts::exe_dir()) / "wheels"); }

struct Calibration { int pedal = -1; Uint64 t0 = 0; int start[AXES], ext[AXES]; } g_cal;

wheelcfg::Pedal& pedal(int i) { return i == 0 ? g_cfg.throttle : i == 1 ? g_cfg.brake : g_cfg.clutch; }
int axis(int a) { return g_st.open && a >= 0 && a < std::min(g_st.axes, AXES) ? g_st.axis[a] : 0; }
bool button(int b) { return g_st.open && b >= 0 && b < 64 && (g_st.pressed[b / 32] >> (b % 32) & 1); }
std::string axis_label(int a) { return a < 0 ? "None" : "Axis " + std::to_string(a + 1); }
std::string button_label(int b) { return b < 0 ? "-" : "Button " + std::to_string(b + 1); }

// ---------------------------------------------------------------- the ini's [wheel] section and profiles
void load_section(const Ini& ini, const char* section, wheelcfg::Config& c) {
    const auto keys = ini.keys(section);
    wheelcfg::load(c, [&](const char* k) -> const char* {
        for (const auto& kv : keys)
            if (kv.first == k) return kv.second.c_str();
        return nullptr;
    });
}

void read_profiles() {
    g_profiles.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(P(wheels_dir()), ec)) {
        if (e.path().extension() != ".ini") continue;
        Ini f;
        f.load(S8(e.path()));
        Profile pr{S8(e.path()), f.get("profile", "name", ""), f.get("profile", "wheels", ""), f.get("profile", "notes", "")};
        if (pr.name.empty()) pr.name = S8(e.path().stem());
        g_profiles.push_back(pr);
    }
    std::sort(g_profiles.begin(), g_profiles.end(), [](const Profile& a, const Profile& b) { return a.name < b.name; });
}
int find_profile(const std::string& name) {
    for (size_t i = 0; i < g_profiles.size(); i++)
        if (!SDL_strcasecmp(g_profiles[i].name.c_str(), name.c_str())) return (int)i;
    return -1;
}
// the profile made for this wheel (its vid:pid in the profile's list), -1 if none
int profile_for(unsigned vid, unsigned pid) {
    char id[16];
    snprintf(id, sizeof(id), "%04x:%04x", vid, pid);
    for (size_t i = 0; i < g_profiles.size(); i++)
        if (g_profiles[i].wheels.find(id) != std::string::npos) return (int)i;
    return -1;
}
void load_profile(int i) {
    if (i < 0 || i >= (int)g_profiles.size()) return;
    const Profile& pr = g_profiles[i];
    Ini f;
    f.load(pr.file);
    wheelcfg::Config c;
    load_section(f, "wheel", c);
    c.device = g_cfg.device;   // (a profile is the settings, not the choice of wheel)
    c.enabled = g_cfg.enabled;
    g_cfg = c;
    g_profile = pr.name;
    g_fresh = false;
    g_status = "Profile \"" + pr.name + "\" loaded." + (pr.notes.empty() ? "" : " " + pr.notes);
}
bool ask(const std::string& question) {
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"}};
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "F1 CE", question.c_str(), 2, buttons, nullptr};
    int hit = 0;
    return SDL_ShowMessageBox(&box, &hit) && hit == 1;
}
void save_profile(std::string name) {
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) return;
    std::string file_name;
    for (char c : name) file_name += strchr("\\/:*?\"<>|", c) ? '_' : c;
    const int existing = find_profile(name);
    const std::string file = existing >= 0 ? g_profiles[existing].file : S8(P(wheels_dir()) / P(file_name + ".ini"));
    if (existing >= 0 && !ask("Replace the profile \"" + name + "\" with the current settings?")) return;
    std::error_code ec;
    fs::create_directories(P(wheels_dir()), ec);
    Ini f;
    f.load(file);
    // the wheels it suits: the profile's list (if it had one) and the wheel connected now
    std::string wheels = existing >= 0 ? g_profiles[existing].wheels : "";
    if (g_st.open) {
        char id[16];
        snprintf(id, sizeof(id), "%04x:%04x", g_st.vid, g_st.pid);
        if (wheels.find(id) == std::string::npos) wheels += (wheels.empty() ? "" : ",") + std::string(id);
    }
    f.set("profile", "name", name);
    f.set("profile", "wheels", wheels);
    if (existing < 0) f.set("profile", "notes", "Saved by the launcher" + (g_st.open ? " for " + g_st.name : std::string()) + ".");
    wheelcfg::save(g_cfg, [&](const char* k, const std::string& v) {
        if (strcmp(k, "device") && strcmp(k, "enabled")) f.set("wheel", k, v);
    });
    if (!f.save(file)) {
        g_status = "Could not write " + file + ".";
        return;
    }
    g_fresh = false;
    g_profile = name;
    read_profiles();
    g_status = "Profile \"" + name + "\" saved in " + file + ".";
}
void delete_profile() {
    const int i = find_profile(g_profile);
    if (i < 0 || !ask("Delete the profile \"" + g_profiles[i].name + "\"?")) return;
    std::error_code ec;
    fs::remove(P(g_profiles[i].file), ec);
    g_profile.clear();
    read_profiles();
}

// another make of wheel: its usual axes and buttons
void vendor_layout(unsigned vid) {
    wheelcfg::Config c = g_cfg;
    wheelcfg::defaults_for(c, vid);
    std::copy(c.buttons, c.buttons + wheelcfg::BUTTONS, g_cfg.buttons);
    g_cfg.steer_axis = c.steer_axis;
    g_cfg.throttle = c.throttle;
    g_cfg.brake = c.brake;
    g_cfg.clutch = c.clutch;
}
// the wheel the thread opens for the selection
std::string target_guid() {
    if (!g_cfg.device.empty()) return g_cfg.device;
    for (const wheeldev::Device& d : g_st.devices)
        if (d.wheel) return d.guid;
    return {};
}
// the thread opened a wheel (or closed it)
void opened() {
    if (!g_st.open) return;
    if (g_fresh) {   // first look at the section: the wheel's profile, else its vendor's usual layout
        const int pr = profile_for(g_st.vid, g_st.pid);
        if (pr >= 0) load_profile(pr);
        else wheelcfg::defaults_for(g_cfg, g_st.vid);
    } else if (g_pending_layout && g_st.guid == target_guid() && g_st.vid) {
        vendor_layout(g_st.vid);
    }
    g_pending_layout = false;
}

// ---------------------------------------------------------------- pedal calibration: press the pedal fully
void calibrate_start(int p) {
    if (!g_st.open) {
        g_status = "Connect the wheel first.";
        return;
    }
    g_cal.pedal = p;
    g_cal.t0 = SDL_GetTicks();
    for (int a = 0; a < AXES; a++) g_cal.start[a] = g_cal.ext[a] = axis(a);
    g_status = std::string("Press the ") + PEDAL_NAMES[p] + " pedal all the way down, then let it go.";
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
        char b[160];
        snprintf(b, sizeof(b), "%s: axis %d, at rest %d, fully pressed %d.", PEDAL_NAMES[g_cal.pedal], best + 1, p.rest, p.full);
        g_status = b;
        g_cal.pedal = -1;
        g_fresh = false;
    } else if (SDL_GetTicks() - g_cal.t0 > 10000) {
        g_status = "No pedal moved: select the pedal's axis row, then press the pedal all the way and let it go.";
        g_cal.pedal = -1;
    }
}

// ---------------------------------------------------------------- live view
void draw_live(float w) {
    using namespace ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const int n = g_st.open ? std::min(g_st.buttons, 40) : 0, per_row = 10, button_rows = std::max((n + per_row - 1) / per_row, 1);
    const float ch = 22 * S, h = 24 * S + 58 * S + button_rows * (ch + 4 * S) + 6 * S;
    ImGui::Dummy(ImVec2(w, h));
    const ImU32 frame = IM_COL32(190, 190, 190, 255);
    text(dl, F.medium, 14, ImVec2(o.x + 4 * S, o.y), g_cal.pedal >= 0 ? RED : HELP, g_status, w - 8 * S);
    // steering: the game's lock inside the wheel's rotation, the wheel's position from the centre
    float y = o.y + 24 * S;
    {
        const ImVec2 a(o.x, y), b(o.x + w, y + 50 * S);
        dl->AddRectFilled(a, b, WHITE);
        dl->AddRect(a, b, frame);
        const float cx = (a.x + b.x) / 2, half = (b.x - a.x) / 2 - 2 * S;
        const float lock = (float)g_cfg.lock_deg(900), range = (float)g_cfg.range;
        const float lw = half * std::min(lock / range, 1.0f);
        dl->AddRectFilled(ImVec2(cx - lw, a.y + 2 * S), ImVec2(cx + lw, b.y - 18 * S), IM_COL32(236, 236, 236, 255));
        const float raw = (float)(axis(g_cfg.steer_axis) - g_cfg.steer_center) / 32767.0f * (g_cfg.steer_invert ? -1 : 1);
        const float x = cx + std::clamp(raw, -1.0f, 1.0f) * half;
        dl->AddRectFilled(ImVec2(std::min(cx, x), a.y + 7 * S), ImVec2(std::max(cx, x) + 1, b.y - 22 * S), RED);
        dl->AddRectFilled(ImVec2(cx - 1, a.y + 2 * S), ImVec2(cx + 1, b.y - 18 * S), INK);
        char t[96];
        const float game = g_cfg.steer(axis(g_cfg.steer_axis));
        snprintf(t, sizeof(t), "STEERING  %+.0f°  (GAME %+.0f%%)", raw * range / 2, std::clamp(game, -1.0f, 1.0f) * 100);
        text(dl, F.xbold, 13, ImVec2(cx - text_w(F.xbold, 13, t) / 2, b.y - 17 * S), INK, t);
    }
    y += 58 * S;
    // buttons and the hat
    const float s = 16 * S, hat_w = 3 * s + 10 * S, cell = std::min((w - hat_w) / per_row, 40 * S);
    for (int b = 0; b < n; b++) {
        const ImVec2 a(o.x + (b % per_row) * cell, y + (b / per_row) * (ch + 4 * S));
        const ImVec2 c(a.x + cell - 4 * S, a.y + ch);
        const bool on = button(b);
        dl->AddRectFilled(a, c, on ? RED : WHITE);
        dl->AddRect(a, c, frame);
        const std::string t = std::to_string(b + 1);
        text(dl, F.semi, 13, ImVec2((a.x + c.x - text_w(F.semi, 13, t)) / 2, a.y + 3 * S), on ? WHITE : INK, t);
    }
    if (!n) text(dl, F.medium, 14, ImVec2(o.x, y), GREY, "(no wheel: its buttons show here)");
    const unsigned char hat = g_st.open ? g_st.hat : 0;
    const float hx = o.x + w - 3 * s, hy = y;
    for (int yy = 0; yy < 3; yy++)
        for (int xx = 0; xx < 3; xx++) {
            if (xx == 1 && yy == 1) continue;
            const bool up = hat & SDL_HAT_UP, down = hat & SDL_HAT_DOWN, left = hat & SDL_HAT_LEFT, right = hat & SDL_HAT_RIGHT;
            const bool vert = yy == 0 ? up : yy == 2 ? down : false, horz = xx == 0 ? left : xx == 2 ? right : false;
            const bool on = (xx == 1 && vert) || (yy == 1 && horz) || (xx != 1 && yy != 1 && vert && horz);
            const ImVec2 a(hx + xx * s, hy + yy * s), c(a.x + s - 3 * S, a.y + s - 3 * S);
            dl->AddRectFilled(a, c, on ? RED : WHITE);
            dl->AddRect(a, c, frame);
        }
}

// a pedal's live bar over the last row (between its label and value): travel, min and max marks, what the game gets
void pedal_bar(int p, bool focused) {
    using namespace ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float w = b.x - a.x, vw = std::max(250 * S, w * 0.42f);
    const float x0 = a.x + 250 * S, x1 = b.x - vw - 18 * S - 64 * S, cy = (a.y + b.y) / 2;   // (room for the value: 64)
    if (x1 - x0 < 60 * S) return;
    const int raw = axis(pedal(p).axis);
    const ImU32 col = p == 1 ? IM_COL32(200, 40, 40, 255) : p == 0 ? IM_COL32(40, 160, 60, 255) : IM_COL32(25, 118, 210, 255);
    const ImU32 track = focused ? IM_COL32(70, 70, 70, 255) : IM_COL32(214, 214, 214, 255), mark = focused ? WHITE : INK;
    dl->AddRectFilled(ImVec2(x0, cy - 5 * S), ImVec2(x1, cy + 5 * S), track);
    dl->AddRectFilled(ImVec2(x0, cy - 5 * S), ImVec2(x0 + (x1 - x0) * pedal(p).travel(raw), cy + 5 * S), col);
    for (int m : {pedal(p).min, pedal(p).max}) {
        const float mx = x0 + (x1 - x0) * m / 100.0f;
        dl->AddRectFilled(ImVec2(mx - 1, cy - 9 * S), ImVec2(mx + 1, cy + 9 * S), mark);
    }
    char t[16];
    snprintf(t, sizeof(t), pedal(p).axis < 0 ? "-" : "%.0f%%", pedal(p).value(raw) * 100);
    text(dl, F.semi, 14, ImVec2(x1 + 8 * S, cy - 9 * S), focused ? WHITE : INK, t);
}

// ---------------------------------------------------------------- rows
ui::RowOut row(const std::string& label, const std::string& value, bool arrows, const char* help) {
    ImGui::PushID(g_index);
    const ui::RowOut r = ui::row_widget("r", label, value, g_index++, arrows, false, help, nullptr);
    ImGui::PopID();
    if (r.delta || r.activated) g_fresh = false;
    return r;
}
// a value stepped by left / right within [lo, hi]
bool slider(const std::string& label, int& v, int lo, int hi, int step, const char* unit, const char* help) {
    char t[32];
    snprintf(t, sizeof(t), "%d%s", v, unit);
    const ui::RowOut r = row(label, t, true, help);
    if (!r.delta) return false;
    v = std::clamp(v + r.delta * step, lo, hi);
    return true;
}
void toggle(const std::string& label, bool& v, const char* help) {
    const ui::RowOut r = row(label, v ? "On" : "Off", true, help);
    if (r.delta || r.activated) v = !v;
}
// a choice among n labels, stepped by left / right, listed by ✕
template <typename Label>
bool choice(const std::string& label, int& sel, int n, Label name, const char* help) {
    const ui::RowOut r = row(label, name(sel), true, help);
    if (r.delta) {
        sel = (sel + r.delta + n) % n;
        return true;
    }
    if (r.activated) {
        std::vector<std::string> items;
        for (int i = 0; i < n; i++) items.push_back(name(i));
        int* target = &sel;
        ui::open_list(items, sel, [target](int i) { *target = i; });
    }
    return false;
}
}  // namespace

// ---------------------------------------------------------------- interface
void start(const Ini& ini) {
    g_cfg = {};
    g_fresh = ini.keys("wheel").empty();
    if (g_fresh) wheelcfg::defaults_for(g_cfg, 0);
    load_section(ini, "wheel", g_cfg);
    g_profile = ini.get("wheel", "profile", "");
    read_profiles();
    g_loaded = true;
    wheeldev::start(g_cfg.device);
}

void save(Ini& ini) {
    if (!g_loaded || g_fresh) return;   // (the game takes the vendor's defaults itself)
    wheelcfg::save(g_cfg, [&](const char* k, const std::string& v) { ini.set("wheel", k, v); });
    for (const char* p : {"throttle", "brake", "clutch"}) ini.erase("wheel", std::string(p) + "_deadzone");   // (now min / max)
    ini.set("wheel", "profile", g_profile);
}

void frame(bool visible) {
    if (!g_loaded) return;
    if (visible != g_visible) {
        g_visible = visible;
        wheeldev::want([&](wheeldev::Want& w) {
            w.fast = visible;
            w.haptic = visible;
        });
        if (!visible) g_cal.pedal = -1;
    }
    g_st = wheeldev::state();
    if (!visible) return;
    if (g_st.status_gen != g_seen_status) {
        g_seen_status = g_st.status_gen;
        g_status = g_st.status;
    }
    if (g_st.open_gen != g_seen_open) {
        g_seen_open = g_st.open_gen;
        opened();
    }
    calibrate_poll();
}

int rows() {
    if (!g_loaded) {
        ui::header_widget("Looking for wheels...");
        return 0;
    }
    g_index = 0;
    draw_live(ImGui::GetContentRegionAvail().x);
    const unsigned prev[2] = {g_prev_buttons[0], g_prev_buttons[1]};
    g_prev_buttons[0] = g_st.pressed[0];
    g_prev_buttons[1] = g_st.pressed[1];
    auto newly_pressed = [&]() {   // a wheel button pressed this frame, -1 if none
        for (int b = 0; b < 64; b++)
            if ((g_st.pressed[b / 32] & ~prev[b / 32]) >> (b % 32) & 1) return b;
        return -1;
    };

    ui::header_widget("Wheel");
    {   // the device: Automatic, the connected ones, a saved one that is not plugged in
        std::vector<std::pair<std::string, std::string>> devs = {{"", "Automatic (the first wheel found)"}};
        for (const wheeldev::Device& d : g_st.devices) devs.push_back({d.guid, d.label});
        if (!g_cfg.device.empty() &&
            std::none_of(devs.begin(), devs.end(), [](const auto& d) { return d.first == g_cfg.device; }))
            devs.push_back({g_cfg.device, "Saved wheel (not connected)"});
        int sel = 0;
        for (size_t i = 0; i < devs.size(); i++)
            if (devs[i].first == g_cfg.device) sel = (int)i;
        const ui::RowOut r = row("Wheel", devs[sel].second, true, "The wheel the game uses.");
        auto pick = [](const std::string& guid) {
            g_cfg.device = guid;
            wheeldev::want([&](wheeldev::Want& w) { w.guid = guid; });
            // another make of wheel: its usual layout, now if it is the open one, else once the thread opened it
            g_pending_layout = !(g_st.open && g_st.guid == target_guid());
            if (!g_pending_layout && g_st.vid) vendor_layout(g_st.vid);
        };
        if (r.delta) pick(devs[(sel + r.delta + devs.size()) % devs.size()].first);
        if (r.activated) {
            std::vector<std::string> items;
            for (const auto& d : devs) items.push_back(d.second);
            ui::open_list(items, sel, [devs, pick](int i) { pick(devs[i].first); });
        }
    }
    toggle("Use the wheel in the game", g_cfg.enabled,
           "The game sees it as a Logitech G25 (its own wheel support, with force feedback). With a wheel the game "
           "makes it the player's controller.");
    {
        const ui::RowOut r = row("Profile", g_profile.empty() ? "(none)" : g_profile, false,
                                 "Settings for a wheel, kept in the wheels folder next to the launcher: select to list them, "
                                 "picking one loads it.");
        if (r.activated && !g_profiles.empty()) {
            std::vector<std::string> items;
            for (const Profile& p : g_profiles) items.push_back(p.name);
            ui::open_list(items, std::max(find_profile(g_profile), 0), [](int i) { load_profile(i); });
        }
        if (row("Save as profile", "Name it...", false, "Keeps the current settings as a profile (the same name replaces it).").activated)
            ui::open_edit(g_profile, [](const std::string& name) { save_profile(name); });
        if (row("Delete profile", g_profile.empty() ? "-" : g_profile, false, "Deletes the profile shown above.").activated)
            delete_profile();
        if (row("Defaults for this wheel", "Reset", false, "Its vendor's usual axes, buttons and forces.").activated) {
            const std::string dev = g_cfg.device;
            const bool en = g_cfg.enabled;
            g_cfg = {};
            g_cfg.device = dev;
            g_cfg.enabled = en;
            wheelcfg::defaults_for(g_cfg, g_st.open ? g_st.vid : 0x044f);
        }
    }

    ui::header_widget("Steering");
    {
        int a = g_cfg.steer_axis + 1;
        choice("Steering axis", a, AXES + 1, [](int i) { return axis_label(i - 1); }, "The wheel's steering axis.");
        g_cfg.steer_axis = a - 1;
    }
    toggle("Invert steering", g_cfg.steer_invert, "For wheels whose axis goes the other way.");
    if (row("Centre", "Set now", false, "Hold the wheel straight and select this row: its position becomes the centre.").activated)
        g_cfg.steer_center = axis(g_cfg.steer_axis);
    slider("Wheel rotation", g_cfg.range, 180, 2520, 10, "°",
           "Lock to lock, as set in the wheel's own control panel or driver (Thrustmaster, Logitech G HUB, Fanatec, "
           "MOZA Pit House...).");
    {
        int li = 0;
        for (int i = 0; i < LOCK_N; i++)
            if (LOCKS[i] == g_cfg.lock) li = i;
        choice("Steering lock", li, LOCK_N,
               [](int i) { return LOCKS[i] ? std::to_string(LOCKS[i]) + "°" : std::string("Game's (900°)"); },
               "The wheel turn that gives full steering in the game. Smaller: quicker steering (the rest of the "
               "wheel's rotation is the soft lock).");
        g_cfg.lock = LOCKS[li];
    }
    slider("Steering dead zone", g_cfg.steer_deadzone, 0, 10, 1, "%", "Wheel movement around the centre that does nothing.");

    const char* range_help = "The part of the pedal's travel the game uses: below min it reads as released, from max on "
                             "as fully pressed (the marks on the bar).";
    for (int i = 0; i < 3; i++) {
        wheelcfg::Pedal& p = pedal(i);
        const std::string name = PEDAL_NAMES[i];
        ui::header_widget(name + " pedal");
        const bool cal = g_cal.pedal == i;
        bool focused = false;
        ImGui::PushID(g_index);
        ui::RowOut r = ui::row_widget("r", "Axis", cal ? "Press it fully..." : axis_label(p.axis), g_index++, true, false,
                                      "Select: press the pedal all the way down and let it go, to find its axis and range. "
                                      "Left / right: pick the axis by hand.", &focused);
        ImGui::PopID();
        pedal_bar(i, focused);
        if (r.activated) calibrate_start(i);
        if (r.delta) {   // a pedal picked by hand: the usual ends (released = top of the axis)
            p.axis = std::clamp(p.axis + r.delta, -1, AXES - 1);
            p.rest = 32767;
            p.full = -32768;
            g_fresh = false;
        }
        for (int m = 0; m < 2; m++) {
            int& v = m ? p.max : p.min;
            const int lo = m ? p.min + wheelcfg::PEDAL_MIN_SPAN : 0, hi = m ? 100 : p.max - wheelcfg::PEDAL_MIN_SPAN;
            focused = false;
            ImGui::PushID(g_index);
            r = ui::row_widget("r", m ? "Max" : "Min", std::to_string(v) + "%", g_index++, true, false, range_help, &focused);
            ImGui::PopID();
            pedal_bar(i, focused);
            if (r.delta) {
                v = std::clamp(v + r.delta, lo, hi);
                g_fresh = false;
            }
        }
    }

    ui::header_widget("Force feedback");
    toggle("Force feedback", g_cfg.ffb, "The game's forces on the wheel.");
    toggle("Invert forces", g_cfg.ffb_invert, "For wheels that push the wrong way.");
    toggle("Soft lock", g_cfg.soft_lock, "Pushes back when the wheel turns past the game's steering lock.");
    for (const Gain& g : GAINS)
        if (slider(g.label, g_cfg.*g.v, 0, g.max, g.max >= 100 ? 5 : 1, "%", g.help) && g.v == &wheelcfg::Config::gain)
            wheeldev::set_gain(g_cfg.gain);
    {
        int fixed = g_cfg.autocenter >= 0;
        choice("Auto-centre", fixed, 2, [](int i) { return std::string(i ? "Fixed spring" : "By the game"); },
               "The game's centring spring, or a fixed one of the strength below.");
        if (fixed && g_cfg.autocenter < 0) g_cfg.autocenter = 30;
        if (!fixed) g_cfg.autocenter = -1;
        if (fixed) slider("Auto-centre strength", g_cfg.autocenter, 0, 100, 5, "%", "The fixed centring spring's strength.");
    }
    {
        const ui::RowOut r = row("Test effect", TESTS[g_test], true,
                                 "Left / right: pick an effect. Select: plays it with the settings above.");
        if (r.delta) g_test = (g_test + r.delta + 5) % 5;
        if (r.activated) wheeldev::test(g_cfg, g_test);
    }
    ui::header_widget("Buttons (select a row, then press the wheel's button)");
    auto button_row = [&](const std::string& label, int& b, const char* help) {
        int v = b + 1;
        bool focused = false;
        ImGui::PushID(g_index);
        const ui::RowOut r = ui::row_widget("r", label, button_label(b), g_index++, true, false, help, &focused);
        ImGui::PopID();
        if (r.delta) v = (v + r.delta + BUTTON_CHOICES + 1) % (BUTTON_CHOICES + 1);
        if (r.activated) {
            std::vector<std::string> items;
            for (int i = 0; i <= BUTTON_CHOICES; i++) items.push_back(button_label(i - 1));
            int* target = &b;
            ui::open_list(items, v, [target](int i) { *target = i - 1; g_fresh = false; });
        }
        if (focused && g_st.open)
            if (const int pressed = newly_pressed(); pressed >= 0 && pressed < BUTTON_CHOICES) v = pressed + 1;
        if (v != b + 1) {
            b = v - 1;
            g_fresh = false;
        }
    };
    for (int i = 0; i < wheelcfg::BUTTONS; i++)
        button_row(wheelcfg::BUTTON_NAMES[i], g_cfg.buttons[i], "The wheel's button for this game button. The wheel's hat is the d-pad.");
    ui::header_widget("H-shifter (optional; without it the paddles shift)");
    for (int i = 0; i < wheelcfg::GEARS; i++)
        button_row(i ? "Gear " + std::to_string(i) : std::string("Reverse"), g_cfg.gears[i], "The shifter position's button.");

    return g_index;
}

void release() {
    g_pending_layout = false;
    g_cal.pedal = -1;
    wheeldev::close_and_wait();
}
void resume() {
    if (!g_loaded) return;
    wheeldev::want([](wheeldev::Want& w) { w.open = true; });
}

std::string name() { return g_st.open ? g_st.name : std::string(); }
}  // namespace wheel
