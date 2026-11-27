// Realistic F1 penalty rules (F1CE_PENALTY_MODE) and mechanical failures (F1CE_RELIABILITY):
//
// 1) Penalty system (F1CE_PENALTY_MODE, launcher "Penalty system"):
//    In retail F1 CE, corner cutting (0x12DAE8 inside 0x12E6D4), collisions (0x12E6D4 / 0x12F644), ignoring blue flags
//    (0x12EE50 inside 0x130EAC), yellow-flag overtakes (0x12F220 inside 0x130EAC), and 0x120658 all impose an artificial
//    on-track rev-limiter slowdown (car + 0xEC bit 0x200000, car + 0xF0 bit 0x800, end tick at car + 0x11C, cumulative
//    counter at car + 0x12A).
//
//    When F1CE_PENALTY_MODE is enabled:
//      drivethrough (1): strips the artificial rev-limiter slowdown for the human car; keeps retail lap invalidation
//                        (car + 0xBA) intact in Practice/Qualifying/Time Trial; in Races:
//                        - Accidental / minor cuts (< 0.45 s gained, no position gained): 2 Steward Warnings, then a
//                          Drive-Through penalty on the 3rd offence (pass through pit lane within 3 laps without
//                          stopping at the pit box, or receive a native Black Flag disqualification via 0x12BBE0).
//                        - Blatant / exploitable shortcuts (>= 0.45 s real time gained measured against the track's
//                          racing-line speed profile, or overtaking off-track): opens a 5-second "Give Back Time"
//                          window. Lifting to return the gained time downgrades it to a Steward Warning; keeping the
//                          illegal advantage past 5 s issues an immediate Drive-Through penalty (anti-exploit).
//      strict       (2): 0 free warnings (immediate Drive-Through on every race offence).
//      warnings     (3): strips rev-limiter slowdown and shows Steward Warnings only (no Drive-Through/DQ).
//
// 2) Mechanical failures (F1CE_RELIABILITY, launcher "Mechanical failures"):
//    In retail F1 CE, sub_0010CBBC rolls per-car mechanical failure distances over a 300 km reference race using each
//    team's reliability probabilities (phys + 4252/4256/4260/4268/4272) and allowed bitmask (phys + 4428). Because 21 AI
//    cars roll every race vs 1 human car (and Reduced rules strip engine blowout bits 0x1/0x40000000), players almost
//    never experience mechanical DNFs themselves despite full native support (75% power loss warning, EngineSmoke.bpe,
//    gearbox lock, hydraulics, suspension, native DNF screen).
//    When enabled, mechanical_failures_hook (0x10CBBC) unlocks the full failure bitmask (0x7FFE3FFF) for the human car
//    in Races and scales failure probabilities cleanly while preserving 2006 team reliability hierarchy.
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kernel.h"
#include "overlay.h"

// (weak: f1ce_online is translated from ONLINE.BIN without these EBOOT-only functions or hooks)
__attribute__((weak)) void sub_0012E6D4(PPUContext& __restrict c);
__attribute__((weak)) void sub_00130EAC(PPUContext& __restrict c);
__attribute__((weak)) void sub_00120658(PPUContext& __restrict c);
__attribute__((weak)) void sub_0012BBE0(PPUContext& __restrict c);
__attribute__((weak)) void sub_00151BDC(PPUContext& __restrict c);
__attribute__((weak)) void sub_0010CBBC(PPUContext& __restrict c);

// From runtime/hle_ai.cpp: expected racing-line travel time (s) between normalized lap progresses p0 -> p1.
float hle_ai_expected_segment_time(float p0, float p1);

namespace {

float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }

enum class PenaltyMode { original, drivethrough, strict, warnings };
enum class ReliabilityMode { original, realistic, high };

PenaltyMode penalty_mode() {
    static const PenaltyMode mode = [] {
        const char* e = getenv("F1CE_PENALTY_MODE");
        if (!e || !*e) return PenaltyMode::original;
        std::string s(e);
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "0" || s == "off" || s == "original") return PenaltyMode::original;
        if (s == "1" || s == "drivethrough" || s == "dt") return PenaltyMode::drivethrough;
        if (s == "2" || s == "strict") return PenaltyMode::strict;
        if (s == "3" || s == "warnings" || s == "warn") return PenaltyMode::warnings;
        return PenaltyMode::original;
    }();
    return mode;
}

ReliabilityMode reliability_mode() {
    static const ReliabilityMode mode = [] {
        const char* e = getenv("F1CE_RELIABILITY");
        if (!e || !*e) return ReliabilityMode::original;
        std::string s(e);
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "0" || s == "off" || s == "original") return ReliabilityMode::original;
        if (s == "1" || s == "realistic" || s == "player") return ReliabilityMode::realistic;
        if (s == "2" || s == "high" || s == "dramatic") return ReliabilityMode::high;
        return ReliabilityMode::original;
    }();
    return mode;
}

bool is_human_car(u32 car) {
    if (!car || !mem::readable(car + 0x70, 4)) return false;
    if (ld32(car + 0x64) & 2u) return true;
    if (!mem::readable(0x100DB3B8, 4)) return false;
    const u32 tl = ld32(0x100DB3B8);
    if (!tl) return false;
    const u32 slot = ld32(car + 0x6C);
    if (slot < 22 && mem::readable(tl + 0x1A5C + 4 * slot, 4) && ld32(tl + 0x1A5C + 4 * slot) == 0)
        return true;
    return false;
}

u32 cur_session_tick() {
    return mem::readable(0x100DB35C, 4) ? ld32(0x100DB35C) : 0;
}

float session_tick_hz() {
    if (mem::readable(0x10041BA4, 4)) {
        float hz = std::bit_cast<float>(ld32(0x10041BA4));
        if (hz >= 25.0f && hz <= 120.0f) return hz;
    }
    return 59.94f;
}

u32 race_total_laps() {
    if (!mem::readable(0x100DB3B8, 4)) return 0;
    const u32 tl = ld32(0x100DB3B8);
    if (!tl || !mem::readable(tl + 0x1A24, 4)) return 0;
    return ld32(tl + 0x1A24);
}

u32 track_length_m() {
    if (!mem::readable(0x100DB3B8, 4)) return 5000;
    const u32 tl = ld32(0x100DB3B8);
    if (!tl || !mem::readable(tl + 0x7FC, 4)) return 5000;
    const u32 len = ld32(tl + 0x7FC);
    return (len >= 1500 && len <= 12000) ? len : 5000;
}

float car_lap_progress(u32 car) {
    if (!car || !mem::readable(car + 0x54, 4)) return 0.0f;
    const float p = ldf(car + 0x54);
    return (std::isfinite(p) && p >= 0.0f && p <= 1.0f) ? p : 0.0f;
}

int car_race_pos(u32 car) {
    if (!car || !mem::readable(car + 0x5E, 1)) return 0;
    return (int)(s8)ld8(car + 0x5E);
}

float expected_segment_time(float p0, float p1) {
    const float cached = hle_ai_expected_segment_time(p0, p1);
    if (cached > 0.0f) return cached;
    float dp = p1 - p0;
    if (dp < -0.5f) dp += 1.0f;
    if (dp <= 0.0f || dp > 0.35f) return 0.0f;
    return (dp * (float)track_length_m()) / 52.0f;
}

struct PenaltyState {
    u32 track_logic = 0;
    u32 human_car = 0;
    u32 last_tick = 0;
    u32 last_offence_tick = 0;
    u16 last_pen_cnt = 0;
    int warnings = 0;
    bool dt_active = false;
    bool in_pit_lane = false;
    bool dq_issued = false;
    int dt_deadline_lap = 0;
    int last_banner_laps_left = -999;

    // Off-track excursion tracking (to measure real time gained vs racing line)
    bool off_track = false;
    u32 off_entry_tick = 0;
    float off_entry_progress = 0.0f;
    int off_entry_pos = 0;
    bool off_severe_cut = false;
    // Remember the most recent completed excursion briefly (sub_0012DAE8 issues the penalty right on rejoin)
    u32 last_exc_end_tick = 0;
    float last_exc_gain_s = 0.0f;
    bool last_exc_pos_gained = false;
    bool last_exc_severe = false;
    int last_exc_entry_pos = 0;

    // 5-second "Give Back Time" anti-exploit window for deliberate shortcuts
    bool gb_active = false;
    float gb_remaining_s = 0.0f;
    float gb_initial_s = 0.0f;
    bool gb_must_yield_pos = false;
    int gb_entry_pos = 0;
    u32 gb_deadline_tick = 0;
    u32 gb_last_tick = 0;
    float gb_last_progress = 0.0f;
    u32 gb_last_ui_tick = 0;
};

PenaltyState g_ps;

void reset_state(u32 tl, u32 car, u32 tick) {
    g_ps = {};
    g_ps.track_logic = tl;
    g_ps.human_car = car;
    g_ps.last_tick = tick;
    g_ps.last_pen_cnt = (car && mem::readable(car + 0x12A, 2)) ? ld16(car + 0x12A) : 0;
    overlay::set_penalty_banner({});
}

void show_toast(overlay::PenaltySeverity sev, const std::string& title, const std::string& sub, u64 dur_ms) {
    overlay::PenaltyBanner b;
    b.active = true;
    b.severity = sev;
    b.title = title;
    b.subtitle = sub;
    b.expires_at_us = dur_ms ? (time_usec() + dur_ms * 1000ull) : 0;
    overlay::set_penalty_banner(b);
}

void update_dt_banner(int laps_left) {
    if (g_ps.in_pit_lane) {
        show_toast(overlay::PenaltySeverity::Serving,
                   "SERVING DRIVE-THROUGH",
                   "Drive straight through pit lane (no stop)", 0);
        return;
    }
    char sub[96];
    if (laps_left <= 0) snprintf(sub, sizeof(sub), "BOX THIS LAP OR BLACK FLAG!");
    else if (laps_left == 1) snprintf(sub, sizeof(sub), "Serve within 1 lap (pit lane pass-through)");
    else snprintf(sub, sizeof(sub), "Serve within %d laps (pit lane pass-through)", laps_left);
    show_toast(overlay::PenaltySeverity::DriveThrough, "DRIVE-THROUGH PENALTY", sub, 0);
}

void issue_drive_through(u32 car, u32 total_laps, const char* reason_sub) {
    const int cur_lap = (int)(s16)ld16(car + 0x5C);
    g_ps.warnings = 0;
    g_ps.gb_active = false;
    g_ps.dt_active = true;
    g_ps.in_pit_lane = false;
    g_ps.dt_deadline_lap = std::min((int)total_laps, cur_lap + 3);
    const int laps_left = std::max(0, g_ps.dt_deadline_lap - cur_lap);
    g_ps.last_banner_laps_left = laps_left;
    LOG_I("penalty: DRIVE-THROUGH issued on lap %d/%u (%s, serve by lap %d)",
          cur_lap, total_laps, reason_sub ? reason_sub : "3rd offence", g_ps.dt_deadline_lap);
    if (reason_sub && *reason_sub) {
        char sub[128];
        if (laps_left <= 0) snprintf(sub, sizeof(sub), "%s — BOX THIS LAP!", reason_sub);
        else snprintf(sub, sizeof(sub), "%s — serve within %d lap%s", reason_sub, laps_left, laps_left == 1 ? "" : "s");
        show_toast(overlay::PenaltySeverity::DriveThrough, "DRIVE-THROUGH PENALTY", sub, 0);
    } else {
        update_dt_banner(laps_left);
    }
}

void record_steward_warning(u32 car, u32 total_laps, bool from_giveback) {
    const int cur_lap = (int)(s16)ld16(car + 0x5C);
    const int warn_limit = (penalty_mode() == PenaltyMode::strict) ? 0 : 2;
    g_ps.warnings++;
    if (g_ps.warnings <= warn_limit) {
        char title[64], sub[96];
        if (g_ps.warnings < warn_limit) {
            snprintf(title, sizeof(title), "STEWARD WARNING  %d / %d", g_ps.warnings, warn_limit);
            snprintf(sub, sizeof(sub), "%s",
                     from_giveback ? "Time given back — next cut is final warning"
                                   : "Track limits / incident — next is final warning");
            LOG_I("penalty: warning %d/%d on lap %d%s", g_ps.warnings, warn_limit, cur_lap,
                  from_giveback ? " (time given back)" : "");
            show_toast(overlay::PenaltySeverity::Warning, title, sub, 5000);
        } else {
            snprintf(title, sizeof(title), "FINAL WARNING  %d / %d", g_ps.warnings, warn_limit);
            snprintf(sub, sizeof(sub), "%s",
                     from_giveback ? "Time given back — next offence: Drive-Through"
                                   : "Next offence: Drive-Through penalty");
            LOG_I("penalty: final warning %d/%d on lap %d%s", g_ps.warnings, warn_limit, cur_lap,
                  from_giveback ? " (time given back)" : "");
            show_toast(overlay::PenaltySeverity::FinalWarning, title, sub, 5500);
        }
        return;
    }
    issue_drive_through(car, total_laps, nullptr);
}

void strip_rev_limiter(u32 car) {
    const u32 ec = ld32(car + 0xEC);
    const u32 f0 = ld32(car + 0xF0);
    if (ec & 0x10200000u) st32(car + 0xEC, ec & ~0x10200000u);
    if (f0 & 0x00000800u) st32(car + 0xF0, f0 & ~0x00000800u);
    if (ld32(car + 0x11C) != 0) st32(car + 0x11C, 0);
}

void ensure_session_fresh(u32 car, u32 tick) {
    const u32 tl = mem::readable(0x100DB3B8, 4) ? ld32(0x100DB3B8) : 0;
    const u16 pen_cnt = mem::readable(car + 0x12A, 2) ? ld16(car + 0x12A) : 0;
    const bool tick_rewound = (g_ps.last_tick > 60 && tick + 30 < g_ps.last_tick);
    const bool cnt_reset = (pen_cnt < g_ps.last_pen_cnt);
    if (tl != g_ps.track_logic || car != g_ps.human_car || tick_rewound || cnt_reset) {
        reset_state(tl, car, tick);
    }
    g_ps.last_tick = tick;
}

void track_offtrack_excursion(u32 car, u32 tick, u32 ec_flags) {
    const bool now_off = (ec_flags & 0x02000000u) != 0;
    const bool now_sev = (ec_flags & 0x08000000u) != 0;
    if (now_off) {
        if (!g_ps.off_track) {
            g_ps.off_track = true;
            g_ps.off_entry_tick = tick;
            g_ps.off_entry_progress = car_lap_progress(car);
            g_ps.off_entry_pos = car_race_pos(car);
            g_ps.off_severe_cut = now_sev;
        } else if (now_sev) {
            g_ps.off_severe_cut = true;
        }
    } else if (g_ps.off_track) {
        // Just returned to track: compute real time gained vs racing line
        const float hz = session_tick_hz();
        const float actual_s = (tick > g_ps.off_entry_tick) ? ((float)(tick - g_ps.off_entry_tick) / hz) : 0.0f;
        const float exit_p = car_lap_progress(car);
        const float exp_s = expected_segment_time(g_ps.off_entry_progress, exit_p);
        const float gain_s = (exp_s > 0.0f && actual_s > 0.05f) ? std::clamp(exp_s - actual_s, -5.0f, 10.0f) : 0.0f;
        const int cur_pos = car_race_pos(car);
        const bool pos_gained = (g_ps.off_entry_pos > 0 && cur_pos > 0 && cur_pos < g_ps.off_entry_pos);

        g_ps.last_exc_end_tick = tick;
        g_ps.last_exc_gain_s = gain_s;
        g_ps.last_exc_pos_gained = pos_gained;
        g_ps.last_exc_severe = g_ps.off_severe_cut || now_sev;
        g_ps.last_exc_entry_pos = g_ps.off_entry_pos;
        g_ps.off_track = false;
        g_ps.off_severe_cut = false;
    }
}

void update_giveback_banner(u32 tick) {
    const float hz = session_tick_hz();
    const float left_s = (g_ps.gb_deadline_tick > tick) ? ((float)(g_ps.gb_deadline_tick - tick) / hz) : 0.0f;
    char sub[112];
    if (g_ps.gb_must_yield_pos && g_ps.gb_remaining_s <= 0.05f) {
        snprintf(sub, sizeof(sub), "Yield illegal overtake  (%.1fs left or Drive-Through)", left_s);
    } else {
        snprintf(sub, sizeof(sub), "Lift off: %.1fs to give back  (%.1fs left or Drive-Through)",
                 std::max(0.1f, g_ps.gb_remaining_s), left_s);
    }
    show_toast(overlay::PenaltySeverity::FinalWarning, "CORNER CUT — GIVE BACK TIME", sub, 0);
}

void update_giveback(u32 car, u32 tick) {
    if (!g_ps.gb_active || g_ps.dt_active || g_ps.dq_issued) return;

    const u32 c104 = ld32(car + 0x68);
    const u32 f0 = ld32(car + 0xF0);
    if ((c104 & 0x0C000000u) != 0 || (f0 & 0x8000u) != 0) {
        g_ps.gb_active = false;
        overlay::set_penalty_banner({});
        return;
    }

    const float hz = session_tick_hz();
    const float cur_p = car_lap_progress(car);
    if (tick > g_ps.gb_last_tick) {
        const float dt_s = (float)(tick - g_ps.gb_last_tick) / hz;
        if (dt_s > 0.0f && dt_s < 1.0f) {
            const float exp_dt = expected_segment_time(g_ps.gb_last_progress, cur_p);
            // Only count time genuinely given back while staying on track (lifting / slowing down below racing-line pace)
            if (!g_ps.off_track && exp_dt >= 0.0f && dt_s > exp_dt) {
                const float gave_back = dt_s - exp_dt;
                g_ps.gb_remaining_s = std::max(0.0f, g_ps.gb_remaining_s - gave_back);
            }
        }
        g_ps.gb_last_tick = tick;
        g_ps.gb_last_progress = cur_p;
    }

    const int cur_pos = car_race_pos(car);
    const bool pos_ok = !g_ps.gb_must_yield_pos || (cur_pos >= g_ps.gb_entry_pos);
    const u32 total_laps = race_total_laps();

    if (g_ps.gb_remaining_s <= 0.05f && pos_ok) {
        g_ps.gb_active = false;
        LOG_I("penalty: player gave back %.2fs advantage within 5s window -> downgraded to warning", g_ps.gb_initial_s);
        record_steward_warning(car, total_laps, true);
        return;
    }

    if (tick >= g_ps.gb_deadline_tick) {
        g_ps.gb_active = false;
        LOG_I("penalty: 5s Give Back Time EXPIRED (%.2fs still unreturned) -> immediate Drive-Through!",
              g_ps.gb_remaining_s);
        issue_drive_through(car, total_laps, "Shortcut advantage not returned");
        return;
    }

    // Refresh countdown banner ~5 times/second (avoids unnecessary overlay re-rasterisation every frame)
    if (tick >= g_ps.gb_last_ui_tick + (u32)(hz * 0.2f)) {
        g_ps.gb_last_ui_tick = tick;
        update_giveback_banner(tick);
    }
}

void on_offence(u32 car, u32 tick, bool retail_severe_cut) {
    const PenaltyMode mode = penalty_mode();
    const u32 cooldown = (u32)(session_tick_hz() * 4.0f);   // 4 s debounce so one corner cut counts once
    if (g_ps.last_offence_tick != 0 && tick >= g_ps.last_offence_tick && (tick - g_ps.last_offence_tick) < cooldown) {
        return;
    }
    g_ps.last_offence_tick = tick;

    const u32 total_laps = race_total_laps();
    const bool is_race = (total_laps >= 2);
    const int cur_lap = (int)(s16)ld16(car + 0x5C);

    if (!is_race) {
        LOG_I("penalty: track limits in non-race session (lap invalidated, no rev limiter)");
        show_toast(overlay::PenaltySeverity::Info, "TRACK LIMITS", "Lap time invalidated (no slowdown)", 4000);
        return;
    }

    if (g_ps.dq_issued) return;

    if (mode == PenaltyMode::warnings) {
        g_ps.warnings++;
        char t[64];
        snprintf(t, sizeof(t), "STEWARD WARNING  #%d", g_ps.warnings);
        LOG_I("penalty: warning #%d on lap %d (warnings-only mode)", g_ps.warnings, cur_lap);
        show_toast(overlay::PenaltySeverity::Warning, t, "Track limits / incident (no slowdown)", 4500);
        return;
    }

    if (g_ps.dt_active) {
        LOG_I("penalty: additional offence on lap %d while Drive-Through already pending", cur_lap);
        return;
    }

    if (mode == PenaltyMode::strict) {
        issue_drive_through(car, total_laps, nullptr);
        return;
    }

    // Measure whether this offence was a deliberate / exploitable shortcut (real time gain or illegal overtake)
    const float hz = session_tick_hz();
    float gain_s = 0.0f;
    bool pos_gained = false;
    bool severe = retail_severe_cut;
    int entry_pos = 0;

    if (g_ps.off_track) {
        const float actual_s = (tick > g_ps.off_entry_tick) ? ((float)(tick - g_ps.off_entry_tick) / hz) : 0.0f;
        const float exp_s = expected_segment_time(g_ps.off_entry_progress, car_lap_progress(car));
        if (exp_s > 0.0f && actual_s > 0.05f) gain_s = std::clamp(exp_s - actual_s, -5.0f, 10.0f);
        const int cur_pos = car_race_pos(car);
        pos_gained = (g_ps.off_entry_pos > 0 && cur_pos > 0 && cur_pos < g_ps.off_entry_pos);
        severe = severe || g_ps.off_severe_cut;
        entry_pos = g_ps.off_entry_pos;
    } else if (g_ps.last_exc_end_tick != 0 && tick >= g_ps.last_exc_end_tick &&
               (tick - g_ps.last_exc_end_tick) <= (u32)(hz * 2.5f)) {
        gain_s = g_ps.last_exc_gain_s;
        pos_gained = g_ps.last_exc_pos_gained;
        severe = severe || g_ps.last_exc_severe;
        entry_pos = g_ps.last_exc_entry_pos;
    }

    const bool blatant_shortcut = (gain_s >= 0.45f) || pos_gained || (severe && gain_s >= 0.25f);
    if (blatant_shortcut && !g_ps.gb_active) {
        const float target_s = std::clamp(std::max(gain_s, pos_gained ? 0.80f : 0.50f), 0.50f, 4.00f);
        g_ps.gb_active = true;
        g_ps.gb_initial_s = target_s;
        g_ps.gb_remaining_s = target_s;
        g_ps.gb_must_yield_pos = pos_gained;
        g_ps.gb_entry_pos = entry_pos;
        g_ps.gb_deadline_tick = tick + (u32)(hz * 5.0f);
        g_ps.gb_last_tick = tick;
        g_ps.gb_last_progress = car_lap_progress(car);
        g_ps.gb_last_ui_tick = tick;
        LOG_I("penalty: blatant shortcut on lap %d (gain %+.2fs, pos_gained=%d, severe=%d) -> 5s Give Back Time (%.2fs)",
              cur_lap, gain_s, pos_gained ? 1 : 0, severe ? 1 : 0, target_s);
        update_giveback_banner(tick);
        return;
    }

    if (g_ps.gb_active) {
        // Cut again while already owing time -> immediate Drive-Through!
        g_ps.gb_active = false;
        issue_drive_through(car, total_laps, "Repeated corner cutting");
        return;
    }

    record_steward_warning(car, total_laps, false);
}

void update_dt_progress(PPUContext& c, u32 car) {
    if (!g_ps.dt_active || g_ps.dq_issued) return;

    const u32 c104 = ld32(car + 0x68);
    const u32 f0 = ld32(car + 0xF0);
    // Race finished / retired / already disqualified
    if ((c104 & 0x0C000000u) != 0 || (f0 & 0x8000u) != 0) {
        g_ps.dt_active = false;
        g_ps.in_pit_lane = false;
        overlay::set_penalty_banner({});
        return;
    }

    const u32 pit_state = (c104 >> 28) & 0xFu;   // 0=track, 1=pit entry, 2=pit lane, 3/4=box, 5=pit exit
    const int cur_lap = (int)(s16)ld16(car + 0x5C);

    if (pit_state == 2) {
        if (!g_ps.in_pit_lane) {
            g_ps.in_pit_lane = true;
            LOG_I("penalty: player entered pit lane to serve Drive-Through (lap %d)", cur_lap);
            update_dt_banner(0);
        }
        return;
    }

    if (g_ps.in_pit_lane && (pit_state == 5 || pit_state == 0)) {
        g_ps.dt_active = false;
        g_ps.in_pit_lane = false;
        g_ps.last_banner_laps_left = -999;
        LOG_I("penalty: Drive-Through SERVED on lap %d!", cur_lap);
        show_toast(overlay::PenaltySeverity::Served, "DRIVE-THROUGH SERVED", "Penalty cleared", 5000);
        return;
    }

    // Still on track (not in pit entry/lane)
    if (pit_state == 0) {
        if (cur_lap > g_ps.dt_deadline_lap) {
            g_ps.dt_active = false;
            g_ps.dq_issued = true;
            LOG_I("penalty: BLACK FLAG — player failed to serve Drive-Through by lap %d (now lap %d)",
                  g_ps.dt_deadline_lap, cur_lap);
            show_toast(overlay::PenaltySeverity::BlackFlag, "BLACK FLAG — DISQUALIFIED",
                       "Failed to serve Drive-Through penalty", 6500);
            if (sub_0012BBE0) {
                PPUContext dq = c;
                dq.r[3] = car;
                dq.r[4] = 0x11;   // pit / rules violation DQ reason
                sub_0012BBE0(dq);
            }
            return;
        }
        const int laps_left = std::max(0, g_ps.dt_deadline_lap - cur_lap);
        if (laps_left != g_ps.last_banner_laps_left) {
            g_ps.last_banner_laps_left = laps_left;
            update_dt_banner(laps_left);
        }
    }
}

void wrap_penalty_fn(PPUContext& c, void (*orig)(PPUContext& __restrict), bool update_tick) {
    const u32 car = (u32)c.r[3];
    if (!is_human_car(car)) {
        orig(c);
        return;
    }
    const u32 tick = cur_session_tick();
    ensure_session_fresh(car, tick);

    const u16 before_cnt = ld16(car + 0x12A);
    const u32 before_ec = ld32(car + 0xEC);

    orig(c);

    const u16 after_cnt = ld16(car + 0x12A);
    const u32 after_ec = ld32(car + 0xEC);
    const bool new_pen = (after_cnt > before_cnt) || (!(before_ec & 0x200000u) && (after_ec & 0x200000u));
    const bool severe_cut = ((after_ec & 0x08000000u) != 0) || ((before_ec & 0x08000000u) != 0);
    g_ps.last_pen_cnt = after_cnt;

    if (update_tick) track_offtrack_excursion(car, tick, after_ec);
    strip_rev_limiter(car);
    if (new_pen) on_offence(car, tick, severe_cut);
    if (update_tick) {
        update_giveback(car, tick);
        update_dt_progress(c, car);
    }
}

}  // namespace

bool penalty_tick_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || penalty_mode() == PenaltyMode::original) return false;
    inside = true;
    wrap_penalty_fn(c, sub_0012E6D4, true);
    inside = false;
    return true;
}

bool penalty_flags_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || penalty_mode() == PenaltyMode::original) return false;
    inside = true;
    wrap_penalty_fn(c, sub_00130EAC, false);
    inside = false;
    return true;
}

bool penalty_issue_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || penalty_mode() == PenaltyMode::original) return false;
    inside = true;
    wrap_penalty_fn(c, sub_00120658, false);
    inside = false;
    return true;
}

// 0x151BDC(pitObj): pit-lane state handler while approaching the pit box (car = ld32(r3 + 8)).
// When the human car is serving an active Drive-Through penalty in the pit lane, skip stopping at the pit box
// (no tyre change / no refuel) and transition straight into pit-exit release (L_151C7C state).
bool penalty_pitbox_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || penalty_mode() == PenaltyMode::original || !g_ps.dt_active) return false;

    const u32 pit_obj = (u32)c.r[3];
    if (!pit_obj || !mem::readable(pit_obj + 8, 4)) return false;
    const u32 car = ld32(pit_obj + 8);
    if (car != g_ps.human_car || !is_human_car(car)) return false;

    const u32 pit_state = (ld32(car + 0x68) >> 28) & 0xFu;
    if (pit_state != 2) return false;

    g_ps.in_pit_lane = true;
    // Apply exact L_151C7C pass-through state (release straight to pit exit without calling 0x121870 service)
    u32 c140 = ld32(car + 140);
    c140 = (c140 & ~0x7Cu) | 0x0Cu | 0x02u;
    st32(car + 140, c140);
    u32 c144 = ld32(car + 144);
    c144 = (c144 & 0x3FFFFFFFu) | 0x40000000u;
    c144 = (c144 & ~0x38000000u) | (2u << 27);
    st32(car + 144, c144);
    st8(car + 247, 3);
    return true;
}

// 0x10CBBC(carPhys, allowedMask, raceFrac, startDist): rolls random mechanical failure distances for a car at race
// start (or after pit repair). Enables full native mechanical failures (gradual engine 75% -> blowout + EngineSmoke,
// sudden engine, gearbox, hydraulics, suspension) for the human car and scales failure rates realistically.
bool mechanical_failures_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const ReliabilityMode mode = reliability_mode();
    if (inside || mode == ReliabilityMode::original) return false;

    const u32 phys = (u32)c.r[3];
    if (!phys || !mem::readable(phys + 4444, 4) || race_total_laps() < 2) return false;
    const u32 car = ld32(phys + 4444);
    const bool human = is_human_car(car);
    if (!human && mode == ReliabilityMode::realistic) return false;

    // Rate offsets in carPhys: +4252 sudden engine, +4256 gearbox, +4260 gradual engine (75% -> smoke),
    // +4268 suspension, +4272 hydraulics
    constexpr u32 kRateOffs[5] = {4252, 4256, 4260, 4268, 4272};
    float saved[5] = {};
    for (int i = 0; i < 5; i++) saved[i] = ldf(phys + kRateOffs[i]);

    const float mult = human ? (mode == ReliabilityMode::high ? 6.0f : 3.0f) : 1.5f;
    // Floor only applies to the human car so even top-tier teams have a real (still modest) race-day failure risk
    const float min_floor[5] = {
        human ? (mode == ReliabilityMode::high ? 0.030f : 0.012f) : 0.0f,   // sudden engine
        human ? (mode == ReliabilityMode::high ? 0.045f : 0.020f) : 0.0f,   // gearbox
        human ? (mode == ReliabilityMode::high ? 0.065f : 0.030f) : 0.0f,   // gradual engine (75% -> blowout + smoke)
        human ? (mode == ReliabilityMode::high ? 0.025f : 0.010f) : 0.0f,   // suspension
        human ? (mode == ReliabilityMode::high ? 0.030f : 0.012f) : 0.0f,   // hydraulics
    };

    for (int i = 0; i < 5; i++) {
        float r = (std::isfinite(saved[i]) && saved[i] > 0.0f) ? saved[i] : 0.0f;
        r = std::clamp(std::max(r * mult, min_floor[i]), 0.0f, 0.35f);
        stf(phys + kRateOffs[i], r);
    }

    if (human) {
        // Unlock full mechanical failure mask (engine gradual/sudden, smoke, gearbox, hydraulics, suspension, punctures, brakes)
        constexpr u32 kFullMask = 0x7FFE3FFFu;
        const u32 cur_mask = ld32(phys + 4428);
        if ((cur_mask & kFullMask) != kFullMask) st32(phys + 4428, cur_mask | kFullMask);
        c.r[4] = (u32)c.r[4] | kFullMask;
    }

    inside = true;
    sub_0010CBBC(c);
    inside = false;

    for (int i = 0; i < 5; i++) stf(phys + kRateOffs[i], saved[i]);

    if (human) {
        const float race_m = (float)c.f[1].d * 300000.0f;
        const float d_gbx = ldf(phys + 4052);
        const float d_eng = ldf(phys + 4056);
        const float d_sud = ldf(phys + 4060);
        const float d_sus = ldf(phys + 4068);
        LOG_I("reliability: human car rolled (mode=%s, race~%.0fm) eng=%.0fm sudden=%.0fm gbx=%.0fm susp=%.0fm",
              mode == ReliabilityMode::high ? "high" : "realistic", race_m, d_eng, d_sud, d_gbx, d_sus);
    }
    return true;
}
