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
//  - LOD 0 only: the car's LAST level (car + (count-1)*0x10D0 + 0x30) is its shadow caster: 0x312900 hands it to the
//    scene every frame besides the level it draws, and the car shadow pass 0x6431F0 and the shadow bounds 0x63ED50
//    take that node. A car with a new level-0 model but the original lod1/lod2 then casts the old shape onto itself
//    (season packs). The game has a path for one-level cars (count 1: level 0 always, and it is also the last one).
//    F1CE_CAR_LOD=0 sets the count to 1 at the car's first update, for good (every reader must see the same last
//    level: with the count at 1 inside 0x312900 only, the shadow pass drew lod2's stale node and the car lost its
//    shadow), and makes 0x3065F0 return level 0. The levels are node lookups in the one model resource (0x30E2E0),
//    so lod1/lod2 left unused leak nothing.
//  - Post effects: the Post Effects settings (DefaultPostEffects.txt / a track's PostEffects.txt, parsed by 0x4583A0)
//    keep their switches as bytes: +0x418 Motion Blur.Enable, +0x419 Depth Of Field.Enable, +0x41B Bloom.Enable, +0x41C
//    Crob Rays.Enable (light rays), +0x41D Rain And Muck.Enable. The post-effects render 0x452E08 (r3: the settings'
//    handle, first word = the settings) tests them directly and skips a pass whose switch is 0, so clearing them at its
//    entry every frame (the game's setters may turn them on again with a scene or camera) takes the game's own off
//    paths. F1CE_DOF=0, F1CE_BLOOM=0, F1CE_LIGHT_RAYS=0; F1CE_POSTFX=0 turns off all three, motion blur and heat haze.
//  - Crowd: the track model's crowd object (+0x4068, sgCrowd.cpp) is only used by its render 0x2FB3A4, which calls the
//    update 0x4306C4, the animation step 0x431688 and the draw 0x433F00 (3D figures and billboards), and goes past all
//    three when there is no object: Time Trial and test sessions never load one. F1CE_CROWD=0 makes the three return
//    at once (the crowd's sound stays). ONLINE.BIN has no crowd loader and never calls these functions.
#include <bit>
#include <cstdlib>

#include "kernel.h"

namespace {
bool env_off(const char* name) {
    const char* e = getenv(name);
    return e && atoi(e) == 0;
}
const bool g_no_postfx = env_off("F1CE_POSTFX");
const bool g_no_motion_blur = g_no_postfx || env_off("F1CE_MOTION_BLUR");
const bool g_no_dof = g_no_postfx || env_off("F1CE_DOF");
const bool g_no_bloom = g_no_postfx || env_off("F1CE_BLOOM");
const bool g_no_light_rays = g_no_postfx || env_off("F1CE_LIGHT_RAYS");
const bool g_no_heat_haze = g_no_postfx || env_off("F1CE_HEAT_HAZE");
const bool g_no_crowd = env_off("F1CE_CROWD");
constexpr u32 ENGINE_HEAT = 2;
const bool g_lod0_only = getenv("F1CE_CAR_LOD") && atof(getenv("F1CE_CAR_LOD")) == 0;   // (not env_off: 0.5 is a scale)
constexpr u32 LOD0_DIST2 = 0x70E4A8, LOD_DISTS = 0x1000E664;   // 15 m squared; 25 m, 50 m
constexpr u32 CAR_LOD_COUNT = 0x32A0;

float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }
}  // namespace

// With motion blur off the game still draws its full-screen pass, as a plain copy (0x4475AC) of the resolved scene
// (02190000 in the race) back into the antialiased target with the same 10-tap shader: no visible change, but ~0.8 ms
// of GPU time a frame on a 5600G (2.6 ms on a Steam Deck) plus the target's MSAA resolves that feed it. The renderers
// leave its draws out (state changes still applied) when this returns true for the program's hash (hash_fp of its
// code, low 48 bits; the same game data everywhere).
bool gfx_skip_program(u64 fh) {
    return g_no_motion_blur && (fh & 0xFFFFFFFFFFFFull) == 0xAAA398817194ull;
}

// 0x447704: Post Effects -> Motion Blur.Enable
bool motion_blur_get_hook(PPUContext& c) {
    if (!g_no_motion_blur) return false;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("effects: motion blur off"); }
    c.r[3] = 0;
    return true;
}
// 0x452E08: post-effects render (main thread, once per frame), r3 = the settings' handle
bool post_effects_hook(PPUContext& c) {
    if (!g_no_dof && !g_no_bloom && !g_no_light_rays) return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_I("effects: depth of field %s, bloom %s, light rays %s", g_no_dof ? "off" : "on", g_no_bloom ? "off" : "on",
              g_no_light_rays ? "off" : "on");
    }
    const u32 settings = ld32((u32)c.r[3]);
    if (!settings) return false;
    if (g_no_dof) st8(settings + 0x419, 0);
    if (g_no_bloom) st8(settings + 0x41B, 0);
    if (g_no_light_rays) st8(settings + 0x41C, 0);
    return false;
}
// 0x2F2CEC: take an instance of effect r4 from the manager r3's pool (0: none free)
bool effect_take_hook(PPUContext& c) {
    if (!g_no_heat_haze || (u32)c.r[4] != ENGINE_HEAT) return false;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("effects: heat haze off"); }
    c.r[3] = 0;
    return true;
}
// 0x433F00, 0x431688, 0x4306C4: the crowd's draw, animation step and update (main thread, once per frame)
bool crowd_hook(PPUContext&) {
    if (!g_no_crowd) return false;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("crowd: off"); }
    return true;
}
// 0x3065F0, 0x312900: a car's level of detail (main thread, r3 = the car)
bool car_lod_hook(PPUContext& c) {
    if (g_lod0_only) {
        static bool logged = false;
        if (!logged) { logged = true; LOG_I("car lod: level 0 only"); }
        if (c.cur_fn == 0x3065F0) {
            c.r[3] = 0;
            return true;
        }
        const u32 car = (u32)c.r[3];
        if (ld32(car + CAR_LOD_COUNT) > 1) st32(car + CAR_LOD_COUNT, 1);
        return false;
    }
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
