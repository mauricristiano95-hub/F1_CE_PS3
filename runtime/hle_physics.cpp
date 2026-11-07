// Car physics: the grid shake with F1CE_60FPS (README "Grid shake").
// Debug: F1CE_PHYS_LOG=<s>,<ea hex>: <s> seconds into the first 3D scene, log the 4 floats at <ea> on entry to the tyre
// force function 0x11c150 (once per substep and car) for the car whose object holds <ea> (",all" appended: every car), 400
// calls.
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "kernel.h"
#include "rsx.h"

// Debug: F1CE_PHYS_SNAP=<s>,<car hex>: <s> seconds into the first 3D scene, that car object (0x1390 bytes) and its body
// object (pointer at +0x140, 0x800 bytes) at the start of 12 consecutive substeps (snap_car_<k>.bin, snap_body_<k>.bin)
static void phys_snap(PPUContext& c) {
    static const char* spec = getenv("F1CE_PHYS_SNAP");
    if (!spec) return;
    static const double after = atof(spec);
    static const u32 want = strchr(spec, ',') ? (u32)strtoul(strchr(spec, ',') + 1, nullptr, 16) : 0x367c8a90;
    const u32 car = (u32)c.r[3];
    if (car != want) return;
    static u64 since3d = 0;
    static int k = 0;
    if (k >= 12 || (rsx::g_last_frame_draws < 800 && !since3d)) return;
    const u64 now = time_usec();
    if (!since3d) since3d = now;
    if (now - since3d < (u64)(after * 1e6)) return;
    const u32 body = ld32(car + 0x140);
    char fn[48];
    snprintf(fn, sizeof(fn), "snap_car_%d.bin", k);
    if (FILE* f = fopen(fn, "wb")) { fwrite(gmem(car), 1, 0x1390, f); fclose(f); }
    snprintf(fn, sizeof(fn), "snap_body_%d.bin", k);
    if (FILE* f = fopen(fn, "wb")) { fwrite(gmem(body), 1, 0x800, f); fclose(f); }
    if (++k == 12) LOG_I("phys snap: car %08x body %08x, 12 substeps saved", car, body);
}

// Shake filter (F1CE_60FPS; F1CE_SHAKE_FILTER=0 turns it off). The player car's state swings with period 2 from substep
// to substep (the game's wheel spin integration, also on the original, where a frame is an even number of substeps). A
// frame of one tick (3 substeps) shows both phases. Between ticks, the car object holds a filtered state instead:
// x = s3 - (s3 - 2 s2 + s1) / 4 from the state at the start of the last two substeps (s1, s2) and at the end (s3), which
// removes a period-2 component exactly and leaves steady motion without delay (error a * dt^2 / 4, under 1 mm). The
// physics state is put back before the next tick, so the simulation is the original's.
// Filtered: each 16-byte quad of the car object whose 12 words (3 states) are all zero or normal floats; constant words
// come out bit-exact. Restored: each word that still holds the filtered value (other writers keep theirs).
bool shake_filter_on() {
    static const bool on = getenv("F1CE_60FPS") && !(getenv("F1CE_SHAKE_FILTER") && atoi(getenv("F1CE_SHAKE_FILTER")) == 0);
    return on;
}

namespace {
constexpr u32 CAR_SIZE = 0x1390;
struct ShakeCar {
    u32 car = 0;
    int substeps = 0;                 // substeps seen in this tick
    std::vector<u8> s1, s2;           // the car at the start of the last two substeps
    std::vector<u32> saved_ofs, saved_orig, saved_new;   // words replaced by the filter
    std::vector<u32> prev_ofs, prev_orig, prev_new;      // the same for the tick before (kept for ticks without substeps)
};
std::vector<ShakeCar> g_shake;
bool g_in_tick = false;

bool float_word(u32 w) {
    const u32 e = (w >> 23) & 0xFF;
    if (e == 0xFF) return false;              // NaN / infinity
    if (e == 0) return (w & 0x7FFFFFFF) == 0;  // zero yes, denormal (small integers, flags) no
    return e < 127 + 24;                       // |x| < 2^24
}

void shake_restore() {
    for (ShakeCar& s : g_shake) {
        for (size_t i = 0; i < s.saved_ofs.size(); i++)
            if (ld32(s.car + s.saved_ofs[i]) == s.saved_new[i]) st32(s.car + s.saved_ofs[i], s.saved_orig[i]);
        s.prev_ofs.swap(s.saved_ofs); s.prev_orig.swap(s.saved_orig); s.prev_new.swap(s.saved_new);
        s.saved_ofs.clear(); s.saved_orig.clear(); s.saved_new.clear();
        s.substeps = 0;
    }
}

void shake_apply() {
    for (ShakeCar& s : g_shake) {
        if (s.substeps == 0) {   // the tick did not move this car (some ticks run no substeps): the last filter again
            for (size_t i = 0; i < s.prev_ofs.size(); i++)
                if (ld32(s.car + s.prev_ofs[i]) == s.prev_orig[i]) {
                    st32(s.car + s.prev_ofs[i], s.prev_new[i]);
                    s.saved_ofs.push_back(s.prev_ofs[i]);
                    s.saved_orig.push_back(s.prev_orig[i]);
                    s.saved_new.push_back(s.prev_new[i]);
                }
            continue;
        }
        if (s.substeps < 2) continue;
        auto be = [](const u8* p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); };
        for (u32 q = 0; q < CAR_SIZE; q += 16) {
            u32 a[4], b[4], c3[4];
            bool ok = true, changed = false;
            for (int k = 0; k < 4; k++) {
                a[k] = be(&s.s1[q + 4 * k]);
                b[k] = be(&s.s2[q + 4 * k]);
                c3[k] = ld32(s.car + q + 4 * k);
                ok &= float_word(a[k]) && float_word(b[k]) && float_word(c3[k]);
                changed |= a[k] != c3[k] || b[k] != c3[k];
            }
            if (!ok || !changed) continue;
            for (int k = 0; k < 4; k++) {
                if (a[k] == c3[k] && b[k] == c3[k]) continue;
                const float x1 = std::bit_cast<float>(a[k]), x2 = std::bit_cast<float>(b[k]), x3 = std::bit_cast<float>(c3[k]);
                const u32 w = std::bit_cast<u32>(x3 - (x3 - 2.0f * x2 + x1) * 0.25f);
                if (w == c3[k]) continue;
                st32(s.car + q + 4 * k, w);
                s.saved_ofs.push_back(q + 4 * k);
                s.saved_orig.push_back(c3[k]);
                s.saved_new.push_back(w);
            }
        }
    }
}
}  // namespace

void sub_0017B200(PPUContext& __restrict c);

// the player car's tick (3 substeps through 0x116d38)
bool car_tick_hook(PPUContext& c) {
    if (g_in_tick || !shake_filter_on()) return false;
    shake_restore();
    g_in_tick = true;
    sub_0017B200(c);
    g_in_tick = false;
    shake_apply();
    return true;
}

// wheels, first function of each substep of a car
bool wheel_update_hook(PPUContext& c) {
    phys_snap(c);
    if (g_in_tick) {
        const u32 car = (u32)c.r[3];
        auto it = std::find_if(g_shake.begin(), g_shake.end(), [&](const ShakeCar& s) { return s.car == car; });
        if (it == g_shake.end()) {
            if (g_shake.size() >= 32) return false;
            g_shake.push_back({});
            it = g_shake.end() - 1;
            it->car = car;
            it->s1.resize(CAR_SIZE);
            it->s2.resize(CAR_SIZE);
            if (g_shake.size() == 1) LOG_I("shake filter on (first car %08x)", car);
        }
        it->s1.swap(it->s2);
        memcpy(it->s2.data(), gmem(car), CAR_SIZE);
        it->substeps++;
    }
    return false;
}

// Debug: F1CE_PHYS_PROBE=<s>,<ea hex>: the update of the quad at <ea> (a car object + 0x910) in 0x117ed0, 40 ticks
void probe_1187ec(PPUContext& c) {
    static const char* spec = getenv("F1CE_PHYS_PROBE");
    if (!spec) return;
    static const double after = atof(spec);
    static const u32 ea = strchr(spec, ',') ? (u32)strtoul(strchr(spec, ',') + 1, nullptr, 16) : 0x367c93a0;
    if ((u32)c.r[23] != ea) return;
    static u64 since3d = 0;
    static int n = 0;
    if (n >= 40 || (rsx::g_last_frame_draws < 800 && !since3d)) return;
    const u64 now = time_usec();
    if (!since3d) since3d = now;
    if (now - since3d < (u64)(after * 1e6)) return;
    n++;
    auto q = [](const v128& v) {
        static char b[4][96];
        static int k = 0;
        char* s = b[k++ & 3];
        snprintf(s, 96, "%9.4f %9.4f %9.4f %9.4f", vmx::F(v, 0), vmx::F(v, 1), vmx::F(v, 2), vmx::F(v, 3));
        return s;
    };
    LOG_I("probe: t %.3f  v0 %s | old %s | g %s", now / 1e6, q(c.v[0]), q(c.v[10]), q(c.v[13]));
    LOG_I("probe:   v6 %s | v8 %s | v1 %s | v11 %s", q(c.v[6]), q(c.v[8]), q(c.v[1]), q(c.v[11]));
    const u32 car = (u32)c.r[3];
    auto m = [&](u32 o) {
        v128 v;
        vmx::lvx(v, car + o);
        return q(v);
    };
    LOG_I("probe:   car %08x +510 %s | +6b0 %s | +380 %s", car, m(0x510), m(0x6b0), m(0x380));
}

// Debug (same F1CE_PHYS_PROBE): the constraint solve in 0x11ac0c just after its vrefp, for the car whose +0x910 is <ea>
void probe_11b05c(PPUContext& c) {
    static const char* spec = getenv("F1CE_PHYS_PROBE");
    if (!spec) return;
    static const double after = atof(spec);
    static const u32 ea = strchr(spec, ',') ? (u32)strtoul(strchr(spec, ',') + 1, nullptr, 16) : 0x367c93a0;
    if ((u32)c.r[30] != ea - 0x910 + 0x10) return;
    static u64 since3d = 0;
    static int n = 0;
    if (n >= 24 || (rsx::g_last_frame_draws < 800 && !since3d)) return;
    const u64 now = time_usec();
    if (!since3d) since3d = now;
    if (now - since3d < (u64)(after * 1e6)) return;
    n++;
    auto q = [](const v128& v) {
        static char b[6][96];
        static int k = 0;
        char* s = b[k++ % 6];
        snprintf(s, 96, "%10.4f %10.4f %10.4f %10.4f", vmx::F(v, 0), vmx::F(v, 1), vmx::F(v, 2), vmx::F(v, 3));
        return s;
    };
    LOG_I("solve: t %.3f denom %s | recip %s | num %s", now / 1e6, q(c.v[11]), q(c.v[1]), q(c.v[9]));
    LOG_I("solve:   target %s | v0 %s | old %s | f2 %.4f", q(c.v[31]), q(c.v[0]), q(c.v[12]), c.f[2].d);
}

bool tyre_forces_hook(PPUContext& c) {
    static const char* spec = getenv("F1CE_PHYS_LOG");
    if (!spec) return false;
    static const double after = atof(spec);
    static const u32 ea = strchr(spec, ',') ? (u32)strtoul(strchr(spec, ',') + 1, nullptr, 16) : 0x382725d0;
    static u64 since3d = 0;
    static int n = 0;
    struct Rec { u64 t; u32 car; float v[4]; };
    static std::vector<Rec> recs;
    if (n >= 400) return false;
    if (rsx::g_last_frame_draws < 800 && !since3d) return false;
    const u64 now = time_usec();
    if (!since3d) since3d = now;
    if (now - since3d < (u64)(after * 1e6)) return false;
    const u32 car = (u32)c.r[3];
    static std::vector<u32> cars;
    if (cars.size() < 32 && std::find(cars.begin(), cars.end(), car) == cars.end()) {
        cars.push_back(car);
        LOG_I("phys: car object %08x", car);
    }
    static const bool any_car = strstr(spec, ",all") != nullptr;
    if (!any_car && (ea < car || ea >= car + 0x4000)) return false;
    Rec r{now, car, {}};
    for (int i = 0; i < 4; i++) r.v[i] = std::bit_cast<float>(ld32(ea + 4 * i));
    recs.push_back(r);
    if (++n == 400) {
        for (const Rec& q : recs)
            LOG_I("phys: %10.3f ms car %08x  %12.4f %12.4f %12.4f %12.4f", (q.t - recs[0].t) / 1e3, q.car, q.v[0], q.v[1],
                  q.v[2], q.v[3]);
    }
    return false;
}
