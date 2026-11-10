// AI driving of the player's car (experiment, groundwork for emulating whole races). Notes: MODDING_IDEAS.md.
// The game has a debug Lua library "cheat" (class __cheat__, registered with the other libraries by 0x1E39A8, so it
// exists in the retail game; table at 0x100420f0, {name, opd, 0} entries) whose functions act on the race manager
// (*0x100de2a0):
//   toggle_ai_control 0x1F4C64, advance_one_lap 0x1F4CD4, goto_last_lap 0x1F4D44, retire 0x1F51A0,
//   make_player_car_pit 0x1F5280, make_ai_car_pit 0x1F5210, recover_car_to_racing_line 0x1F4FD4, ...
// They ignore their lua_State argument. toggle_ai_control looks for the human driver by its byte +0x206, which is 0
// in a Quick Race, so the retail cheat passes NULL on and crashes; this code finds the driver itself and calls
// 0x1546D8(driver), which flips the driver's AI override byte +0x224:
//   0 -> 1: 0x1201D8(entity): the AI drives (entity +0x138 = controller +0x28, per-car AI table entry = 1)
//   1 -> 0: 0x12014C(entity): the pad drives again (entity +0x138 = controller +0x24, table entry 0)
// Driver objects: the manager's list *(mgr + 0x44) (sentinel; node +0 next, node +8 driver) holds the human drivers
// only; driver +0x220 = car entity, entity +0x1C = physics car (= controller +0x160 -> +0x24, speed at +0x3C0).
// The race's own hand-overs use the same pair: 0x15439C(driver) gives the car to the AI (pit lane, end of race,
// black flag ...) and 0x154980(driver) gives it back unless +0x224 is set, so the override survives them.
// F1CE_AI_DRIVE (launcher: Developer Tools, "AI drives the player's car"): set, F8 toggles AI control in a race
// (either way); "key" = F8 only, a number s = also toggles once s seconds after the player first drives in each race
// (0 = at once) and logs the car's speed every 2 s. Tested 2026-11-10: the car starts and races Silverstone in traffic.
#include <bit>
#include <cstdlib>
#include <cstring>

#include "host.h"
#include "kernel.h"

void sub_001546D8(PPUContext& __restrict c);

namespace {
// the human driver whose physics car is car, or 0
u32 find_driver(u32 car) {
    const u32 mgr = ld32(0x100de2a0);
    const u32 head = mgr ? ld32(mgr + 0x44) : 0;
    for (u32 n = head ? ld32(head) : 0, k = 0; n && n != head && k < 64; n = ld32(n), k++) {
        const u32 d = ld32(n + 8), entity = ld32(d + 0x220);
        if (entity && ld32(entity + 0x1c) == car) return d;
    }
    return 0;
}

void toggle(PPUContext& c, u32 ctl, u32 car, const char* why) {
    const u32 player = car ? find_driver(car) : 0;
    if (!player) {
        LOG_W("ai drive: %s: no human driver for controller %08x car %08x", why, ctl, car);
        return;
    }
    const u64 r3 = c.r[3], lr = c.lr;
    const u32 fn = c.cur_fn;
    c.r[3] = player;
    sub_001546D8(c);
    c.r[3] = r3;
    c.lr = lr;
    c.cur_fn = fn;
    LOG_W("ai drive: %s: %s drives (driver %08x)", why, ld8(player + 0x224) ? "the AI" : "the player", player);
}
}  // namespace

// from the per-frame controller update hook (hle_rumble.cpp), after the update ran: ctl = controller,
// driving = the update's second argument (0 while the AI drives)
void ai_drive_update(PPUContext& c, u32 ctl, bool driving) {
    static const char* env = getenv("F1CE_AI_DRIVE");
    if (!env) return;
    static const bool timed = strcmp(env, "key") != 0;
    static const u64 delay = (u64)(atof(env) * 1e6);
    static u64 first_drive = 0, last_call = 0, next_log = 0;
    static bool done = false, key_was = false;
    const u64 now = time_usec();
    const u32 p = ld32(ctl + 0x160), car = p ? ld32(p + 0x24) : 0;
    if (now - last_call > 1000000) {   // no update for a second: loading, a new session follows
        first_drive = 0;
        done = false;
    }
    last_call = now;
    const bool key = (host::debug_keys() & host::DK_AI_TOGGLE) != 0;
    if (key && !key_was) toggle(c, ctl, car, "F8");
    key_was = key;
    if (driving && !first_drive) first_drive = now;
    if (timed && !done && first_drive && now - first_drive >= delay) {
        done = true;
        const u32 d = car ? find_driver(car) : 0;
        if (!d || !ld8(d + 0x224)) toggle(c, ctl, car, "F1CE_AI_DRIVE");   // (unless F8 already gave it to the AI)
    }
    if (timed && now >= next_log && car) {
        next_log = now + 2000000;
        LOG_I("ai drive: driving %d speed %.1f m/s", (int)driving, std::bit_cast<float>(ld32(car + 0x3c0)));
    }
}
