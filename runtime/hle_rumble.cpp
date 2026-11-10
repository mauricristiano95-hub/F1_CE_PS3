// Controller vibration, ported from the game's 02.00 update. This 01.00 build has no pad vibration (no
// cellPadSetActDirect import); update 02.00 added DualShock 3 vibration in 0x3591e8, called at the end of the per-frame
// controller update 0x35af88 (= 01.00 0x3574c0, the same function without the pad branch) as
//   rumble(controller, pad, driving && controller byte +0x15)
// where driving is the update's second argument (1 only while the player drives) and +0x15 the vibration flag, copied
// next to the wheel force-feedback flag +0x14 in both versions (0x35560c here; the controller options' Vibration
// toggle, "controllervibration"). Here F1CE_RUMBLE replaces that flag, which is 0 in 01.00 profiles. The car (controller +0x160 -> +0x24)
// has the same layout in both versions:
//   +0x7b4 / +0x7e4 (u16) impact flags, +0x7b8 / +0x7e8 impact strength  -> small motor when a flagged strength is > 1
//   +0x94c engine speed in rad/s: between 0 and 1200 rpm (stall, start)  -> large motor rpm / 10
//   +0x530 4 wheel loads; when their sum is > 100, for each wheel with contact bit w of +0x980 and surface +0x920:
//     tarmac ((id - 1) >> 3 == 0, or id 0x27..0x29) with bit w + 8 of +0x97c  -> small motor
//     kerb (1): speed +0x3c0 <= 35 m/s  -> large motor += speed * 5, faster  -> small motor
//     run-off (2, 3)  -> large motor += sqrt(speed) * 6.25, the sum capped at 127
//   +0x554 tyre grip below 0.92  -> large motor += 127 * min((0.92 - grip) * 100, 1)
// The large motor is capped at 255, the small one is on/off. Vibration may last 240 controller updates, then stops for
// 80 (the counter runs back towards 0 while there is none).
// F1CE_RUMBLE=<0..200> strength in percent (default 100, 0 off); F1CE_RUMBLE_LOG=1 logs the inputs 10 times a second.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

#include "host.h"
#include "kernel.h"

namespace {
std::atomic<u64> g_last_out{0};   // time of the last motor update from the controller update

float strength() {
    static const float s = getenv("F1CE_RUMBLE") ? std::clamp(atoi(getenv("F1CE_RUMBLE")), 0, 200) / 100.0f : 1.0f;
    return s;
}
float ldf(u32 ea) { return std::bit_cast<float>(ld32(ea)); }

void motors(bool small, int large) {
    host::pad_rumble(std::min(large / 255.0f * strength(), 1.0f), small ? std::min(strength(), 1.0f) : 0.0f);
    g_last_out = time_usec();
}

// 0x3591e8 of version 02.00
void rumble(u32 ctl, bool active) {
    static s32 counter = 0;   // 02.00 keeps it in the pad's controller state (+0xc4)
    static const bool log = getenv("F1CE_RUMBLE_LOG") && atoi(getenv("F1CE_RUMBLE_LOG"));
    static u64 next_log = 0;
    const u32 car = ld32(ld32(ctl + 0x160) + 0x24);
    bool small = false;
    s32 large = 0;
    if (active && car) {
        if ((ld16(car + 0x7b4) && ldf(car + 0x7b8) > 1.0f) || (ld16(car + 0x7e4) && ldf(car + 0x7e8) > 1.0f)) small = true;
        const float omega = ldf(car + 0x94c);
        if (0.0f < omega * 9.549297f && omega * 9.549297f < 1200.0f) large = (s32)(omega * 0.95492977f);
        float load = 0;
        for (int w = 0; w < 4; w++) load += ldf(car + 0x530 + 4 * w);
        const float speed = ldf(car + 0x3c0);
        if (100.0f < load) {
            const s32 contact = (s32)ld32(car + 0x980), slip = (s32)ld32(car + 0x97c);
            for (int w = 0; w < 4; w++) {
                const s32 id = (s32)ld32(car + 0x920 + 4 * w);
                if (!((contact >> w) & 1) || id == 0) continue;
                if (id < 0x21 ? (id - 1) >> 3 == 0 : (u32)(id - 0x27) < 3) {
                    if ((slip >> (w + 8)) & 1) small = true;
                }
                if (id < 0x21) {
                    const s32 kind = (id - 1) >> 3;
                    if (kind == 1) {
                        if (speed <= 35.0f) large += (s32)(speed * 5.0f);
                        else small = true;
                    }
                    if (kind == 2 || kind == 3) {
                        large += (s32)(std::sqrt(std::max(speed, 0.0f)) * 6.25f);
                        if (large > 0x7f) large = 0x7f;
                    }
                }
            }
        }
        const float grip = std::min(std::max(0.92f - ldf(car + 0x554), 0.0f) * 100.0f, 1.0f);
        large = std::min((s32)(grip * 127.0f) + large, 255);
        if (log && time_usec() >= next_log) {
            next_log = time_usec() + 100000;
            LOG_I("rumble: speed %.1f rpm %.0f load %.0f grip %.3f surf %d %d %d %d contact %x slip %x hit %u %.2f %u %.2f "
                  "count %d -> small %d large %d",
                  speed, omega * 9.549297f, load, ldf(car + 0x554), (s32)ld32(car + 0x920), (s32)ld32(car + 0x924),
                  (s32)ld32(car + 0x928), (s32)ld32(car + 0x92c), ld32(car + 0x980) & 0xf, (ld32(car + 0x97c) >> 8) & 0xf,
                  ld16(car + 0x7b4), ldf(car + 0x7b8), ld16(car + 0x7e4), ldf(car + 0x7e8), counter, small, large);
        }
    }
    if (small || large > 0) {
        if (++counter > 240) {
            counter = -80;
            motors(false, 0);
        } else {
            motors(counter >= 0 && small, counter >= 0 ? large & 0xff : 0);
        }
    } else {
        counter = counter > 0 ? counter - 1 : counter < 0 ? counter + 1 : 0;
        motors(false, 0);
    }
}
}  // namespace

void sub_003574C0(PPUContext& __restrict c);
void ai_drive_update(PPUContext& c, u32 ctl, bool driving);   // hle_aidrive.cpp

// the per-frame controller update (r3 controller, r4 driving); 02.00 vibrates the pad at its end
bool controller_update_hook(PPUContext& c) {
    static thread_local bool inside = false;
    if (inside) return false;
    const u32 ctl = (u32)c.r[3];
    const bool driving = (u32)c.r[4] != 0;
    // the update returns at once during its first 15 calls (counter at 0x70f648), without reaching the vibration
    const bool runs = (s32)ld32(ld32(0x70f648)) >= 15;
    inside = true;
    sub_003574C0(c);
    inside = false;
    if (runs) ai_drive_update(c, ctl, driving);
    if (!runs || strength() <= 0) return true;
    const bool flag = ld8(ctl + 0x15) != 0;
    static int last_flag = -1;
    if (last_flag != (int)flag) {
        last_flag = flag;
        LOG_I("rumble: controller %08x vibration flag %d", ctl, (int)flag);
    }
    rumble(ctl, driving);   // the in-game Vibration option (+0x15) is off in 01.00 profiles: F1CE_RUMBLE decides
    return true;
}

// once per pad poll (cellPadGetData): motors off when the controller update stops (front end, loading)
void rumble_update() {
    const u64 last = g_last_out;
    if (last && time_usec() - last > 200000) {
        host::pad_rumble(0, 0);
        g_last_out = 0;
    }
}
