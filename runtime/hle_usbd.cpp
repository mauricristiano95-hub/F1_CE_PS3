// HLE: cellUsbd, with a Logitech G25 attached: the steering wheel of host::wheel_input (wheel_sdl.cpp).
//
// The game has its own driver for Logitech wheels (0x558244: an extra LDD per model, 0x558310 probe, 0x5589b8 attach;
// the models it knows: 046d:c283 c285 c293 c294 c295 c298 c299 ca03, ca00..ca7f as plain HID). Its report decoder
// 0x55a660 reads the G25 (c299) as:
//   byte 0: hat (low nibble, 8 = centred) | buttons (high nibble)
//   byte 1: buttons (bits 0 and 1: the paddles)
//   byte 2: shifter: bit 7 set = sequential mode (bits 0/1 = up/down, as the paddles); clear = H pattern, gear one-hot
//           in bits 0..3 (1st..4th), 0x10 5th, 0x20 6th, 0x40 reverse (tables at 0x10029968 / 0x10029960), 0 neutral
//   bytes 3-4: steering, 14 bits: byte 4 << 6 | byte 3 >> 2 (0..0x3fff, calibrated against that range)
//   bytes 5, 6, 7: throttle, brake, clutch, 0xff released .. 0 pressed (flags 0x1c0 of 0x559848: inverted)
// and sends the Logitech force-feedback protocol (7-byte commands) on the interrupt-out pipe; ffb() below turns those
// into host::wheel_forces.
// F1CE_WHEEL=0: no wheel (the game sees no USB device); F1CE_WHEEL_LOG=1: logs the transfers and force commands.
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "host.h"
#include "kernel.h"

namespace {
constexpr s32 DEV_ID = 1;
constexpr s32 PIPE_CONTROL = 1, PIPE_IN = 2, PIPE_OUT = 3;
constexpr u16 VID = 0x046d, PID = 0xc299;   // G25
constexpr u32 REPORT_US = 4000;             // input reports every 4 ms (the UI thread samples the wheel as often)

bool enabled() {
    static const bool e = !getenv("F1CE_WHEEL") || atoi(getenv("F1CE_WHEEL")) != 0;
    return e;
}
bool logging() {
    static const bool l = getenv("F1CE_WHEEL_LOG") && atoi(getenv("F1CE_WHEEL_LOG"));
    return l;
}

struct Ldd { u32 ops; u16 vid, pid; };   // CellUsbdLddOps: name, probe, attach, detach (OPDs)
struct Transfer { s32 pipe; u32 buf, len, cb, arg; u8 data[8]; };   // data: an out transfer's, copied when queued

std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<Ldd> g_ldds;
std::deque<Transfer> g_queue;          // control and interrupt-out transfers, completed at once
std::optional<Transfer> g_in;          // the pending interrupt-in transfer, completed with the next report
u32 g_desc = 0, g_desc_size = 0;       // the descriptors (device, configuration, interface, HID, endpoints)
u32 g_private = 0;
bool g_attached = false, g_thread = false;
u64 g_registered_at = 0;

void make_descriptors() {
    static const u8 d[] = {
        18, 1, 0x00, 0x02, 0, 0, 0, 16, VID & 0xff, VID >> 8, PID & 0xff, PID >> 8, 0x22, 0x12, 1, 2, 0, 1,   // device
        9, 2, 41, 0, 1, 1, 0, 0x80, 250,                                                                     // configuration
        9, 4, 0, 0, 2, 3, 0, 0, 0,                                                                           // interface (HID)
        9, 0x21, 0x11, 0x01, 0x21, 1, 0x22, 111, 0,                                                          // HID
        7, 5, 0x81, 3, 16, 0, 2,                                                                             // interrupt in
        7, 5, 0x01, 3, 16, 0, 2,                                                                             // interrupt out
    };
    g_desc_size = sizeof(d);
    g_desc = mem::alloc(g_desc_size, 16);
    memcpy(gmem(g_desc), d, sizeof(d));
}

// the G25 input report from the host wheel
void make_report(u8* r) {
    const host::WheelInput w = host::wheel_input();
    memset(r, 0, 16);
    static const s8 hat_of[16] = {8, 0, 4, 8, 6, 7, 5, 6, 2, 1, 3, 2, 8, 0, 4, 8};   // index: up|down<<1|left<<2|right<<3
    const u32 b = w.buttons;
    const int dir = (b & host::WBTN_UP ? 1 : 0) | (b & host::WBTN_DOWN ? 2 : 0) | (b & host::WBTN_LEFT ? 4 : 0) |
                    (b & host::WBTN_RIGHT ? 8 : 0);
    r[0] = (u8)hat_of[dir] | (u8)((b & 0xf) << 4);   // cross, square, circle, triangle
    // byte 1, bit by bit: shift up, shift down, R2, L2, select, start, R3, L3 (WBTN_* bits 4..11)
    r[1] = (u8)((b >> 4) & 0xff);
    if (w.hshifter) {
        static const u8 gear_bits[8] = {0x40, 0, 1, 2, 4, 8, 0x10, 0x20};   // index gear + 1
        r[2] = gear_bits[std::clamp((int)w.gear, -1, 6) + 1];
    } else {
        r[2] = 0x80;   // sequential mode, nothing pressed: the paddles shift
    }
    const int steer = std::clamp((int)std::lround((w.steer + 1.0f) * 0.5f * 0x3fff), 0, 0x3fff);
    r[3] = (u8)((steer & 0x3f) << 2);
    r[4] = (u8)(steer >> 6);
    auto pedal = [](float v) { return (u8)(255 - std::clamp((int)std::lround(v * 255.0f), 0, 255)); };
    r[5] = pedal(w.throttle);
    r[6] = pedal(w.brake);
    r[7] = pedal(w.clutch);
}

// ------------------------------------------------------------------ Logitech force-feedback protocol
// The classic protocol's byte 0 is slot mask << 4 | command, but this game's send path 0x558160 overwrites it with 3
// whenever the command is below 6 (all downloads, plays and stops): only the force type in byte 1 and its parameters
// reach the wheel. What it sends (Quick Race, 2026-11-11):
//   03 00 00 .. 00           at start: all forces off
//   f8 02                    Driving Force Pro 900 degree mode
//   03 08 L1 xx 00 00 00     variable force, level L1 (0x80 none, above pushes left), every controller update
//   03 0b 7f 7f KK 00 ff     high-resolution spring at the centre, K2 | K1 (4 bits each: 0..15) growing with speed
// So each force type is a slot of its own, replaced by its next command. The variable force is dropped when the game
// stops refreshing it (pause, end of the race) and everything when the controller update stops (rumble_update).
struct Force { bool on = false; u8 p[5] = {}; };
Force g_force[16];               // by type (0x00 constant .. 0x0e friction)
u64 g_level_at = 0;              // time of the last variable / constant force
u64 g_cmd_at = 0;                // time of the last command
bool g_autocenter = true;        // the wheel's own centring spring (on at power-up)
u8 g_autocenter_k = 2, g_autocenter_clip = 0x80;

void apply_forces() {
    host::WheelForces f;
    auto k11 = [](u8 hi, int lo) { return (float)((hi << 3) | lo) / 2047.0f * 2.0f - 1.0f; };   // 11-bit position
    const bool level_fresh = time_usec() - g_level_at < 300000;
    // (above 0x80 pushes left: steered right with the wheel held, the game answers with levels above 0x80 that centre it)
    if (g_force[0x00].on && level_fresh) f.constant += (128 - g_force[0x00].p[0]) / 127.0f;
    if (g_force[0x08].on && level_fresh) f.constant += (128 - g_force[0x08].p[0]) / 127.0f;
    f.constant = std::clamp(f.constant, -1.0f, 1.0f);
    for (u8 type : {0x01, 0x0b}) {   // spring / high-resolution spring: dead band [D1, D2], K2 | K1, inversion, clip
        const Force& s = g_force[type];
        if (!s.on) continue;
        const bool hi = type == 0x0b;
        const float d1 = hi ? k11(s.p[0], (s.p[3] >> 1) & 7) : s.p[0] / 255.0f * 2 - 1;
        const float d2 = hi ? k11(s.p[1], (s.p[3] >> 5) & 7) : s.p[1] / 255.0f * 2 - 1;
        const float kmax = hi ? 15.0f : 7.0f;
        float k1 = (s.p[2] & (hi ? 0xf : 7)) / kmax, k2 = ((s.p[2] >> 4) & (hi ? 0xf : 7)) / kmax;
        if (s.p[3] & 1) k1 = -k1;
        if (s.p[3] & 0x10) k2 = -k2;
        f.spring = k1 != 0 || k2 != 0;
        f.spring_center = (d1 + d2) * 0.5f;
        f.spring_deadband = std::max(0.0f, (d2 - d1) * 0.5f);
        f.spring_left = k1;
        f.spring_right = k2;
        f.spring_sat = s.p[4] / 255.0f;
    }
    for (u8 type : {0x02, 0x0c}) {   // damper / high-resolution damper: K1, S1, K2, S2 (clip on the high-res one)
        const Force& s = g_force[type];
        if (!s.on) continue;
        const float kmax = type == 0x0c ? 15.0f : 7.0f;
        f.damper = true;
        f.damper_left = (s.p[0] & 0xf) / kmax * (s.p[1] & 1 ? -1 : 1);
        f.damper_right = (s.p[2] & 0xf) / kmax * (s.p[3] & 1 ? -1 : 1);
        f.damper_sat = type == 0x0c ? s.p[4] / 255.0f : 1.0f;
    }
    if (const Force& s = g_force[0x0e]; s.on) {   // friction: K1, K2, clip
        f.friction = true;
        f.friction_coef = std::max(s.p[0], s.p[1]) / 255.0f;
        f.friction_sat = s.p[2] / 255.0f;
    }
    // the wheel's own spring while the game has none of its own
    if (g_autocenter && !g_force[0x01].on && !g_force[0x0b].on) {
        f.spring = f.autocenter = true;
        f.spring_left = f.spring_right = std::min(g_autocenter_k / 7.0f, 1.0f);
        f.spring_sat = g_autocenter_clip / 255.0f;
    }
    host::wheel_forces(f);
}

void forces_off() {
    for (Force& s : g_force) s = {};
    apply_forces();
}

void ffb(const u8* c, u32 len) {
    if (len < 7) return;
    g_cmd_at = time_usec();
    if (logging()) LOG_I("wheel: ffb %02x %02x %02x %02x %02x %02x %02x", c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
    switch (c[0]) {
        case 0xf8:   // extended commands
            if (c[1] == 0x81) {   // set range (G25/G27): degrees in c[2] | c[3] << 8
                LOG_I("wheel: the game sets the steering range to %d degrees", c[2] | c[3] << 8);
                host::wheel_lock_deg(c[2] | c[3] << 8);
            } else if (c[1] == 0x02 || c[1] == 0x03) {   // Driving Force Pro 900 / 200 degree mode
                LOG_I("wheel: the game sets the steering range to %d degrees", c[1] == 0x02 ? 900 : 200);
                host::wheel_lock_deg(c[1] == 0x02 ? 900 : 200);
            }
            return;
        case 0xf5: g_autocenter = false; break;   // auto-centre off
        case 0x14: g_autocenter = true; break;    // auto-centre on
        case 0xfe:                                // auto-centre spring parameters
            if (c[1] != 0x0d) return;
            g_autocenter_k = std::max(c[2], c[3]) & 0xf;
            g_autocenter_clip = c[4];
            break;
        default: {
            const u8 type = c[1];
            if ((c[0] & 0xf) >= 6) {
                if (logging()) LOG_I("wheel: ffb command %02x not handled", c[0]);
                return;
            }
            if (type == 0x00 && !c[2] && !c[3] && !c[4] && !c[5] && !c[6]) {   // (a stop of every slot)
                forces_off();
                return;
            }
            if (type > 0x0e) return;
            g_force[type].on = true;
            memcpy(g_force[type].p, c + 2, 5);
            if (type == 0x00 || type == 0x08) g_level_at = time_usec();
            break;
        }
    }
    apply_forces();
}

// ------------------------------------------------------------------ the usbd thread
void complete(PPUContext& c, const Transfer& t, s32 count) {
    if (t.cb) guest_call(c, t.cb, {0, (u64)count, t.arg});
}

void usbd_thread(PPUContext& c) {
    u64 next_report = 0;
    bool was_connected = false;
    for (;;) {
        Transfer t{};
        bool have = false, attach = false, detach = false;
        {
            std::unique_lock lk(g_mutex);
            g_cv.wait_for(lk, std::chrono::microseconds(2000), [] { return !g_queue.empty(); });
            if (!g_queue.empty()) {
                t = g_queue.front();
                g_queue.pop_front();
                have = true;
            }
        }
        if (have) {
            if (t.pipe == PIPE_OUT) ffb(t.data, std::min<u32>(t.len, 8));
            complete(c, t, (s32)t.len);
            continue;
        }
        // plugging and unplugging (the registrations settle first: the game registers its models in a loop)
        const bool connected = host::wheel_input().connected;
        {
            std::lock_guard lk(g_mutex);
            if (connected && !g_attached && time_usec() - g_registered_at > 200000) attach = true;
            if (!connected && g_attached) detach = true;
        }
        if (connected != was_connected) {
            LOG_I("wheel: %s", connected ? "connected" : "disconnected");
            was_connected = connected;
        }
        if (attach) {
            Ldd ldd{};
            {
                std::lock_guard lk(g_mutex);
                for (const Ldd& l : g_ldds)
                    if (l.vid == VID && l.pid == PID) ldd = l;
                if (!ldd.ops) continue;
                g_attached = true;
            }
            const s32 probe = (s32)guest_call(c, ld32(ldd.ops + 4), {(u64)DEV_ID});
            const s32 att = probe == 0 ? (s32)guest_call(c, ld32(ldd.ops + 8), {(u64)DEV_ID}) : -1;
            LOG_I("wheel: Logitech G25 (%04x:%04x) probe %d attach %d", VID, PID, probe, att);
            if (att != 0) {
                std::lock_guard lk(g_mutex);
                g_registered_at = time_usec() + 5000000;   // (retry in 5 s)
                g_attached = false;
            }
            continue;
        }
        if (detach) {
            Ldd ldd{};
            {
                std::lock_guard lk(g_mutex);
                for (const Ldd& l : g_ldds)
                    if (l.vid == VID && l.pid == PID) ldd = l;
                g_attached = false;
                g_in.reset();
            }
            for (Force& s : g_force) s = {};
            host::wheel_forces({});
            if (ldd.ops) guest_call(c, ld32(ldd.ops + 12), {(u64)DEV_ID});
            LOG_I("wheel: detached");
            continue;
        }
        const u64 now = time_usec();
        // the game stopped refreshing its forces: the variable force goes after 300 ms, everything after 1 s
        if (g_cmd_at && now - g_cmd_at > 1000000) {
            g_cmd_at = 0;
            forces_off();
        } else if (g_force[0x08].on && now - g_level_at > 300000 && now - g_level_at < 400000) {
            apply_forces();
        }
        if (now < next_report) continue;
        std::optional<Transfer> in;
        {
            std::lock_guard lk(g_mutex);
            in.swap(g_in);
        }
        if (!in) continue;
        next_report = std::max(next_report + REPORT_US, now);
        u8 r[16];
        make_report(r);
        const u32 n = std::min<u32>(in->len, 16);
        memcpy(gmem(in->buf), r, n);
        guest_written(in->buf, n);
        if (logging()) {
            static u64 next_log = 0;
            if (now >= next_log) {
                next_log = now + 500000;
                LOG_I("wheel: report %02x %02x %02x %02x %02x %02x %02x %02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            }
        }
        complete(c, *in, (s32)n);
    }
}

void start_thread() {
    if (g_thread) return;
    g_thread = true;
    make_descriptors();
    thread_create_host("usbd", usbd_thread);
}
}  // namespace

HLE_FUNC(cellUsbdInit) { RET(CELL_OK); }
HLE_FUNC(cellUsbdEnd) { RET(CELL_OK); }
HLE_FUNC(cellUsbdRegisterExtraLdd) {
    const u32 ops = (u32)ARG(0);
    const u16 vid = (u16)ARG(1), pid = (u16)ARG(2);
    if (logging()) LOG_I("usbd: register LDD '%s' %04x:%04x", gstr(ld32(ops)).c_str(), vid, pid);
    if (!enabled()) RET(CELL_OK);
    std::lock_guard lk(g_mutex);
    g_ldds.push_back({ops, vid, pid});
    g_registered_at = time_usec();
    start_thread();
    RET(CELL_OK);
}
HLE_FUNC(cellUsbdUnregisterExtraLdd) {
    std::lock_guard lk(g_mutex);
    std::erase_if(g_ldds, [&](const Ldd& l) { return l.ops == (u32)ARG(0); });
    RET(CELL_OK);
}
// the next descriptor of this type after ptr (from the first when ptr is null), null at the end
HLE_FUNC(cellUsbdScanStaticDescriptor) {
    const u32 ptr = (u32)ARG(1);
    const u8 type = (u8)ARG(2);
    if ((s32)ARG(0) != DEV_ID || !g_desc) RET(0);
    u32 p = ptr ? ptr + ld8(ptr) : g_desc;
    while (p >= g_desc && p < g_desc + g_desc_size) {
        if (ld8(p + 1) == type) RET(p);
        p += std::max<u8>(ld8(p), 1);
    }
    RET(0);
}
HLE_FUNC(cellUsbdGetPrivateData) { RET((s32)ARG(0) == DEV_ID ? g_private : 0); }
HLE_FUNC(cellUsbdSetPrivateData) {
    if ((s32)ARG(0) == DEV_ID) g_private = (u32)ARG(1);
    RET(CELL_OK);
}
// endpoint descriptor null: the control pipe
HLE_FUNC(cellUsbdOpenPipe) {
    const u32 ed = (u32)ARG(1);
    if ((s32)ARG(0) != DEV_ID) RET((s32)0x80110004);   // CELL_USBD_ERROR_DEVICE_NOT_FOUND
    RET(!ed ? PIPE_CONTROL : (ld8(ed + 2) & 0x80) ? PIPE_IN : PIPE_OUT);
}
HLE_FUNC(cellUsbdClosePipe) { RET(CELL_OK); }
HLE_FUNC(cellUsbdControlTransfer) {
    const u32 req = (u32)ARG(1);
    if (logging()) {
        const u8* q = gmem(req);
        LOG_I("usbd: control %02x %02x %02x%02x %02x%02x %02x%02x", q[0], q[1], q[3], q[2], q[5], q[4], q[7], q[6]);
    }
    std::lock_guard lk(g_mutex);
    g_queue.push_back({(s32)ARG(0), (u32)ARG(2), 0, (u32)ARG(3), (u32)ARG(4), {}});
    g_cv.notify_one();
    RET(CELL_OK);
}
HLE_FUNC(cellUsbdInterruptTransfer) {
    const s32 pipe = (s32)ARG(0);
    Transfer t{pipe, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), {}};
    if (pipe == PIPE_OUT) memcpy(t.data, gmem(t.buf), std::min<u32>(t.len, 8));
    std::lock_guard lk(g_mutex);
    if (pipe == PIPE_IN) {
        g_in = t;
    } else {
        g_queue.push_back(t);
        g_cv.notify_one();
    }
    RET(CELL_OK);
}
HLE_FUNC(cellUsbdGetDeviceLocation) { RET(CELL_OK); }
