// Graphics options of the launcher's Video Settings that change what the game draws. Hooks in tools/recomp.py HOOKS.
//  - Motion blur: "Motion Blur.Enable" (byte +0x418 of the Post Effects settings, DefaultPostEffects.txt) is read only
//    through its getter 0x447704, by foaTrackModel's render (0x2FB3A4) to choose between the motion-blur pass and the
//    plain copy (0x4475AC). The game clears it itself in some camera modes (setter 0x447710 from 0x34AF1C), so the
//    off path is one the game uses. F1CE_MOTION_BLUR=0 makes the getter return 0.
//  - Heat haze: the shimmer behind the cars is the particle effect Effects/EngineHeat.bpe, index 2 of the effect
//    manager's 28 pools (names at 0x10139B28, 44 instances: car effect slots 4 and 5, one per exhaust side). A car
//    takes an instance with 0x2F2CEC(manager, index) in 0x322714, which already copes with an empty pool (returns 0).
//    F1CE_HEAT_HAZE=0 makes that call return 0 for index 2, so the effect is never spawned.
//  - Car detail distance: a car's level of detail comes from its squared distance to the camera (car +14020, scaled
//    by the field of view) in 0x3065F0 and 0x312900 (which stores it at car +13696): LOD 0 up to 15 m (the float 225
//    at TOC 0x70E4A8), then one more level past each distance of the table at 0x1000E664 (25, 50 m), up to the car's
//    level count (car +12960). Only these two functions read either constant. F1CE_CAR_LOD=<k> multiplies the three
//    distances by k (2 = full detail up to 30 m), patched on the first call of either function.
#include <bit>
#include <cstdlib>

#include "kernel.h"

namespace {
bool env_off(const char* name) {
    const char* e = getenv(name);
    return e && atoi(e) == 0;
}
const bool g_no_motion_blur = env_off("F1CE_MOTION_BLUR");
const bool g_no_heat_haze = env_off("F1CE_HEAT_HAZE");
constexpr u32 ENGINE_HEAT = 2;
constexpr u32 LOD0_DIST2 = 0x70E4A8, LOD_DISTS = 0x1000E664;   // 15 m squared; 25 m, 50 m

float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }
}  // namespace

// 0x447704: Post Effects -> Motion Blur.Enable
bool motion_blur_get_hook(PPUContext& c) {
    if (!g_no_motion_blur) return false;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("effects: motion blur off"); }
    c.r[3] = 0;
    return true;
}
// 0x2F2CEC: take an instance of effect r4 from the manager r3's pool (0: none free)
bool effect_take_hook(PPUContext& c) {
    if (!g_no_heat_haze || (u32)c.r[4] != ENGINE_HEAT) return false;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("effects: heat haze off"); }
    c.r[3] = 0;
    return true;
}
// 0x3065F0, 0x312900: a car's level of detail (main thread)
bool car_lod_hook(PPUContext&) {
    static bool patched = false;
    if (patched) return false;
    patched = true;
    const float k = getenv("F1CE_CAR_LOD") ? (float)atof(getenv("F1CE_CAR_LOD")) : 1.0f;
    if (k <= 0 || k == 1) return false;
    if (ldf(LOD0_DIST2) != 225.0f || ldf(LOD_DISTS) != 25.0f || ldf(LOD_DISTS + 4) != 50.0f) {
        LOG_W("car lod: unexpected distance table, F1CE_CAR_LOD ignored");
        return false;
    }
    stf(LOD0_DIST2, 225.0f * k * k);
    stf(LOD_DISTS, 25.0f * k);
    stf(LOD_DISTS + 4, 50.0f * k);
    LOG_I("car lod: distances %.0f, %.0f, %.0f m", 15.0f * k, 25.0f * k, 50.0f * k);
    return false;
}
