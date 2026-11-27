// AI pace balancing and race pit strategy variety (README "AI").
//
// 1) Track AI pace (F1CE_AI_PACE, launcher "AI pace"):
//    The AI does not compute cornering speeds dynamically from car physics; instead, LoadTrackSpeedSegments (0x1D5F50)
//    loads a pre-baked table of 28-byte TrackSpeedSeg structs into TrackData + 0xEC ({ptr, cap, count}):
//      +0x00 float speed_center (m/s), +0x04 speed_right, +0x08 speed_left,
//      +0x0C len_right, +0x10 len_left, +0x14 len_center, +0x18 s16 startIdx, +0x1A s16 endIdx.
//    Across the 18 circuits, Sony Liverpool baked very inconsistent lateral-G targets: Suzuka sits at ~3.84 G median
//    (only 13% of corners below 2.5 G) and Melbourne is tuned aggressively (~1:25.5 Hard AI), while Magny-Cours
//    (29% of corners below 2.5 G, e.g. Estoril at 1.76 G / 202 km/h and Adelaide at 64.6 km/h), Bahrain (50% of corners
//    below 3.0 G, 3.15 G median), Hungary, and Nürburgring leave 3-4.5 s/lap on the table.
//    When enabled, track_ai_speeds_hook runs once after 0x1D5F50 loads the table, computes the 3D Menger curvature of
//    each corner from the track's own actualracingline spline (*(TrackData + 0x0C): +0x0C count, +0x18 vec4[]), and
//    lifts under-baked corner speeds up to the Melbourne/Suzuka benchmark while keeping Melbourne and Suzuka untouched
//    in "balanced" mode. speed_right and speed_left scale proportionally, and Easy/Medium/Hard difficulty (0xE22B0)
//    plus wet-weather scaling (0x100DB3B4) continue to apply on top as usual.
//    Values: unset / 0 / original = untouched; 1 / balanced = equalise weak tracks to Melbourne/Suzuka;
//            2 / fast = raise all tracks (including Melbourne/Suzuka) for experienced players;
//            <float> (0.70..1.25) = direct corner-speed multiplier on non-straight segments (< 84 m/s).
//
// 2) AI pit stop strategies (F1CE_AI_STRATEGIES, launcher "AI pit strategies"):
//    RaceStrategies/<track>_100.bin already defines valid 1-stop, 2-stop, and 3-stop lap windows loaded into
//    TrackLogic + 0x7C8 (1-stop), +0x7CC (2-stop), and +0x7D0 (3-stop) by 0xCC388. However, AssignAIPitStrategies
//    (0xCB0E4) skips its strategy randomiser branch (0xCB978) whenever race_distance == 1.0 (100% races), assigning
//    the single default stop count to all 11 teams (only staggering teammates by a few laps), and even at 75% only
//    flips between two slots.
//    When enabled (F1CE_AI_STRATEGIES=1 / realistic), ai_pit_strategies_hook lets 0xCB0E4 run first (preserving fuel-off
//    sessions, short races, and human player setup), then assigns a realistic 2006 F1 mix of 1-stop, 2-stop, and 3-stop
//    strategies to the AI cars (staggering teammates so they never double-stack) and calls 0x11E7B0(car, &strat) so
//    0xCC038 recalculates exact per-stint fuel loads automatically.
#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel.h"

// (weak: the multiplayer executable f1ce_online is translated from ONLINE.BIN without these functions; it also has none
// of the hooks below - tools/recomp.py installs them in the EBOOT's code only - so nothing calls them there)
__attribute__((weak)) void sub_001D5F50(PPUContext& __restrict c);
__attribute__((weak)) void sub_001C04DC(PPUContext& __restrict c);
__attribute__((weak)) void sub_001BFDB0(PPUContext& __restrict c);
__attribute__((weak)) void sub_000CB0E4(PPUContext& __restrict c);
__attribute__((weak)) void sub_0011E7B0(PPUContext& __restrict c);
__attribute__((weak)) void sub_000E22B0(PPUContext& __restrict c);
__attribute__((weak)) void sub_00113F90(PPUContext& __restrict c);
__attribute__((weak)) void sub_0062761C(PPUContext& __restrict c);

namespace {

float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }

// ------------------------------------------------------------------ AI pace balancing

enum class PaceMode { original, balanced, fast, custom };

struct PaceConfig {
    PaceMode mode = PaceMode::original;
    float custom_mult = 1.0f;
};

PaceConfig pace_config() {
    static const PaceConfig cfg = [] {
        PaceConfig c;
        const char* e = getenv("F1CE_AI_PACE");
        if (!e || !*e || strcmp(e, "0") == 0 || strcmp(e, "original") == 0) return c;
        if (strcmp(e, "1") == 0 || strcmp(e, "balanced") == 0) {
            c.mode = PaceMode::balanced;
        } else if (strcmp(e, "2") == 0 || strcmp(e, "fast") == 0) {
            c.mode = PaceMode::fast;
        } else {
            char* end = nullptr;
            float v = strtof(e, &end);
            if (end && end != e && std::isfinite(v) && v >= 0.70f && v <= 1.25f) {
                c.mode = PaceMode::custom;
                c.custom_mult = v;
            }
        }
        if (c.mode == PaceMode::balanced) LOG_I("ai pace: balanced (equalised to Melbourne/Suzuka benchmark)");
        else if (c.mode == PaceMode::fast) LOG_I("ai pace: fast (raised across all tracks)");
        else if (c.mode == PaceMode::custom) LOG_I("ai pace: custom corner multiplier %.3fx", c.custom_mult);
        return c;
    }();
    return cfg;
}

struct TrackCalibration {
    const char* name;
    const char* fkt_name; // track stem used in FakeTimes/<stem>_<easy|hard>.fkt (case-insensitive)
    u32 spline_len;       // exact actualracingline point count (unique across all 18 official tracks)
    float g_slow;         // target minimum lateral G at <= 30 m/s
    float g_fast;         // target minimum lateral G at >= 65 m/s
    float max_boost;      // upper clamp on any single corner's speed lift
    float corner_mul;     // gentle track-wide corner calibration (< 84 m/s)
    float fkt_scale;      // matching simulated-lap multiplier for FakeTimes/*.fkt in balanced mode
};

// Calibrated from 3D spline curvature + FakeTimes Hard AI benchmarks across all 18 tracks:
// Melbourne (5246 m) and Suzuka (5785 m) stay 1.00x as the reference difficulty ceiling.
// France (Magny-Cours), Bahrain, Hungary, and Nürburgring receive targeted curvature-floor + corner lifts (~2.8-3.7 s/lap);
// other below-benchmark circuits receive a conservative curvature-only floor for under-baked corners (< 2.8 G).
constexpr TrackCalibration kBalancedTracks[] = {
    {"Melbourne",   "melbourne",   5246, 0.00f, 0.00f, 1.00f, 1.000f, 1.000f},   // benchmark (untouched)
    {"Suzuka",      "suzuka",      5785, 0.00f, 0.00f, 1.00f, 1.000f, 1.000f},   // benchmark (untouched)
    {"France",      "magnycours",  4354, 3.20f, 3.65f, 1.16f, 1.045f, 0.962f},   // Estoril 1.76G, Adelaide 64.6 km/h
    {"Bahrain",     "bahrain",     5425, 3.20f, 3.65f, 1.16f, 1.040f, 0.961f},   // 50% corners < 3.0G, 3.15G median
    {"Hungary",     "hungaroring", 4307, 3.20f, 3.65f, 1.15f, 1.040f, 0.952f},   // slow corners 2.91G, wide track
    {"Nurburgring", "nurburgring", 5066, 3.20f, 3.65f, 1.15f, 1.035f, 0.961f},   // wide track (12.1m), conservative corners
    {"Brazil",      "interlagos",  4221, 3.00f, 3.45f, 1.12f, 1.015f, 0.970f},   // 51% corners < 3.0G
    {"Istanbul",    "istanbul",    5232, 3.00f, 3.45f, 1.12f, 1.015f, 0.976f},   // lowest median G (3.03G)
    {"Shanghai",    "shanghai",    5348, 3.20f, 3.70f, 1.16f, 1.042f, 0.962f},   // 669m of corners < 2.8G (-1.35s extra)
    {"Indy",        "indy",        4110, 3.00f, 3.45f, 1.12f, 1.040f, 0.963f},   // 25% corners < 2.5G (-0.80s extra)
    {"Hockenheim",  "hockenheim",  4526, 3.10f, 3.60f, 1.14f, 1.040f, 0.962f},   // (-1.25s extra)
    {"SanMarino",   "imola",       4856, 3.25f, 3.70f, 1.18f, 1.045f, 0.961f},   // Variante Alta 1.65G, Rivazza 2.29G, Tosa 2.33G
    {"Canada",      "montreal",    4283, 3.00f, 3.50f, 1.12f, 1.028f, 0.973f},   // (-0.65s extra)
    {"Monaco",      "montecarlo",  3283, 2.85f, 3.25f, 1.08f, 1.022f, 0.976f},   // tight barriers: safe +2.2% corner lift (-0.75s extra)
    {"Malaysia",    "sepang",      5471, 3.15f, 3.60f, 1.14f, 1.028f, 0.967f},   // fast sweepers T5/T6/T12/T13 (-1.55s extra)
    {"Silverstone", "silverstone", 5033, 2.90f, 3.35f, 1.10f, 1.000f, 0.987f},
    {"Spain",       "barcelona",   4536, 2.90f, 3.35f, 1.10f, 1.000f, 0.988f},
    {"Monza",       "monza",       5680, 3.10f, 3.60f, 1.14f, 1.030f, 0.979f},   // (-0.75s extra)
};

struct Vec3 {
    double x, y, z;
};

double menger_curvature(const std::vector<Vec3>& pts, int idx, int h) {
    const int n = (int)pts.size();
    const Vec3& a = pts[(idx - h + n) % n];
    const Vec3& b = pts[idx % n];
    const Vec3& c = pts[(idx + h) % n];
    const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    const double bcx = c.x - b.x, bcy = c.y - b.y, bcz = c.z - b.z;
    const double cax = a.x - c.x, cay = a.y - c.y, caz = a.z - c.z;
    const double ab = std::sqrt(abx * abx + aby * aby + abz * abz);
    const double bc = std::sqrt(bcx * bcx + bcy * bcy + bcz * bcz);
    const double ca = std::sqrt(cax * cax + cay * cay + caz * caz);
    if (ab < 0.1 || bc < 0.1 || ca < 0.1) return 0.0;
    const double acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
    const double cx = aby * acz - abz * acy;
    const double cy = abz * acx - abx * acz;
    const double cz = abx * acy - aby * acx;
    return 2.0 * std::sqrt(cx * cx + cy * cy + cz * cz) / (ab * bc * ca);
}

void apply_track_pace(u32 track_data, const PaceConfig& cfg) {
    if (!mem::readable(track_data, 0x100)) return;
    const u32 seg_ptr = ld32(track_data + 0xEC);
    const u32 seg_cnt = ld32(track_data + 0xF4);
    if (!seg_ptr || seg_cnt < 5 || seg_cnt > 2000 || !mem::readable(seg_ptr, seg_cnt * 28)) return;

    // Load actualracingline spline (*(track_data + 0x0C): +0x0C count, +0x18 vec4 points ptr)
    std::vector<Vec3> pts;
    const u32 spline = ld32(track_data + 0x0C);
    if (spline && mem::readable(spline, 0x20)) {
        const u32 n_pts = ld32(spline + 0x0C);
        const u32 pts_ptr = ld32(spline + 0x18);
        if (n_pts >= 500 && n_pts <= 20000 && pts_ptr && mem::readable(pts_ptr, n_pts * 16)) {
            pts.resize(n_pts);
            for (u32 i = 0; i < n_pts; i++) {
                const u32 p = pts_ptr + i * 16;
                pts[i] = {ldf(p), ldf(p + 4), ldf(p + 8)};
            }
        }
    }

    const u32 n_pts = (u32)pts.size();
    const char* track_name = "custom";
    float g_slow = 3.00f, g_fast = 3.45f, max_boost = 1.12f, corner_mul = 1.00f;

    if (cfg.mode == PaceMode::balanced) {
        bool matched = false;
        for (const auto& t : kBalancedTracks) {
            if (t.spline_len == n_pts) {
                track_name = t.name;
                g_slow = t.g_slow;
                g_fast = t.g_fast;
                max_boost = t.max_boost;
                corner_mul = t.corner_mul;
                matched = true;
                break;
            }
        }
        if (matched && g_slow <= 0.0f && corner_mul == 1.0f) {
            LOG_I("ai pace: %s (%um) kept at benchmark (1.00x)", track_name, n_pts);
            return;
        }
    } else if (cfg.mode == PaceMode::fast) {
        for (const auto& t : kBalancedTracks)
            if (t.spline_len == n_pts) { track_name = t.name; break; }
        g_slow = (n_pts == 3283) ? 3.05f : 3.30f;   // gentler on Monaco walls
        g_fast = (n_pts == 3283) ? 3.45f : 3.75f;
        max_boost = (n_pts == 3283) ? 1.12f : 1.18f;
        corner_mul = (n_pts == 3283) ? 1.02f : 1.045f;
    } else if (cfg.mode == PaceMode::custom) {
        g_slow = 0.0f;
        g_fast = 0.0f;
        max_boost = cfg.custom_mult;
        corner_mul = cfg.custom_mult;
    }

    std::vector<double> curv;
    if (g_slow > 0.0f && n_pts >= 500) {
        curv.resize(n_pts);
        for (u32 i = 0; i < n_pts; i++)
            curv[i] = 0.5 * (menger_curvature(pts, (int)i, 6) + menger_curvature(pts, (int)i, 10));
    }

    u32 changed = 0;
    double sum_ratio = 0.0;
    std::vector<double> ks;
    for (u32 i = 0; i < seg_cnt; i++) {
        const u32 s = seg_ptr + i * 28;
        const float sc = ldf(s);
        const float sr = ldf(s + 4);
        const float sl = ldf(s + 8);
        if (!std::isfinite(sc) || sc < 5.0f || sc >= 84.0f) continue;   // leave flat-out straights untouched

        double boost = corner_mul;
        if (!curv.empty() && sc < 80.0f) {
            const int s_idx = (s16)ld16(s + 0x18);
            const int e_idx = (s16)ld16(s + 0x1A);
            int slen = e_idx - s_idx;
            if (slen < 0) slen += (int)n_pts;
            if (slen >= 5 && slen < (int)n_pts / 2) {
                ks.clear();
                ks.reserve((size_t)slen);
                for (int k = 0; k < slen; k++) ks.push_back(curv[(size_t)((s_idx + k) % (int)n_pts)]);
                std::sort(ks.begin(), ks.end());
                const double k85 = ks[(size_t)(0.85 * (double)(ks.size() - 1))];
                if (k85 >= 0.0025) {
                    const double alpha = std::clamp(((double)sc - 30.0) / 35.0, 0.0, 1.0);
                    const double g_target = (double)g_slow + alpha * ((double)g_fast - (double)g_slow);
                    const double v_target = std::sqrt(g_target * 9.81 / k85);
                    if (v_target > (double)sc * boost)
                        boost = std::max(boost, std::min((double)max_boost, v_target / (double)sc));
                }
            }
        }

        const float new_sc = std::clamp((float)((double)sc * boost), 5.0f, 85.0f);
        const float ratio = new_sc / sc;
        if (std::fabs(ratio - 1.0f) > 0.001f) {
            stf(s, new_sc);
            if (std::isfinite(sr) && sr > 1.0f) stf(s + 4, std::min(sr * ratio, 85.0f));
            if (std::isfinite(sl) && sl > 1.0f) stf(s + 8, std::min(sl * ratio, 85.0f));
            changed++;
            sum_ratio += ratio;
        }
    }

    const double avg_pct = changed ? ((sum_ratio / (double)changed) - 1.0) * 100.0 : 0.0;
    LOG_I("ai pace: %s (%um) adjusted %u/%u corner segments (avg %+.1f%%)", track_name, n_pts, changed, seg_cnt, avg_pct);
}

std::string read_track_stem(u32 track_info) {
    if (!track_info || !mem::readable(track_info, 0x20)) return {};
    const s32 off = (s32)ld32(track_info + 0x1C);
    if (off < 0) return {};
    const u32 base = ld32(track_info + 0x00);
    const u32 str_ea = base + (u32)off;
    if (!str_ea || !mem::readable(str_ea, 1)) return {};
    std::string out;
    for (u32 i = 0; i < 64 && mem::readable(str_ea + i, 1); i++) {
        const u8 ch = ld8(str_ea + i);
        if (!ch) break;
        out.push_back((char)std::tolower(ch));
    }
    return out;
}

// Track the exact FakeTimes scale applied per FakeTimes object so career_trial_target_hook (0x62761C)
// can undo AI pace scaling for solo Career Trial target times without affecting AI Quali/Practice/Race pace.
std::unordered_map<u32, float> g_fkt_scales;
float g_last_fkt_scale = 1.0f;

void scale_faketimes(PPUContext& c, u32 fkt_obj, u32 track_info, const PaceConfig& cfg) {
    if (!fkt_obj || !mem::readable(fkt_obj, 792)) return;
    const std::string stem = read_track_stem(track_info);
    float scale = 1.0f;
    if (cfg.mode == PaceMode::balanced) {
        for (const auto& t : kBalancedTracks) {
            if (stem == t.fkt_name) { scale = t.fkt_scale; break; }
        }
    } else if (cfg.mode == PaceMode::fast) {
        float bal = 0.985f;
        for (const auto& t : kBalancedTracks) {
            if (stem == t.fkt_name) { bal = t.fkt_scale; break; }
        }
        scale = std::clamp(bal * 0.975f, 0.90f, 1.0f);
    } else if (cfg.mode == PaceMode::custom) {
        // Corner speed multiplier m moves lap time roughly by 1 / (1 + 0.55 * (m - 1))
        scale = std::clamp(1.0f / (1.0f + 0.55f * (cfg.custom_mult - 1.0f)), 0.85f, 1.20f);
    }
    if (std::fabs(scale - 1.0f) < 0.001f) {
        g_fkt_scales[fkt_obj] = 1.0f;
        g_last_fkt_scale = 1.0f;
        return;
    }

    // FakeTimes object stores 198 sector-time floats (792 bytes = 66 laps x 3 sectors) at +0x000..+0x317;
    // 0x1BFDB0(fkt_obj) rebuilds its derived lookup tables from those 198 floats.
    for (u32 i = 0; i < 198; i++) {
        const u32 ea = fkt_obj + i * 4;
        const float v = ldf(ea);
        if (std::isfinite(v) && v > 5.0f && v < 120.0f) stf(ea, v * scale);
    }
    const u64 r3 = c.r[3], lr = c.lr;
    const u32 fn = c.cur_fn;
    c.r[3] = fkt_obj;
    sub_001BFDB0(c);
    c.r[3] = r3;
    c.lr = lr;
    c.cur_fn = fn;
    g_fkt_scales[fkt_obj] = scale;
    g_last_fkt_scale = scale;
    LOG_I("ai pace: scaled FakeTimes (%s) by %.3fx", stem.empty() ? "?" : stem.c_str(), scale);
}

// ------------------------------------------------------------------ AI pit stop strategies

bool strategies_enabled() {
    static const bool on = [] {
        const char* e = getenv("F1CE_AI_STRATEGIES");
        const bool v = e && *e && strcmp(e, "0") != 0 && strcmp(e, "original") != 0;
        if (v) LOG_I("ai strategies: realistic variety enabled (1/2/3-stop mix)");
        return v;
    }();
    return on;
}

struct Strat4 {
    u8 lap[3] = {0, 0, 0};
    u8 stops = 0;
};

Strat4 read_slot(u32 ea) {
    Strat4 s;
    s.lap[0] = ld8(ea);
    s.lap[1] = ld8(ea + 1);
    s.lap[2] = ld8(ea + 2);
    s.stops = ld8(ea + 3);
    return s;
}

// Validate a loaded slot (fixes e.g. Monza 2-stop [30, 44] on 53 laps) or synthesise evenly spaced windows.
Strat4 make_base_strategy(u32 track_logic, int want_stops, int total_laps) {
    Strat4 s;
    if (want_stops >= 1 && want_stops <= 3) {
        const Strat4 loaded = read_slot(track_logic + 0x7C8 + (u32)(want_stops - 1) * 4);
        if (loaded.stops == (u8)want_stops) {
            bool ok = true;
            const int min_stint = std::max(4, total_laps / (want_stops + 3));
            int prev = 0;
            for (int i = 0; i < want_stops; i++) {
                const int lp = loaded.lap[i];
                if (lp - prev < min_stint || total_laps - lp < min_stint) { ok = false; break; }
                prev = lp;
            }
            if (ok && want_stops == 2 && loaded.lap[0] > (total_laps * 52) / 100) ok = false;   // Monza [30, 44]/53
            if (ok) return loaded;
        }
    }
    s.stops = (u8)want_stops;
    for (int i = 0; i < want_stops; i++) {
        // Place stops slightly past exact fractions (heavier first stint like real 2006 F1)
        const double frac = ((double)(i + 1) + 0.08) / (double)(want_stops + 1);
        s.lap[i] = (u8)std::clamp((int)std::lround(frac * (double)total_laps), 4, std::max(4, total_laps - 3));
    }
    return s;
}

Strat4 jitter_strategy(const Strat4& base, int delta, int total_laps, const Strat4* avoid) {
    Strat4 s = base;
    const int n = s.stops;
    const int min_lap = std::max(4, total_laps / 7);
    const int max_lap = (n == 1)
                            ? std::min(std::max(min_lap + 1, total_laps - 4), (total_laps * 58) / 100)
                            : std::max(min_lap + 1, total_laps - 4);
    const int min_gap = std::max(4, total_laps / (n + 3));

    for (int i = 0; i < n; i++) {
        int lp = (int)s.lap[i] + delta;
        lp = std::clamp(lp, min_lap, max_lap);
        if (i > 0 && lp < (int)s.lap[i - 1] + min_gap) lp = (int)s.lap[i - 1] + min_gap;
        s.lap[i] = (u8)std::clamp(lp, min_lap, max_lap);
    }
    // Pull back earlier stops if clamping compressed the end of the race
    for (int i = n - 2; i >= 0; i--) {
        if ((int)s.lap[i + 1] - (int)s.lap[i] < min_gap)
            s.lap[i] = (u8)std::max(min_lap, (int)s.lap[i + 1] - min_gap);
    }
    // Guarantee teammates never pit on the exact same lap
    if (avoid && avoid->stops > 0) {
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < avoid->stops; j++) {
                if (s.lap[i] == avoid->lap[j]) {
                    int cand = (int)s.lap[i] + (delta >= 0 ? 1 : -1);
                    if (cand >= min_lap && cand <= max_lap &&
                        (i == 0 || cand >= (int)s.lap[i - 1] + min_gap) &&
                        (i + 1 == n || cand <= (int)s.lap[i + 1] - min_gap)) {
                        s.lap[i] = (u8)cand;
                    } else {
                        cand = (int)s.lap[i] + (delta >= 0 ? -1 : 1);
                        if (cand >= min_lap && cand <= max_lap) s.lap[i] = (u8)cand;
                    }
                }
            }
        }
    }
    return s;
}

std::unordered_set<u32> collect_human_entities() {
    std::unordered_set<u32> out;
    if (!mem::readable(0x100DE2A0, 4)) return out;
    const u32 mgr = ld32(0x100DE2A0);
    if (!mgr || !mem::readable(mgr + 0x44, 4)) return out;
    const u32 head = ld32(mgr + 0x44);
    if (!head || !mem::readable(head, 12)) return out;
    for (u32 n = ld32(head), k = 0; n && n != head && k < 64 && mem::readable(n, 12); n = ld32(n), k++) {
        const u32 drv = ld32(n + 8);
        if (drv && mem::readable(drv + 0x220, 4)) {
            const u32 ent = ld32(drv + 0x220);
            if (ent) out.insert(ent);
        }
    }
    return out;
}

bool is_human_car(u32 track_logic, u32 car, const std::unordered_set<u32>& humans) {
    if (!car) return false;
    if (humans.count(car)) return true;
    // 0xCB280's own human-car flag: bit 1 of car + 0x64
    if (mem::readable(car + 0x64, 4) && (ld32(car + 0x64) & 2u) != 0) return true;
    // TrackLogic + 0x1A5C: per-car controller table indexed by car + 0x6C (0 = human, 1 = AI; hle_handling.cpp)
    if (mem::readable(car + 0x6C, 4)) {
        const u32 slot = ld32(car + 0x6C);
        if (slot < 22 && mem::readable(track_logic + 0x1A5C + 4 * slot, 4) &&
            ld32(track_logic + 0x1A5C + 4 * slot) == 0)
            return true;
    }
    return false;
}

void apply_car_strategy(PPUContext& c, u32 car_entity, const Strat4& s) {
    static u32 s_buf = 0;
    if (!s_buf) s_buf = mem::alloc(16, 16);
    st8(s_buf + 0, s.lap[0]);
    st8(s_buf + 1, s.lap[1]);
    st8(s_buf + 2, s.lap[2]);
    st8(s_buf + 3, s.stops);

    const u64 r3 = c.r[3], r4 = c.r[4], lr = c.lr;
    const u32 fn = c.cur_fn;
    c.r[3] = car_entity;
    c.r[4] = s_buf;
    sub_0011E7B0(c);
    c.r[3] = r3;
    c.r[4] = r4;
    c.lr = lr;
    c.cur_fn = fn;
}

void diversify_pit_strategies(PPUContext& c, u32 track_logic) {
    if (!mem::readable(track_logic + 0x7C8, 0x2C) || !mem::readable(track_logic + 0x1AC8, 11 * 12)) return;
    if (!mem::readable(0x101108A4, 4)) return;
    const u32 settings = ld32(0x101108A4);
    if (!settings || !mem::readable(settings + 0xDC, 4)) return;

    const float race_dist = ldf(settings + 0xB0);
    if (!std::isfinite(race_dist) || race_dist < 0.45f) return;   // leave short sprints (< 50%) to the game

    const u32 track_info = ld32(track_logic + 0x7F0);
    if (!track_info || !mem::readable(track_info + 0x44, 4)) return;
    const u32 base_laps = ld32(track_info + 0x40);
    const int total_laps = (int)std::lround((double)base_laps * (double)race_dist);
    if (total_laps < 18) return;

    const auto humans = collect_human_entities();
    const u32 teams_base = track_logic + 0x1AC8;

    // Check if pit stops are actually active (fuel/tyre wear off leaves numStops == 0 on AI cars)
    bool any_stops = false;
    for (int t = 0; t < 11; t++) {
        for (int s = 0; s < 2; s++) {
            const u32 car = ld32(teams_base + (u32)(t * 12 + s * 4));
            if (car && !is_human_car(track_logic, car, humans) && mem::readable(car + 0x9F, 1) && ld8(car + 0x9F) > 0) {
                any_stops = true;
                break;
            }
        }
        if (any_stops) break;
    }
    if (!any_stops) return;

    const Strat4 base1 = make_base_strategy(track_logic, 1, total_laps);
    const Strat4 base2 = make_base_strategy(track_logic, 2, total_laps);
    const Strat4 base3 = make_base_strategy(track_logic, 3, total_laps);
    auto base_for = [&](int stops) -> const Strat4& {
        return stops == 1 ? base1 : (stops == 3 ? base3 : base2);
    };

    std::random_device rd;
    std::mt19937 rng(rd());
    auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    // Realistic 2006 F1 stop-count mix by race distance (front teams 0..3 skew slightly more aggressive,
    // midfield/backmarker teams 4..10 more willing to run heavy 1-stoppers to gain track position).
    auto pick_stops = [&](int team) -> int {
        const int r = roll(0, 99);
        if (race_dist >= 0.99f) {
            if (team < 4) return r < 10 ? 1 : (r < 65 ? 2 : 3);   // top teams: 10% 1-stop, 55% 2-stop, 35% 3-stop
            return r < 30 ? 1 : (r < 85 ? 2 : 3);                 // midfield/back: 30% 1-stop, 55% 2-stop, 15% 3-stop
        }
        if (race_dist >= 0.70f) {
            if (team < 4) return r < 25 ? 1 : (r < 85 ? 2 : 3);   // 75%: mostly 2-stop, some 1-stop & rare 3-stop
            return r < 45 ? 1 : (r < 95 ? 2 : 3);
        }
        return r < 65 ? 1 : 2;                                    // 50%: mix of 1-stop and aggressive 2-stop
    };

    int counts[4] = {0, 0, 0, 0};
    for (int t = 0; t < 11; t++) {
        const u32 car1 = ld32(teams_base + (u32)(t * 12 + 0));
        const u32 car2 = ld32(teams_base + (u32)(t * 12 + 4));
        const bool hum1 = is_human_car(track_logic, car1, humans);
        const bool hum2 = is_human_car(track_logic, car2, humans);
        const bool ai1 = car1 && !hum1 && mem::readable(car1 + 0xA0, 16) && ld8(car1 + 0x9F) > 0;
        const bool ai2 = car2 && !hum2 && mem::readable(car2 + 0xA0, 16) && ld8(car2 + 0x9F) > 0;
        if (!ai1 && !ai2) continue;

        const int stops1 = pick_stops(t);
        // ~35% of teams split strategies between their two drivers; others run the same stop count staggered.
        int stops2 = stops1;
        if (roll(0, 99) < 35) {
            stops2 = pick_stops(t);
            if (stops2 == stops1) {
                if (race_dist >= 0.99f) stops2 = (stops1 == 2) ? (roll(0, 1) ? 1 : 3) : 2;
                else stops2 = (stops1 == 1) ? 2 : 1;
            }
        }

        const int max_jit = std::clamp(total_laps / 16, 2, 4);
        int jit1 = roll(1, max_jit);
        if (roll(0, 1)) jit1 = -jit1;
        int jit2 = -jit1;

        Strat4 s1{}, s2{};
        // If one car is human, avoid colliding with the human car's pit laps when assigning the AI teammate
        Strat4 human_strat{};
        const Strat4* avoid_for_1 = nullptr;
        const Strat4* avoid_for_2 = nullptr;
        if (hum2 && mem::readable(car2 + 0x9C, 4)) {
            human_strat = read_slot(car2 + 0x9C);
            avoid_for_1 = &human_strat;
        } else if (hum1 && mem::readable(car1 + 0x9C, 4)) {
            human_strat = read_slot(car1 + 0x9C);
            avoid_for_2 = &human_strat;
        }

        if (ai1) {
            s1 = jitter_strategy(base_for(stops1), jit1, total_laps, avoid_for_1);
            apply_car_strategy(c, car1, s1);
            counts[s1.stops]++;
            avoid_for_2 = &s1;
        }
        if (ai2) {
            s2 = jitter_strategy(base_for(stops2), jit2, total_laps, avoid_for_2);
            apply_car_strategy(c, car2, s2);
            counts[s2.stops]++;
        }
        auto fmt_s = [](const Strat4& s, bool active, char* buf, size_t n) {
            if (!active) { snprintf(buf, n, "human/off"); return; }
            if (s.stops == 1) snprintf(buf, n, "1-stop [L%u]", s.lap[0]);
            else if (s.stops == 2) snprintf(buf, n, "2-stop [L%u,L%u]", s.lap[0], s.lap[1]);
            else snprintf(buf, n, "3-stop [L%u,L%u,L%u]", s.lap[0], s.lap[1], s.lap[2]);
        };
        char b1[32], b2[32];
        fmt_s(s1, ai1, b1, sizeof(b1));
        fmt_s(s2, ai2, b2, sizeof(b2));
        LOG_I("ai strategies: team %2d: car1=%s | car2=%s", t, b1, b2);
    }

    LOG_I("ai strategies: %.0f%% race (%d laps) -> 1-stop: %d, 2-stop: %d, 3-stop: %d AI cars",
          race_dist * 100.0f, total_laps, counts[1], counts[2], counts[3]);
}

// ------------------------------------------------------------------ Tyre wear & degradation (F1CE_TYRE_WEAR)
//
// In the retail game, sub_00113F90 (UpdateTyreTempAndWear) reads four per-compound floats copied from dynamics.bin
// section 9 into car + 0x37C..0x388:
//   +0x37C  high-speed/load wear rate  (dry hard 0.070, dry soft 0.085, inter 0.350, wet 0.7875)
//   +0x380  base rolling wear rate     (dry 0.0175, inter 0.035, wet 0.045)
//   +0x384  wear cliff threshold       (dry 0.375 -> 0% grip drop for the first 37.5% of wear)
//   +0x388  max grip loss at 100% wear (dry hard 0.065, dry soft 0.075 -> barely felt even on bald tyres)
// Both human (car + 0x2E0 == 0) and AI (car + 0x2E0 != 0) accumulate per-wheel wear in car + 0x8B0 [0..1],
// but the retail code skips L_114DDC (wear grip penalty) for AI cars and writes final 4-wheel tyre grip to car + 0x4D0.
// When enabled, tyre_wear_hook temporarily scales +0x37C..+0x388 while 0x113F90 runs (restoring originals right after),
// and applies matching proportional grip degradation to AI cars' car + 0x4D0 so tyre management and undercuts are fair.

enum class TyreWearMode { original, mild, medium, realistic, high, custom };

struct TyreWearConfig {
    TyreWearMode mode = TyreWearMode::original;
    float dry_mult = 1.0f;
    float soft_extra = 1.0f;
    float wet_mult = 1.0f;
    float dry_threshold = 0.375f;
    float hard_loss = 0.065f;
    float soft_loss = 0.075f;
    float wet_loss = 0.095f;
    const char* label = "original";
};

TyreWearConfig parse_tyre_wear() {
    TyreWearConfig cfg;
    const char* env = getenv("F1CE_TYRE_WEAR");
    if (!env || !*env) return cfg;
    std::string s(env);
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    if (s == "0" || s == "off" || s == "original") return cfg;
    if (s == "mild" || s == "1.5") {
        cfg.mode = TyreWearMode::mild;
        cfg.dry_mult = 1.5f;
        cfg.soft_extra = 1.12f;
        cfg.wet_mult = 1.15f;
        cfg.dry_threshold = 0.22f;
        cfg.hard_loss = 0.15f;
        cfg.soft_loss = 0.18f;
        cfg.wet_loss = 0.15f;
        cfg.label = "mild (1.5x)";
        return cfg;
    }
    if (s == "medium" || s == "2.0") {
        cfg.mode = TyreWearMode::medium;
        cfg.dry_mult = 2.0f;
        cfg.soft_extra = 1.12f;
        cfg.wet_mult = 1.20f;
        cfg.dry_threshold = 0.20f;
        cfg.hard_loss = 0.16f;
        cfg.soft_loss = 0.19f;
        cfg.wet_loss = 0.16f;
        cfg.label = "medium (2.0x)";
        return cfg;
    }
    if (s == "1" || s == "realistic" || s == "on" || s == "2.5") {
        cfg.mode = TyreWearMode::realistic;
        cfg.dry_mult = 2.5f;
        cfg.soft_extra = 1.12f;
        cfg.wet_mult = 1.25f;
        cfg.dry_threshold = 0.20f;
        cfg.hard_loss = 0.16f;
        cfg.soft_loss = 0.19f;
        cfg.wet_loss = 0.16f;
        cfg.label = "realistic (2.5x)";
        return cfg;
    }
    if (s == "2" || s == "high" || s == "heavy" || s == "3.5") {
        cfg.mode = TyreWearMode::high;
        cfg.dry_mult = 3.5f;
        cfg.soft_extra = 1.15f;
        cfg.wet_mult = 1.40f;
        cfg.dry_threshold = 0.15f;
        cfg.hard_loss = 0.22f;
        cfg.soft_loss = 0.26f;
        cfg.wet_loss = 0.20f;
        cfg.label = "high (3.5x)";
        return cfg;
    }
    char* end = nullptr;
    float v = strtof(env, &end);
    if (end && end != env && std::isfinite(v) && v >= 0.5f && v <= 6.0f) {
        cfg.mode = TyreWearMode::custom;
        cfg.dry_mult = v;
        float t = std::clamp((v - 1.0f) / 2.5f, 0.0f, 1.0f);
        cfg.soft_extra = 1.0f + 0.15f * t;
        cfg.wet_mult = 1.0f + 0.35f * t;
        cfg.dry_threshold = 0.25f - 0.10f * t;
        cfg.hard_loss = 0.14f + 0.08f * t;
        cfg.soft_loss = 0.17f + 0.09f * t;
        cfg.wet_loss = 0.14f + 0.06f * t;
        cfg.label = "custom";
    }
    return cfg;
}

const TyreWearConfig& tyre_wear_config() {
    static const TyreWearConfig cfg = [] {
        TyreWearConfig c = parse_tyre_wear();
        if (c.mode != TyreWearMode::original) {
            LOG_I("tyre_wear: %s (dry x%.2f, soft extra x%.2f, wet x%.2f, cliff %.0f%%, max grip loss %.0f%%/%.0f%%)",
                  c.label, c.dry_mult, c.soft_extra, c.wet_mult,
                  c.dry_threshold * 100.0f, c.hard_loss * 100.0f, c.soft_loss * 100.0f);
        }
        return c;
    }();
    return cfg;
}

// Cached per-meter racing-line expected speed profile for the currently loaded track (used by hle_penalties.cpp
// to measure real time gained when cutting corners and time given back during the 5 s Give Back Time window).
std::vector<float> g_ref_speed;

void cache_track_ref_speeds(u32 track_data) {
    g_ref_speed.clear();
    if (!mem::readable(track_data, 0x100)) return;
    const u32 seg_ptr = ld32(track_data + 0xEC);
    const u32 seg_cnt = ld32(track_data + 0xF4);
    const u32 spline = ld32(track_data + 0x0C);
    if (!seg_ptr || seg_cnt < 5 || seg_cnt > 2000 || !spline || !mem::readable(spline, 0x20)) return;
    const u32 n_pts = ld32(spline + 0x0C);
    if (n_pts < 500 || n_pts > 20000 || !mem::readable(seg_ptr, seg_cnt * 28)) return;

    g_ref_speed.assign(n_pts, 85.0f);
    for (u32 i = 0; i < seg_cnt; i++) {
        const u32 s = seg_ptr + i * 28;
        const float sc = ldf(s);
        if (!std::isfinite(sc) || sc < 5.0f || sc > 110.0f) continue;
        const int s_idx = (s16)ld16(s + 0x18);
        const int e_idx = (s16)ld16(s + 0x1A);
        int slen = e_idx - s_idx;
        if (slen < 0) slen += (int)n_pts;
        if (slen <= 0) slen = 1;
        if (slen > (int)n_pts) slen = (int)n_pts;
        for (int k = 0; k < slen; k++) {
            const size_t idx = (size_t)((s_idx + k) % (int)n_pts);
            if (sc < g_ref_speed[idx]) g_ref_speed[idx] = sc;
        }
    }
    // Realistic F1 braking/acceleration smoothing along the cached racing-line profile (~38 m/s^2 brake, ~13 m/s^2 accel)
    for (int pass = 0; pass < 2; pass++) {
        for (int i = (int)n_pts - 1; i >= 0; i--) {
            const float nxt = g_ref_speed[(size_t)((i + 1) % (int)n_pts)];
            g_ref_speed[(size_t)i] = std::min(g_ref_speed[(size_t)i], std::sqrt(nxt * nxt + 76.0f));
        }
        for (u32 i = 0; i < n_pts; i++) {
            const float prv = g_ref_speed[(i + n_pts - 1) % n_pts];
            const float acc = std::max(2.5f, 15.0f - 0.0013f * prv * prv);
            g_ref_speed[i] = std::min(g_ref_speed[i], std::sqrt(prv * prv + 2.0f * acc));
        }
    }
}

}  // namespace

// Expected racing-line travel time (seconds) between normalized lap progresses p0 -> p1 (0..1, wrap-safe).
// Returns < 0 if no track profile is cached or the delta is invalid.
float hle_ai_expected_segment_time(float p0, float p1) {
    const size_t n = g_ref_speed.size();
    if (n < 500) return -1.0f;
    float dp = p1 - p0;
    if (dp < -0.5f) dp += 1.0f;
    if (dp <= 0.0f || dp > 0.35f) return -1.0f;
    const int i0 = ((int)std::floor(p0 * (float)n) % (int)n + (int)n) % (int)n;
    const int steps = std::max(1, (int)std::lround(dp * (float)n));
    double sec = 0.0;
    for (int k = 0; k < steps; k++) {
        const float v = std::clamp(g_ref_speed[(size_t)((i0 + k) % (int)n)], 12.0f, 95.0f);
        sec += 1.0 / (double)v;
    }
    return (float)sec;
}

// 0x1D5F50(trackData, stream): loads the .tbf section-6 AI segment speed tables into trackData + 0xEC.
bool track_ai_speeds_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside) return false;
    const PaceConfig cfg = pace_config();
    const u32 track_data = (u32)c.r[3];
    inside = true;
    sub_001D5F50(c);
    inside = false;
    if (cfg.mode != PaceMode::original) apply_track_pace(track_data, cfg);
    cache_track_ref_speeds(track_data);
    return true;
}

// 0x1C04DC(fakeTimesObj, trackInfo, diffIdx): loads FakeTimes/<track>_<easy|hard>.fkt used for simulated
// qualifying/practice times. Scale its 198 sector floats in sync with track_ai_speeds_hook.
bool ai_faketimes_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const PaceConfig cfg = pace_config();
    if (inside || cfg.mode == PaceMode::original) return false;
    const u32 fkt_obj = (u32)c.r[3];
    const u32 track_info = (u32)c.r[4];
    inside = true;
    sub_001C04DC(c);
    inside = false;
    scale_faketimes(c, fkt_obj, track_info, cfg);
    return true;
}

// 0x62761C(outTicksPtr, trialSession): computes the Career Trial target lap time (in 50.025 Hz ticks) by
// querying TrackLogic + 0x18C (FakeTimes) for the team's #1 driver. Because F1CE_AI_PACE scales FakeTimes to
// match faster AI cornering without changing player car physics, undo that scale here so Career Trial targets
// (and the 3-lap/session timers computed from it in 0x5E9940 / 0x61DB04) stay calibrated to the actual car,
// while leaving Career Practice, Qualifying and Race AI pace 100% enhanced.
bool career_trial_target_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const PaceConfig cfg = pace_config();
    if (inside || cfg.mode == PaceMode::original) return false;

    const u32 out_ptr = (u32)c.r[3];
    const u32 trial_session = (u32)c.r[4];

    inside = true;
    sub_0062761C(c);
    inside = false;

    float scale = g_last_fkt_scale;
    if (trial_session && mem::readable(trial_session + 52, 4)) {
        const u32 track_logic = ld32(trial_session + 52);
        if (track_logic) {
            const auto it = g_fkt_scales.find(track_logic + 0x18Cu);
            if (it != g_fkt_scales.end()) scale = it->second;
        }
    }

    if (scale > 0.50f && scale < 0.999f && out_ptr && mem::readable(out_ptr, 4)) {
        const s32 scaled_ticks = (s32)ld32(out_ptr);
        if (scaled_ticks > 500 && scaled_ticks < 30000) {
            const s32 orig_ticks = (s32)std::lround((double)scaled_ticks / (double)scale);
            st32(out_ptr, (u32)orig_ticks);
            const double orig_s = (double)orig_ticks / 50.02500915527344;
            const double scaled_s = (double)scaled_ticks / 50.02500915527344;
            LOG_I("career trial: restored retail target %d:%06.3f (was %d:%06.3f under AI pace %.3fx)",
                  (int)(orig_s / 60.0), std::fmod(orig_s, 60.0),
                  (int)(scaled_s / 60.0), std::fmod(scaled_s, 60.0), scale);
        }
    }
    return true;
}

// 0xCB0E4(trackLogic): assigns initial pit stop strategies & stint fuel to every AI car before a race.
bool ai_pit_strategies_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || !strategies_enabled()) return false;
    const u32 track_logic = (u32)c.r[3];
    inside = true;
    sub_000CB0E4(c);
    inside = false;
    diversify_pit_strategies(c, track_logic);
    return true;
}

// 0xE22B0(segPair, aiSpeedsOut): computes AI target speeds (r4 + 68..88 = current/next center, right, left) with
// difficulty + wet-weather scaling. Retail computes wet_mult = 1.0 - 0.17 * wetness (0xE3980 -> 0x100DB3B4), skips
// wet scaling on corners >= 80 m/s, clamps slow corners at >= 20 m/s (72 km/h), and skips AI puddle/aquaplaning grip
// penalties (0x1148A0), leaving AI disproportionately fast in light and heavy rain. When F1CE_AI_WET_DROP is set,
// scale wet corner speeds to a realistic 1.0 - drop * wetness curve (default realistic = 0.20, balanced = 0.22,
// cautious = 0.25), lower the wet hairpin floor to 14 m/s (50 km/h), and smoothly cover 80..84 m/s fast corners while
// leaving flat-out straights (>= 84 m/s) untouched.
float ai_wet_drop() {
    static const float drop = [] {
        const char* e = getenv("F1CE_AI_WET_DROP");
        if (!e || !*e) return 0.0f;
        std::string s(e);
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "0" || s == "off" || s == "original") return 0.0f;
        if (s == "realistic") return 0.20f;
        if (s == "balanced") return 0.22f;
        if (s == "cautious") return 0.25f;
        if (s == "heavy") return 0.28f;
        char* end = nullptr;
        float v = strtof(e, &end);
        if (end && end != e && std::isfinite(v)) return std::clamp(v, 0.0f, 0.40f);
        return 0.0f;
    }();
    return drop;
}

bool ai_wet_speed_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const float drop = ai_wet_drop();
    if (inside || drop <= 0.171f) return false;

    if (!mem::readable(0x100DB3B8, 4)) return false;
    const u32 track_logic = ld32(0x100DB3B8);
    if (!track_logic || !mem::readable(track_logic + 0x80C, 4)) return false;
    const u32 weather = ld32(track_logic + 0x80C);
    if (!weather || !mem::readable(weather + 0x24, 4)) return false;

    const float wetness = std::clamp(ldf(weather + 0x24), 0.0f, 1.0f);
    if (wetness <= 0.01f) return false;

    const u32 out = (u32)c.r[4];
    inside = true;
    sub_000E22B0(c);
    inside = false;

    const float retail_mult = std::clamp(1.0f - 0.17f * wetness, 0.50f, 1.0f);
    const float target_mult = std::clamp(1.0f - drop * wetness, 0.50f, 1.0f);
    const float extra = target_mult / retail_mult;
    const float retail_hi_cut = 80.0f * retail_mult + 0.25f;
    const float wet_floor = 20.0f - 6.0f * wetness;   // 20 m/s dry -> 14 m/s (50 km/h) in full heavy rain

    auto adjust_one = [&](float v) -> float {
        if (!std::isfinite(v) || v <= 5.0f || v >= 84.0f) return v;
        if (v < retail_hi_cut) return std::max(wet_floor, v * extra);
        if (v >= 79.5f) {
            // Fast corners (80..84 m/s) that retail's 80 m/s cutoff skipped completely
            const float t = std::clamp((v - 79.5f) / 4.5f, 0.0f, 1.0f);
            const float mult = target_mult + (1.0f - target_mult) * t;
            return std::max(wet_floor, v * mult);
        }
        return v;
    };

    float sp[6];
    for (int i = 0; i < 6; i++) sp[i] = adjust_one(ldf(out + 68 + 4 * i));
    // Keep right/left passing-line speeds <= center racing-line speed
    if (sp[1] > sp[0]) sp[1] = sp[0];
    if (sp[2] > sp[0]) sp[2] = sp[0];
    if (sp[4] > sp[3]) sp[4] = sp[3];
    if (sp[5] > sp[3]) sp[5] = sp[3];
    for (int i = 0; i < 6; i++) stf(out + 68 + 4 * i, sp[i]);

    using clock = std::chrono::steady_clock;
    static auto s_last_log = clock::now() - std::chrono::seconds(60);
    const auto now = clock::now();
    if (now - s_last_log >= std::chrono::seconds(30)) {
        s_last_log = now;
        LOG_I("ai pace: wet scaling active (wetness=%.0f%%, retail=%.3fx -> target=%.3fx, extra=%.3fx)",
              wetness * 100.0f, retail_mult, target_mult, extra);
    }
    return true;
}

// Retail dynamics.bin section 9 gives Dry Soft only +2.28% more peak grip than Hard (1.79 vs 1.75 -> ~0.35-0.45 s/lap
// fresh), making Soft strictly worse than Hard once realistic tyre wear is active. F1CE_SOFT_GRIP (default off; 1: 1.042x,
// lifting effective peak grip to ~1.865 vs 1.75 = +6.6% -> ~0.9-1.2 s/lap faster when new) makes the Soft vs Hard
// speed/wear trade-off genuinely worthwhile in qualifying, undercuts and multi-stop races.
float soft_grip_boost() {
    static const float boost = [] {
        const char* e = getenv("F1CE_SOFT_GRIP");
        if (!e || !*e) return 1.0f;   // (off by default: the original grip)
        std::string s(e);
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "0" || s == "off" || s == "original") return 1.0f;
        if (s == "1" || s == "on" || s == "realistic") {
            LOG_I("tyre_grip: realistic Dry Soft peak grip active (+4.2%% -> ~1.865 vs Hard 1.75)");
            return 1.042f;
        }
        char* end = nullptr;
        float v = strtof(e, &end);
        if (end && end != e && std::isfinite(v) && v >= 1.0f && v <= 1.12f) {
            LOG_I("tyre_grip: custom Dry Soft peak grip multiplier %.3fx", v);
            return v;
        }
        return 1.042f;
    }();
    return boost;
}

float current_track_wetness() {
    if (!mem::readable(0x100DB3B8, 4)) return 0.0f;
    const u32 track_logic = ld32(0x100DB3B8);
    if (!track_logic || !mem::readable(track_logic + 0x80C, 4)) return 0.0f;
    const u32 weather = ld32(track_logic + 0x80C);
    if (!weather || !mem::readable(weather + 0x24, 4)) return 0.0f;
    return std::clamp(ldf(weather + 0x24), 0.0f, 1.0f);
}

// 0x113F90(car): updates tyre temperature, per-wheel wear (car + 0x8B0), and final 4-wheel tyre grip (car + 0x4D0).
bool tyre_wear_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const TyreWearConfig& cfg = tyre_wear_config();
    const float s_boost = soft_grip_boost();
    if (inside || (cfg.mode == TyreWearMode::original && s_boost <= 1.0005f)) return false;

    const u32 car = (u32)c.r[3];
    // Bit 1 of car + 0x1148 is SET when tyre wear is active (0x114988 runs when (flags & 2) != 0)
    const bool wear_active = (cfg.mode != TyreWearMode::original) && ((ld32(car + 0x1148) & 0x2u) != 0);

    const u32 orig_hi_u = ld32(car + 0x37C);
    const u32 orig_base_u = ld32(car + 0x380);
    const u32 orig_thr_u = ld32(car + 0x384);
    const u32 orig_loss_u = ld32(car + 0x388);

    const float orig_hi = std::bit_cast<float>(orig_hi_u);
    const float orig_base = std::bit_cast<float>(orig_base_u);
    const float orig_thr = std::bit_cast<float>(orig_thr_u);
    const float orig_loss = std::bit_cast<float>(orig_loss_u);

    // Dry compounds have orig_hi < 0.20 (hard 0.070, soft 0.085); inter (0.35) and wet (0.7875) already wear fast.
    const bool is_dry = orig_hi > 0.001f && orig_hi < 0.20f;
    const bool is_soft = is_dry && orig_hi >= 0.080f;
    const bool apply_soft_boost = is_soft && s_boost > 1.0005f;

    if (!wear_active && !apply_soft_boost) return false;

    u32 mine_hi_u = orig_hi_u, mine_base_u = orig_base_u, mine_thr_u = orig_thr_u, mine_loss_u = orig_loss_u;
    float new_thr = orig_thr, new_loss = orig_loss;

    if (wear_active) {
        const float rate_mult = is_dry ? (cfg.dry_mult * (is_soft ? cfg.soft_extra : 1.0f)) : cfg.wet_mult;
        const float new_hi = orig_hi * rate_mult;
        const float new_base = orig_base * rate_mult;
        new_thr = is_dry ? std::min(orig_thr, cfg.dry_threshold) : std::min(orig_thr, 0.35f);
        new_loss = is_dry ? (is_soft ? cfg.soft_loss : cfg.hard_loss) : std::max(orig_loss, cfg.wet_loss);

        mine_hi_u = std::bit_cast<u32>(new_hi);
        mine_base_u = std::bit_cast<u32>(new_base);
        mine_thr_u = std::bit_cast<u32>(new_thr);
        mine_loss_u = std::bit_cast<u32>(new_loss);

        st32(car + 0x37C, mine_hi_u);
        st32(car + 0x380, mine_base_u);
        st32(car + 0x384, mine_thr_u);
        st32(car + 0x388, mine_loss_u);
    }

    inside = true;
    sub_00113F90(c);
    inside = false;

    if (wear_active) {
        if (ld32(car + 0x37C) == mine_hi_u) st32(car + 0x37C, orig_hi_u);
        if (ld32(car + 0x380) == mine_base_u) st32(car + 0x380, orig_base_u);
        if (ld32(car + 0x384) == mine_thr_u) st32(car + 0x384, orig_thr_u);
        if (ld32(car + 0x388) == mine_loss_u) st32(car + 0x388, orig_loss_u);
    }

    // Boost fresh Dry Soft peak grip on dry surfaces (fades smoothly to 1.0x as track gets wet)
    float eff_soft_boost = 1.0f;
    if (apply_soft_boost) {
        const float dry_factor = 1.0f - current_track_wetness();
        eff_soft_boost = 1.0f + (s_boost - 1.0f) * dry_factor;
        if (eff_soft_boost > 1.0005f) {
            for (int i = 0; i < 4; i++) {
                const float grip = ldf(car + 0x4D0 + 4 * i);
                if (grip > 0.0f && std::isfinite(grip)) {
                    stf(car + 0x4D0 + 4 * i, grip * eff_soft_boost);
                }
            }
        }
    }

    if (!wear_active) return true;

    // Apply proportional wear grip degradation to AI cars too (retail skips L_114DDC when car + 0x2E0 != 0)
    const bool is_ai = ld8(car + 0x2E0) != 0;
    float w[4], avg_w = 0.0f;
    for (int i = 0; i < 4; i++) {
        w[i] = std::clamp(ldf(car + 0x8B0 + 4 * i), 0.0f, 1.0f);
        avg_w += w[i];
    }
    avg_w *= 0.25f;

    static float s_ai_wear_sum = 0.0f;
    static int s_ai_wear_cnt = 0;

    if (is_ai) {
        s_ai_wear_sum += avg_w;
        s_ai_wear_cnt++;
        const float denom = std::max(0.05f, 1.0f - new_thr);
        const float ai_loss = new_loss * 0.75f;  // fair stint pace drop without making AI spin out
        for (int i = 0; i < 4; i++) {
            const float blended = 0.5f * (w[i] + avg_w);
            const float eff = std::clamp((blended - new_thr) / denom, 0.0f, 1.0f);
            stf(car + 0x8D0 + 4 * i, eff);
            const float grip = ldf(car + 0x4D0 + 4 * i);
            if (grip > 0.0f && eff > 0.0f) {
                stf(car + 0x4D0 + 4 * i, grip * (1.0f - eff * ai_loss));
            }
        }
    } else if (avg_w > 0.005f) {
        using clock = std::chrono::steady_clock;
        static auto s_last_log = clock::now() - std::chrono::seconds(20);
        auto now = clock::now();
        if (now - s_last_log >= std::chrono::seconds(15)) {
            s_last_log = now;
            const float denom = std::max(0.05f, 1.0f - new_thr);
            const float eff = std::clamp((avg_w - new_thr) / denom, 0.0f, 1.0f);
            const float grip_mult = (1.0f - eff * new_loss) * eff_soft_boost;
            const float ai_avg = s_ai_wear_cnt > 0 ? (s_ai_wear_sum / (float)s_ai_wear_cnt) : 0.0f;
            s_ai_wear_sum = 0.0f;
            s_ai_wear_cnt = 0;
            LOG_I("tyre_wear: player FL=%.0f%% FR=%.0f%% RL=%.0f%% RR=%.0f%% (%s, grip x%.3f) | ai_avg=%.0f%%",
                  w[0] * 100.0f, w[1] * 100.0f, w[2] * 100.0f, w[3] * 100.0f,
                  is_dry ? (is_soft ? "soft" : "hard") : "wet/inter", grip_mult, ai_avg * 100.0f);
        }
    }
    return true;
}
