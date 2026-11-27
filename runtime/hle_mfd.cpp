// On-track Pit-Stop Tyre Selector MFD (F1CE_ONTRACK_MFD):
//   - Dedicated 4-way gamepad D-Pad selector on track (menus, pause screen, garage and replays stay 100% normal):
//       D-Pad Up    -> SOFT  (tap again -> AUTO)
//       D-Pad Down  -> HARD  (tap again -> AUTO)
//       D-Pad Left  -> INTER (tap again -> AUTO)
//       D-Pad Right -> WET   (tap again -> AUTO)
//   - Keyboard shortcuts:
//       F5 / Shift+F5 -> cycle next pit-stop tyre (AUTO / HARD / SOFT / INTER / WET)
//       F6            -> live Traction Control cycle (keyboard-only)
//       F7            -> live ABS toggle (keyboard-only)
//   - Hooks sub_0010D3E8 for both automatic (0x121870, lr=0x121A34) and interactive (0x5F833C, lr=0x5F84DC) pit stops.
//   - Uses controller_update_hook (0x3574C0) r4 (driving) so it ONLY activates when the player actually has live
//     control of the car on track (plus pit entry / pit lane after driving). Strips gamepad D-Pad bits/pressures while
//     driving so D-Pad Left/Right never steers the car; keyboard arrow keys are never touched.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "host.h"
#include "kernel.h"
#include "overlay.h"

// (weak: f1ce_online is translated from ONLINE.BIN without this EBOOT-only function)
__attribute__((weak)) void sub_0010D3E8(PPUContext& __restrict c);

namespace {

enum class MfdMode { off, keyboard_only, full };

MfdMode mfd_mode() {
    static const MfdMode mode = [] {
        if (g_online_image) return MfdMode::off;
        const char* e = getenv("F1CE_ONTRACK_MFD");
        if (!e || !*e) return MfdMode::full;   // enabled by default (gamepad D-Pad tyres + F5/F6/F7)
        std::string s(e);
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "0" || s == "off" || s == "false") return MfdMode::off;
        if (s == "kb" || s == "keyboard" || s == "keys") return MfdMode::keyboard_only;
        return MfdMode::full;
    }();
    return mode;
}

enum class NextTyre : int { Auto = 0, Hard = 1, Soft = 2, Inter = 3, Wet = 4, Count = 5 };

const char* tyre_label(NextTyre t) {
    switch (t) {
        case NextTyre::Auto:  return "AUTO";
        case NextTyre::Hard:  return "HARD";
        case NextTyre::Soft:  return "SOFT";
        case NextTyre::Inter: return "INTER";
        case NextTyre::Wet:   return "WET";
        default:              return "AUTO";
    }
}

const char* tyre_dir_hint(NextTyre t) {
    switch (t) {
        case NextTyre::Soft:  return "UP";
        case NextTyre::Hard:  return "DN";
        case NextTyre::Inter: return "LT";
        case NextTyre::Wet:   return "RT";
        default:              return "";
    }
}

float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
void stf(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }

constexpr u32 kTrackLogicPtr = 0x100DB3B8;
constexpr u32 kSessionTickAddr = 0x100DB35C;
constexpr u32 kPlayerMgrPtr = 0x100DE2A0;   // TOC - 17892

// Walk the game's player manager list (same logic as sub_00140578(mgr, 0)) to get player_ctrl.
u32 find_player_ctrl() {
    if (!mem::readable(kPlayerMgrPtr, 4)) return 0;
    const u32 mgr = ld32(kPlayerMgrPtr);
    if (!mgr || (mgr & 3) || !mem::readable(mgr + 68, 4)) return 0;
    const u32 head = ld32(mgr + 68);
    if (!head || (head & 3) || !mem::readable(head, 4)) return 0;
    const u32 first = ld32(head);
    if (!first || first == head || (first & 3) || !mem::readable(first + 8, 4)) return 0;
    const u32 ctrl = ld32(first + 8);
    if (!ctrl || (ctrl & 3) || !mem::readable(ctrl + 548, 4)) return 0;
    return ctrl;
}

bool is_replaying() {
    if (!mem::readable(kPlayerMgrPtr, 4)) return false;
    const u32 sess = ld32(kPlayerMgrPtr);
    if (!sess || (sess & 3) || !mem::readable(sess + 0x2C, 4)) return false;
    return ld32(sess + 0x28) != 0;
}

bool is_human_car(u32 car) {
    if (!car || (car & 3) || !mem::readable(car + 0x70, 4)) return false;
    if (ld32(car + 0x64) & 2u) return true;
    if (!mem::readable(kTrackLogicPtr, 4)) return false;
    const u32 tl = ld32(kTrackLogicPtr);
    if (!tl || !mem::readable(tl + 0x1A5C, 88)) return false;
    const u32 slot = ld32(car + 0x6C);
    return slot < 22 && ld32(tl + 0x1A5C + 4 * slot) == 0;
}

struct TcStep {
    float val;
    int slot;
    const char* name;
};

std::vector<TcStep> build_tc_ladder(u32 car) {
    float slots[3] = {0.0f, 0.0f, 0.0f};
    bool ok = car && mem::readable(car + 0x48, 12);
    if (ok) {
        for (int i = 0; i < 3; i++) {
            slots[i] = ldf(car + 0x48 + (u32)(i * 4));
            if (!std::isfinite(slots[i]) || slots[i] < 0.0f || slots[i] > 1.0f) ok = false;
        }
    }
    if (!ok || (slots[0] == 0.0f && slots[1] == 0.0f && slots[2] == 0.0f)) {
        return {{0.0f, 0, "OFF"}, {0.45f, 0, "LOW"}, {0.72f, 1, "MED"}, {0.90f, 2, "HIGH"}};
    }

    struct Raw { float v; int s; };
    std::vector<Raw> raw = {{slots[0], 0}, {slots[1], 1}, {slots[2], 2}};
    std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.v < b.v; });

    std::vector<Raw> uniq;
    for (const auto& r : raw) {
        if (uniq.empty() || std::fabs(r.v - uniq.back().v) > 0.015f) uniq.push_back(r);
    }

    std::vector<TcStep> out;
    if (uniq.empty() || uniq.front().v > 0.02f) {
        out.push_back({0.0f, uniq.empty() ? 0 : uniq.front().s, "OFF"});
    }
    for (size_t i = 0; i < uniq.size(); i++) {
        const float v = uniq[i].v;
        const char* lbl = "HIGH";
        if (v <= 0.02f) lbl = "OFF";
        else if (uniq.size() == 1) lbl = "ON";
        else if (i == 0) lbl = "LOW";
        else if (i + 1 == uniq.size()) lbl = "HIGH";
        else lbl = "MED";
        out.push_back({v, uniq[i].s, lbl});
    }
    return out;
}

struct MfdStateLocal {
    u32 track_logic = 0;
    u32 human_car = 0;
    u32 last_tick = 0;
    u64 last_tick_us = 0;
    u64 pause_guard_until_us = 0;
    u64 dpad_debounce_until_us = 0;
    u8 prev_gp_dpad = 0;
    NextTyre next_tyre = NextTyre::Auto;
    overlay::MfdState ov;
};

MfdStateLocal g_mfd;

// Pending keyboard triggers queued from host_sdl.cpp UI thread
std::atomic<int> g_kb_tyre_delta{0};
std::atomic<int> g_kb_tc_step{0};
std::atomic<int> g_kb_abs_toggle{0};

// Updated per-frame by controller_update_hook (0x3574C0, hle_rumble.cpp): r4 (driving) is 1 ONLY while the human
// player actually has live control of the car on track (0 in menus, Grand Prix screen, garage, attract mode, replays,
// and stopped completely while paused).
std::atomic<u64> g_ctl_last_us{0};
std::atomic<bool> g_ctl_driving{false};
std::atomic<bool> g_driven_in_session{false};

void push_tyre_overlay(const std::string& note, u64 popup_ms) {
    g_mfd.ov.visible = true;
    g_mfd.ov.tyre_code = (int)g_mfd.next_tyre;
    g_mfd.ov.tyre_label = tyre_label(g_mfd.next_tyre);
    g_mfd.ov.note = note;
    g_mfd.ov.popup_until_us = popup_ms ? (time_usec() + popup_ms * 1000ull) : 0;
    overlay::set_mfd_state(g_mfd.ov);
}

void push_aid_toast(const char* tag, const std::string& val, bool green, u64 dur_ms) {
    g_mfd.ov.visible = true;
    g_mfd.ov.tyre_code = (int)g_mfd.next_tyre;
    g_mfd.ov.tyre_label = tyre_label(g_mfd.next_tyre);
    g_mfd.ov.aid_tag = tag;
    g_mfd.ov.aid_val = val;
    g_mfd.ov.aid_green = green;
    g_mfd.ov.aid_until_us = time_usec() + dur_ms * 1000ull;
    overlay::set_mfd_state(g_mfd.ov);
}

void reset_mfd_session(u32 tl, u32 car) {
    g_mfd.track_logic = tl;
    g_mfd.human_car = car;
    g_mfd.next_tyre = NextTyre::Auto;
    g_mfd.ov = {};
    g_driven_in_session.store(false);
    g_kb_tyre_delta.store(0);
    g_kb_tc_step.store(0);
    g_kb_abs_toggle.store(0);
    overlay::set_mfd_state({});
}

void set_next_tyre(NextTyre target, bool toggle_to_auto) {
    if (toggle_to_auto && g_mfd.next_tyre == target) {
        g_mfd.next_tyre = NextTyre::Auto;
    } else {
        g_mfd.next_tyre = target;
    }
    LOG_I("mfd: next pit tyre -> %s", tyre_label(g_mfd.next_tyre));
    char note[64];
    if (g_mfd.next_tyre == NextTyre::Auto) {
        snprintf(note, sizeof(note), "AUTO (weather selection)");
    } else {
        snprintf(note, sizeof(note), "Next pit: %s (%s again = AUTO)",
                 tyre_label(g_mfd.next_tyre), tyre_dir_hint(g_mfd.next_tyre));
    }
    push_tyre_overlay(note, 4000);
}

void apply_tyre_delta(int delta) {
    int v = ((int)g_mfd.next_tyre + delta) % (int)NextTyre::Count;
    if (v < 0) v += (int)NextTyre::Count;
    set_next_tyre((NextTyre)v, false);
}

void apply_tc_cycle(u32 car, u32 phys) {
    if (!car || !phys || !mem::readable(phys + 2400, 4)) return;
    const auto steps = build_tc_ladder(car);
    if (steps.empty()) return;

    const float cur = std::clamp(ldf(phys + 2400), 0.0f, 1.0f);
    int idx = 0;
    float best_d = 1e9f;
    for (int i = 0; i < (int)steps.size(); i++) {
        const float d = std::fabs(steps[i].val - cur);
        if (d < best_d) { best_d = d; idx = i; }
    }
    const TcStep& chosen = steps[(idx + 1) % (int)steps.size()];

    stf(phys + 2400, chosen.val);
    if (mem::readable(car + 0x181, 1)) st8(car + 0x181, (u8)std::clamp(chosen.slot, 0, 2));
    if (mem::readable(car + 444, 1)) st8(car + 444, 0);

    const int pct = (int)std::lround(chosen.val * 100.0f);
    char val[32];
    if (pct <= 1) snprintf(val, sizeof(val), "OFF");
    else snprintf(val, sizeof(val), "%s (%d%%)", chosen.name, pct);
    LOG_I("mfd: traction control -> %s", val);
    push_aid_toast("TC", val, pct > 1, 3000);
}

void apply_abs_toggle(u32 player_ctrl, u32 phys) {
    if (!phys || !mem::readable(phys + 1520, 4)) return;
    u32 flags = ld32(phys + 1520);
    const bool new_on = (flags & 0x10u) == 0;
    flags = new_on ? (flags | 0x10u) : (flags & ~0x10u);
    st32(phys + 1520, flags);

    if (player_ctrl && mem::readable(player_ctrl + 536, 4)) {
        const u32 aids = ld32(player_ctrl + 536);
        if (aids && !(aids & 3) && mem::readable(aids + 8, 2)) {
            u16 bits = ld16(aids + 8);
            bits = new_on ? (u16)(bits | 0x4u) : (u16)(bits & ~0x4u);
            st16(aids + 8, bits);
        }
    }

    LOG_I("mfd: ABS -> %s", new_on ? "ON" : "OFF");
    push_aid_toast("ABS", new_on ? "ON" : "OFF", new_on, 3000);
}

}  // namespace

// Called from hle_rumble.cpp inside controller_update_hook (0x3574C0)
void mfd_controller_tick(bool driving) {
    g_ctl_last_us.store(time_usec());
    g_ctl_driving.store(driving);
    if (driving) g_driven_in_session.store(true);
}

// Called from host_sdl.cpp UI thread on F5 / F6 / F7 key presses
void mfd_key_tyre(int delta) {
    if (mfd_mode() == MfdMode::off) return;
    g_kb_tyre_delta.fetch_add(delta);
}
void mfd_key_tc() {
    if (mfd_mode() == MfdMode::off) return;
    g_kb_tc_step.fetch_add(1);
}
void mfd_key_abs() {
    if (mfd_mode() == MfdMode::off) return;
    g_kb_abs_toggle.fetch_add(1);
}

// Called from hle_misc.cpp inside cellPadGetData on every pad poll
void mfd_pad_filter(host::PadState& s) {
    const MfdMode mode = mfd_mode();
    if (mode == MfdMode::off) return;

    const u64 now = time_usec();
    if (s.digital1 & 0x08u) g_mfd.pause_guard_until_us = now + 300000ull;

    const u32 tl = mem::readable(kTrackLogicPtr, 4) ? ld32(kTrackLogicPtr) : 0;
    const u32 tick = mem::readable(kSessionTickAddr, 4) ? ld32(kSessionTickAddr) : 0;
    const u32 player_ctrl = tl ? find_player_ctrl() : 0;
    const u32 car = player_ctrl ? ld32(player_ctrl + 544) : 0;
    const u32 phys = (car && !(car & 3) && mem::readable(car + 28, 4)) ? ld32(car + 28) : 0;

    if (!tl || !car || !phys || tl != g_mfd.track_logic || car != g_mfd.human_car) {
        if (g_mfd.track_logic || g_mfd.human_car || g_mfd.next_tyre != NextTyre::Auto)
            reset_mfd_session(tl, car);
        else {
            g_mfd.track_logic = tl;
            g_mfd.human_car = car;
        }
        g_mfd.last_tick = tick;
        g_mfd.last_tick_us = 0;
        g_mfd.prev_gp_dpad = s.gp_dpad;
        return;
    }

    if (tick != g_mfd.last_tick) {
        if (g_mfd.last_tick > 60 && tick + 30 < g_mfd.last_tick) reset_mfd_session(tl, car);
        g_mfd.last_tick = tick;
        if (tick > 0) g_mfd.last_tick_us = now;
    }

    const u32 c104 = mem::readable(car + 0x68, 4) ? ld32(car + 0x68) : 0;
    const u32 f0 = mem::readable(car + 0xF0, 4) ? ld32(car + 0xF0) : 0;
    const u32 pit_state = (c104 >> 28) & 0xFu;   // 0=track, 1=pit entry, 2=pit lane, 3/4=box, 5=pit exit
    const bool finished_or_dnf = ((c104 & 0x0C000000u) != 0) || ((f0 & 0x8000u) != 0);
    const bool ticking = (g_mfd.last_tick_us != 0) && (now - g_mfd.last_tick_us < 75000ull);
    const u64 ctl_us = g_ctl_last_us.load();
    const bool ctl_recent = (ctl_us != 0) && (now - ctl_us < 75000ull);
    const bool player_has_car = g_ctl_driving.load() ||
                                (g_driven_in_session.load() && (pit_state == 1 || pit_state == 2));
    const bool active_driving = ctl_recent && player_has_car && ticking && !finished_or_dnf && !is_replaying() &&
                                (pit_state != 3 && pit_state != 4) &&
                                (now >= g_mfd.pause_guard_until_us) &&
                                !overlay::input_captured();

    if (!active_driving) {
        g_mfd.prev_gp_dpad = s.gp_dpad;
        g_kb_tyre_delta.store(0);
        g_kb_tc_step.store(0);
        g_kb_abs_toggle.store(0);
        return;
    }

    // 1) Keyboard shortcuts (F5 tyres, F6 TC, F7 ABS)
    if (int d = g_kb_tyre_delta.exchange(0); d != 0) apply_tyre_delta(d);
    if (int n = g_kb_tc_step.exchange(0); n > 0) {
        for (int i = 0; i < n; i++) apply_tc_cycle(car, phys);
    }
    if (int n = g_kb_abs_toggle.exchange(0); n > 0) {
        if (n & 1) apply_abs_toggle(player_ctrl, phys);
    }

    // 2) Full 4-way gamepad D-Pad dedicated to Pit Tyres ONLY (UP=SOFT, DOWN=HARD, LEFT=INTER, RIGHT=WET;
    //    pressing the active direction again returns to AUTO). Ignores ambiguous diagonal presses and debounces.
    if (mode == MfdMode::full) {
        const u8 gp = (u8)(s.gp_dpad & 0xF0u);
        const u8 edge = (u8)(gp & ~g_mfd.prev_gp_dpad);
        g_mfd.prev_gp_dpad = gp;

        // Only trigger on a clean single-direction press (not a simultaneous diagonal) with 160 ms debounce
        if (edge && std::popcount(gp) == 1 && now >= g_mfd.dpad_debounce_until_us) {
            g_mfd.dpad_debounce_until_us = now + 160000ull;
            if (edge & 0x10u) set_next_tyre(NextTyre::Soft, true);        // D-Pad Up    -> SOFT
            else if (edge & 0x40u) set_next_tyre(NextTyre::Hard, true);   // D-Pad Down  -> HARD
            else if (edge & 0x80u) set_next_tyre(NextTyre::Inter, true);  // D-Pad Left  -> INTER
            else if (edge & 0x20u) set_next_tyre(NextTyre::Wet, true);    // D-Pad Right -> WET
        }

        // Strip gamepad D-Pad bits/pressures while driving so steering is never affected (keyboard arrows stay free)
        if (gp) {
            s.digital1 &= (u16)~(u16)(gp & ~s.kb_dpad);
            if ((gp & 0x20u) && !(s.kb_dpad & 0x20u)) s.pressure[0] = 0;   // RIGHT
            if ((gp & 0x80u) && !(s.kb_dpad & 0x80u)) s.pressure[1] = 0;   // LEFT
            if ((gp & 0x10u) && !(s.kb_dpad & 0x10u)) s.pressure[2] = 0;   // UP
            if ((gp & 0x40u) && !(s.kb_dpad & 0x40u)) s.pressure[3] = 0;   // DOWN
        }
    } else {
        g_mfd.prev_gp_dpad = s.gp_dpad;
    }
}

// 0x10D3E8(carPhys, tyre_type, dry_compound, skip_if_unchanged):
// Native tyre mount routine called on pit stops by both sub_00121870 (auto pit stop, lr=0x121A34) and
// sub_005F833C (interactive pit stop, lr=0x5F84DC). Overrides the human player's tyre choice when selected via MFD.
bool tyre_mount_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside || mfd_mode() == MfdMode::off || g_mfd.next_tyre == NextTyre::Auto) return false;

    const u32 lr = (u32)c.lr;
    if (lr != 0x121A34u && lr != 0x5F84DCu) return false;

    const u32 phys = (u32)c.r[3];
    if (!phys || (phys & 3) || !mem::readable(phys + 4444, 4)) return false;
    const u32 car = ld32(phys + 4444);
    if (!is_human_car(car)) return false;

    const NextTyre chosen = g_mfd.next_tyre;
    u32 tyre_type = 0, dry_comp = 0;
    switch (chosen) {
        case NextTyre::Hard:  tyre_type = 0; dry_comp = 0; break;
        case NextTyre::Soft:  tyre_type = 0; dry_comp = 1; break;
        case NextTyre::Inter: tyre_type = 1; dry_comp = 0; break;
        case NextTyre::Wet:   tyre_type = 2; dry_comp = 0; break;
        default: return false;
    }

    c.r[4] = tyre_type;
    c.r[5] = dry_comp;
    c.r[6] = 0;   // force full mount (reset wear/temp, clear punctures, reload dynamics & tread)

    LOG_I("mfd: pit stop mounting %s tyres (type=%u comp=%u, lr=0x%X)",
          tyre_label(chosen), tyre_type, dry_comp, lr);

    inside = true;
    if (sub_0010D3E8) sub_0010D3E8(c);
    inside = false;

    g_mfd.next_tyre = NextTyre::Auto;
    char note[64];
    snprintf(note, sizeof(note), "Fitted %s tyres!", tyre_label(chosen));
    push_tyre_overlay(note, 4500);
    return true;
}
