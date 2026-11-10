// In-game cameras (PlayerCam, class vtable 0x100475f0).
// The player camera object (constructor 0x36fa28) follows the car of controller +0x14 (car = controller +0x24, its
// world matrix at +0xb0: rows right, up, forward, position). Its camera list (+0x168 .. +0x16c, ints) holds the camera
// ids the player can cycle through; +0x80 is the index of the current one. The ids are the game's camera types:
// 0 external, 1 external far, 2 TV pod, 3 TV pod offset, 4 cockpit, 5 nose (6/7 with nose damage), 0xb..0xd the
// split-screen variants (game object +0x6fb4.., Lua options "cameranose" ..). Each car carries a camera set
// (controller +0x58 -> PlayerCam +0x160): one 0x40-byte record per id:
//   +0x00 position and +0x10 look-at point in car space (x right, y up, z forward; metres), +0x20 half the vertical
//   field of view in radians, +0x24 near plane, +0x34 shake flag, +0x38 flags, +0x3c 1 = chase camera (0x36cc94,
//   smoothed, collision tested), 0 = fixed to the car (0x36ead8).
// The per-frame update is vtable slot 7 = 0x36f0e8(PlayerCam, output): it writes the camera's world matrix to
// output +0x80 (rows right, up, forward, position) and its inverse, the view matrix, to +0xc0; +0x100 half fov,
// +0x104 near, +0x108 far. 0x36f79c(PlayerCam, index) selects a camera (resets the smoothing state); 0x36f83c
// selects the profile's default camera.
//
// Changes here:
//  * Onboard TV camera (F1CE_TVCAM=1, default): camera 3 (the second TV pod camera of the cycle) becomes a broadcast
//    style T-cam, placed relative to the car's own TV pod camera 2: F1CE_TVCAM_UP / F1CE_TVCAM_BACK / F1CE_TVCAM_SIDE
//    (metres), F1CE_TVCAM_PITCH (degrees down), F1CE_TVCAM_FOV (vertical field of view in degrees). Values saved in
//    tvcam_tune.ini (F1CE_TVCAM_FILE) in the working directory override these.
//    F1CE_TVCAM_TUNE=1: the numpad tunes the camera live (host::debug_keys): 8/2 up/down, 4/6 left/right, 9/3 back/
//    forward, 7/1 tilt up/down, +/- field of view, 0 back to the defaults, 5 prints the values; every change is printed
//    on the console and saved to the file.
//  * The external (chase) cameras turn around the car with the right stick (F1CE_CAM_ORBIT=1, default): left/right
//    up to 180 degrees (behind = looking at the front of the car), up/down raises and lowers the view; the camera
//    springs back when the stick is released. F1CE_CAM_ORBIT_TEST=<yaw>,<pitch> (degrees) holds a fixed orbit.
//  * F1CE_CAM_LOG=1 logs the state once a second; F1CE_CAMERA=<id> forces that camera id (test aids).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host.h"
#include "kernel.h"

void sub_0036F0E8(PPUContext& __restrict c);
void sub_0036F79C(PPUContext& __restrict c);

namespace {
constexpr float kPi = 3.14159265358979f;
bool env_on(const char* name, bool def) {
    const char* e = getenv(name);
    return e ? atoi(e) != 0 : def;
}
float env_f(const char* name, float def) {
    const char* e = getenv(name);
    return e && *e ? (float)atof(e) : def;
}
bool cam_log() {
    static const bool v = env_on("F1CE_CAM_LOG", false);
    return v;
}
int forced_camera() {
    static const int v = getenv("F1CE_CAMERA") ? atoi(getenv("F1CE_CAMERA")) : -1;
    return v;
}
float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float f) { st32(ea, std::bit_cast<u32>(f)); }

struct V3 { float x, y, z; };
V3 ldv(u32 ea) { return {ldf(ea), ldf(ea + 4), ldf(ea + 8)}; }
void stv(u32 ea, V3 v) { stf(ea, v.x); stf(ea + 4, v.y); stf(ea + 8, v.z); }
bool operator==(V3 a, V3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 rot_y(V3 v, float a) {   // about the world's up axis
    const float c = std::cos(a), s = std::sin(a);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
V3 rot_axis(V3 v, V3 k, float a) {   // Rodrigues, k unit
    const float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

// ---- onboard TV camera -------------------------------------------------------------------------------------------
// defaults matched on the Honda against a 2005 BAR Honda onboard shot: helmet bottom centre, horizon in the upper third,
// the front wheels either side of the nose
struct TvCam { float up = 0.10f, back = 0.05f, side = 0.0f, pitch = 20.0f, fov = 68.0f; };
const TvCam kTvDefault;
TvCam g_tv;
bool g_tv_on = true, g_tv_tune = false;
std::string g_tv_file;

void tv_print(const char* why) {
    LOG_W("TV camera %s: up %.3f back %.3f side %.3f pitch %.1f fov %.1f  (F1CE_TVCAM_UP=%.3f F1CE_TVCAM_BACK=%.3f "
          "F1CE_TVCAM_SIDE=%.3f F1CE_TVCAM_PITCH=%.1f F1CE_TVCAM_FOV=%.1f)",
          why, g_tv.up, g_tv.back, g_tv.side, g_tv.pitch, g_tv.fov, g_tv.up, g_tv.back, g_tv.side, g_tv.pitch, g_tv.fov);
}
void tv_save() {
    FILE* f = fopen(g_tv_file.c_str(), "w");
    if (!f) { LOG_W("TV camera: cannot write %s", g_tv_file.c_str()); return; }
    fprintf(f, "# onboard TV camera (F1CE_TVCAM_TUNE), relative to the car's TV pod camera; delete to return to the defaults\n"
               "up=%.3f\nback=%.3f\nside=%.3f\npitch=%.1f\nfov=%.1f\n", g_tv.up, g_tv.back, g_tv.side, g_tv.pitch, g_tv.fov);
    fclose(f);
}
bool tv_load() {
    FILE* f = fopen(g_tv_file.c_str(), "r");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[32]; float v;
        if (sscanf(line, " %31[a-z] = %f", key, &v) != 2) continue;
        if (!strcmp(key, "up")) g_tv.up = v;
        else if (!strcmp(key, "back")) g_tv.back = v;
        else if (!strcmp(key, "side")) g_tv.side = v;
        else if (!strcmp(key, "pitch")) g_tv.pitch = v;
        else if (!strcmp(key, "fov")) g_tv.fov = v;
    }
    fclose(f);
    return true;
}
void tv_init() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = getenv("F1CE_TVCAM");   // 0 off, 1 on, "tune" on with the numpad tuning (launcher)
    const bool tune_word = e && !strcmp(e, "tune");
    g_tv_on = !e || tune_word || atoi(e) != 0;
    g_tv_tune = tune_word || env_on("F1CE_TVCAM_TUNE", false);
    g_tv.up = env_f("F1CE_TVCAM_UP", g_tv.up); g_tv.back = env_f("F1CE_TVCAM_BACK", g_tv.back);
    g_tv.side = env_f("F1CE_TVCAM_SIDE", g_tv.side); g_tv.pitch = env_f("F1CE_TVCAM_PITCH", g_tv.pitch);
    g_tv.fov = env_f("F1CE_TVCAM_FOV", g_tv.fov);
    g_tv_file = getenv("F1CE_TVCAM_FILE") ? getenv("F1CE_TVCAM_FILE") : "tvcam_tune.ini";
    if (tv_load()) tv_print((g_tv_file + " loaded").c_str());
    else if (g_tv_tune) tv_print("tuning (numpad 8/2 up/down, 4/6 side, 9/3 back/forward, 7/1 tilt, +/- fov, 0 reset, 5 print)");
}
// the numpad moves the camera: 0.15 m/s, 10 deg/s tilt, 15 deg/s field of view
void tv_tune() {
    static u64 last = 0, last_print = 0, changed_at = 0;
    static bool dirty = false, print_held = false;
    const u64 now = time_usec();
    const float dt = last ? std::min((now - last) / 1e6f, 0.1f) : 0;
    last = now;
    const u32 k = host::debug_keys();
    TvCam t = g_tv;
    if (k & host::DK_UP) t.up += 0.15f * dt;
    if (k & host::DK_DOWN) t.up -= 0.15f * dt;
    if (k & host::DK_RIGHT) t.side += 0.15f * dt;
    if (k & host::DK_LEFT) t.side -= 0.15f * dt;
    if (k & host::DK_BACK) t.back += 0.15f * dt;
    if (k & host::DK_FWD) t.back -= 0.15f * dt;
    if (k & host::DK_PITCH_DOWN) t.pitch += 10 * dt;
    if (k & host::DK_PITCH_UP) t.pitch -= 10 * dt;
    if (k & host::DK_FOV_UP) t.fov += 15 * dt;
    if (k & host::DK_FOV_DOWN) t.fov -= 15 * dt;
    if (k & host::DK_RESET) t = kTvDefault;
    t.fov = std::clamp(t.fov, 20.0f, 120.0f); t.pitch = std::clamp(t.pitch, -30.0f, 80.0f);
    if (std::memcmp(&t, &g_tv, sizeof t) != 0) {
        g_tv = t;
        dirty = true;
        changed_at = now;
        if (now - last_print > 250000) { last_print = now; tv_print("set"); }
    }
    if (dirty && now - changed_at > 300000) { dirty = false; tv_save(); tv_print("saved"); }
    const bool print = (k & host::DK_PRINT) != 0;
    if (print && !print_held) tv_print("values");
    print_held = print;
}
// Camera 3's record of the set is rewritten from camera 2's (the car's own TV pod position) whenever it does not hold
// the wanted values: a new car (next race) brings a new set, a tuning key changed them.
void apply_tvcam(u32 set) {
    const u32 rec2 = set + 2 * 0x40, rec3 = set + 3 * 0x40;
    const V3 base = ldv(rec2);
    const V3 pos = base + V3{g_tv.side, g_tv.up, -g_tv.back};
    const float p = g_tv.pitch * kPi / 180;
    const V3 at = pos + V3{0, -std::sin(p), std::cos(p)} * 2.5f;
    const float half = g_tv.fov * kPi / 360;
    if (ldv(rec3) == pos && ldv(rec3 + 0x10) == at && ldf(rec3 + 0x20) == half && ld32(rec3 + 0x3c) == 0) return;
    LOG_I("cam: TV camera 3 of set %08x: pod (%.3f %.3f %.3f) -> pos (%.3f %.3f %.3f) at (%.3f %.3f %.3f) fov %.1f "
          "(was pos %.3f %.3f %.3f at %.3f %.3f %.3f fov %.1f)",
          set, base.x, base.y, base.z, pos.x, pos.y, pos.z, at.x, at.y, at.z, g_tv.fov, ldf(rec3), ldf(rec3 + 4),
          ldf(rec3 + 8), ldf(rec3 + 0x10), ldf(rec3 + 0x14), ldf(rec3 + 0x18), ldf(rec3 + 0x20) * 360 / kPi);
    stv(rec3, pos); stf(rec3 + 0xc, 1.0f);
    stv(rec3 + 0x10, at); stf(rec3 + 0x1c, 1.0f);
    stf(rec3 + 0x20, half);
    st32(rec3 + 0x3c, 0);   // fixed to the car
    st32(rec3 + 0x38, ld32(rec2 + 0x38));
    st32(rec3 + 0x34, ld32(rec2 + 0x34));
}

// ---- right-stick orbit of the chase cameras -----------------------------------------------------------------------
bool orbit_on() {
    static const bool v = env_on("F1CE_CAM_ORBIT", true);
    return v;
}
// the stick's target angles (radians): yaw about the world's up axis, pitch positive = view from higher up
void orbit_target(float* yaw, float* pitch) {
    static const bool test = getenv("F1CE_CAM_ORBIT_TEST") != nullptr;
    if (test) {
        static float ty = 0, tp = 0;
        static bool parsed = false;
        if (!parsed) { sscanf(getenv("F1CE_CAM_ORBIT_TEST"), "%f,%f", &ty, &tp); parsed = true; }
        *yaw = ty * kPi / 180; *pitch = tp * kPi / 180;
        return;
    }
    const host::PadState s = host::pad_state();
    float x = (s.rx - 128) / 127.0f, y = (s.ry - 128) / 127.0f;
    const float dead = 0.15f;
    auto dz = [dead](float v) { return std::fabs(v) < dead ? 0.0f : (v - std::copysign(dead, v)) / (1 - dead); };
    x = std::clamp(dz(x), -1.0f, 1.0f); y = std::clamp(dz(y), -1.0f, 1.0f);
    *yaw = x * kPi;             // full deflection = 180 degrees: looking at the car's front
    *pitch = -y * kPi / 4;      // stick up = view from higher up, up to 45 degrees
}
void apply_orbit(u32 out, u32 car) {
    static float yaw = 0, pitch = 0;
    static u64 last = 0;
    float ty, tp;
    orbit_target(&ty, &tp);
    const u64 now = time_usec();
    const float dt = last ? std::min((now - last) / 1e6f, 0.1f) : 0.1f;
    last = now;
    const float k = 1 - std::exp(-dt / 0.08f);   // follows the stick in about a quarter of a second
    yaw += (ty - yaw) * k; pitch += (tp - pitch) * k;
    if (std::fabs(yaw) < 1e-4f && std::fabs(pitch) < 1e-4f) return;
    const V3 C = ldv(car + 0xe0);
    V3 right = ldv(out + 0x80), up = ldv(out + 0x90), fwd = ldv(out + 0xa0), pos = ldv(out + 0xb0);
    V3 off = pos - C;
    right = rot_y(right, yaw); up = rot_y(up, yaw); fwd = rot_y(fwd, yaw); off = rot_y(off, yaw);
    // pitch about the (turned) right axis, limited so the camera stays above the car's ground
    const float lim_lo = -15 * kPi / 180, lim_hi = 70 * kPi / 180;
    const float p = std::clamp(pitch, lim_lo, lim_hi);
    up = rot_axis(up, right, p); fwd = rot_axis(fwd, right, p); off = rot_axis(off, right, p);
    pos = C + off;
    stv(out + 0x80, right); stv(out + 0x90, up); stv(out + 0xa0, fwd); stv(out + 0xb0, pos);
    // the view matrix: transposed rotation, translation = -pos * R^T
    stf(out + 0xc0, right.x); stf(out + 0xc4, up.x); stf(out + 0xc8, fwd.x);
    stf(out + 0xd0, right.y); stf(out + 0xd4, up.y); stf(out + 0xd8, fwd.y);
    stf(out + 0xe0, right.z); stf(out + 0xe4, up.z); stf(out + 0xe8, fwd.z);
    stf(out + 0xf0, -dot(pos, right)); stf(out + 0xf4, -dot(pos, up)); stf(out + 0xf8, -dot(pos, fwd));
}

// ---- diagnostics ---------------------------------------------------------------------------------------------------
void log_vec(const char* name, u32 ea) {
    LOG_I("cam:   %-8s %9.3f %9.3f %9.3f %9.3f", name, ldf(ea), ldf(ea + 4), ldf(ea + 8), ldf(ea + 12));
}
void log_state(u32 pc, u32 out) {
    const u32 ctl = ld32(pc + 0x14), car = ctl ? ld32(ctl + 0x24) : 0;
    const u32 list = ld32(pc + 0x168), list_end = ld32(pc + 0x16c), idx = ld32(pc + 0x80), set = ld32(pc + 0x160);
    const u32 n = list ? (list_end - list) / 4 : 0;
    char ids[128] = "";
    for (u32 i = 0; i < n && i < 16; i++) snprintf(ids + strlen(ids), sizeof(ids) - strlen(ids), " %d", (s32)ld32(list + i * 4));
    const s32 id = (list && idx < n) ? (s32)ld32(list + idx * 4) : -1;
    LOG_I("cam: pc %08x out %08x ctl %08x car %08x set %08x idx %u id %d list[%u]%s", pc, out, ctl, car, set, idx, id, n, ids);
    if (set && id >= 0) {
        const u32 rec = set + id * 0x40;
        for (int r = 0; r < 4; r++) {
            char name[8];
            snprintf(name, sizeof(name), "rec+%02x", r * 16);
            log_vec(name, rec + r * 16);
        }
        LOG_I("cam:   rec words +20 %08x +24 %08x +30 %08x +34 %08x +38 %08x +3c %08x", ld32(rec + 0x20), ld32(rec + 0x24),
              ld32(rec + 0x30), ld32(rec + 0x34), ld32(rec + 0x38), ld32(rec + 0x3c));
    }
    if (out) {
        log_vec("out+80", out + 0x80); log_vec("out+90", out + 0x90); log_vec("out+a0", out + 0xa0); log_vec("out+b0", out + 0xb0);
        log_vec("out+c0", out + 0xc0); log_vec("out+d0", out + 0xd0); log_vec("out+e0", out + 0xe0); log_vec("out+f0", out + 0xf0);
        LOG_I("cam:   out +100 %.4f +104 %.4f +108 %.1f +50 %.4f flags 44 %u 45 %u 47 %u 49 %u 4a %u 4b %u 4c %u 4d %u 4e %u",
              ldf(out + 0x100), ldf(out + 0x104), ldf(out + 0x108), ldf(out + 0x50), ld8(out + 0x44), ld8(out + 0x45),
              ld8(out + 0x47), ld8(out + 0x49), ld8(out + 0x4a), ld8(out + 0x4b), ld8(out + 0x4c), ld8(out + 0x4d), ld8(out + 0x4e));
    }
    if (car) {
        log_vec("car+b0", car + 0xb0); log_vec("car+c0", car + 0xc0); log_vec("car+d0", car + 0xd0); log_vec("car+e0", car + 0xe0);
    }
}
}  // namespace

// PlayerCam::Update (vtable slot 7): r3 PlayerCam, r4 the camera output
bool playercam_update_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside) return false;
    tv_init();
    const u32 pc = (u32)c.r[3], out = (u32)c.r[4];
    const u32 list = ld32(pc + 0x168), list_end = ld32(pc + 0x16c);
    const u32 n = list ? (list_end - list) / 4 : 0;
    if (forced_camera() >= 0) {
        const u32 idx = ld32(pc + 0x80);
        for (u32 i = 0; i < n; i++) {
            if ((s32)ld32(list + i * 4) != forced_camera()) continue;
            if (i != idx) {
                const u64 r3 = c.r[3], r4 = c.r[4];
                c.r[3] = pc; c.r[4] = i;
                inside = true;
                sub_0036F79C(c);
                inside = false;
                c.r[3] = r3; c.r[4] = r4;
                LOG_I("cam: forced camera id %d (index %u)", forced_camera(), i);
            }
            break;
        }
    }
    // +0x160 is refreshed by the update itself: when the output's controller (+0xc) differs from the cached one (+0x14,
    // a new car: Time Trial start, back to the menus after a race) the set becomes that controller's +0x58. Before the
    // update +0x160 can still point at the previous car's freed set, so take the set from the controller here.
    const u32 next_ctl = out ? ld32(out + 0xc) : 0;
    const u32 next_set = next_ctl ? ld32(next_ctl + 0x58) : 0;
    if (g_tv_on && g_tv_tune) tv_tune();
    if (g_tv_on && next_set) apply_tvcam(next_set);
    inside = true;
    sub_0036F0E8(c);
    inside = false;
    const u32 set = ld32(pc + 0x160);
    const u32 idx = ld32(pc + 0x80);
    const s32 id = (list && idx < n) ? (s32)ld32(list + idx * 4) : -1;
    const u32 ctl = ld32(pc + 0x14), car = ctl ? ld32(ctl + 0x24) : 0;
    if (orbit_on() && out && car && set && id >= 0 && ld32(set + id * 0x40 + 0x3c) != 0) apply_orbit(out, car);
    if (cam_log()) {
        static u64 next = 0;
        if (time_usec() >= next) {
            next = time_usec() + 1000000;
            log_state(pc, out);
        }
    }
    return true;
}

// PlayerCam::SetCamera (vtable slot 10): r3 PlayerCam, r4 index
bool playercam_setcam_hook(PPUContext& c) {
    if (cam_log()) LOG_I("cam: set camera index %u (pc %08x) from %08x", (u32)c.r[4], (u32)c.r[3], (u32)c.lr);
    return false;
}
