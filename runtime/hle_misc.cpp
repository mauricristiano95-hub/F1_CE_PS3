// HLE: sys_io (pad/keyboard/mouse), sys_fs (cellFs*), cellSysutil, cellAudio, cellUsbd.
#include "host.h"
#include "kernel.h"
#include "lv2_fs.h"
#include "rsx.h"

#include <algorithm>
#include <array>
#include <deque>
#include <vector>
void set_thread_name(const char* name);   // core.cpp

// ================================================================== sys_io
HLE_FUNC(cellPadInit) { RET(CELL_OK); }
HLE_FUNC(cellPadEnd) { RET(CELL_OK); }
// CellPadInfo: max_connect, now_connect, system_info, vendor_id[127], product_id[127], status[127]
HLE_FUNC(cellPadGetInfo) {
    u32 p = (u32)ARG(0);
    memset(gmem(p), 0, 12 + 127 * 2 * 2 + 127);
    st32(p, 7);
    st32(p + 4, 1);
    st16(p + 12, 0x054C);              // vendor_id[0]
    st16(p + 12 + 254, 0x0268);        // product_id[0]
    st8(p + 12 + 508, 1);              // status[0] = CELL_PAD_STATUS_CONNECTED
    RET(CELL_OK);
}
void quick_race_overrides(u32 digital1, u32 lx, u32 ly);   // hle_quickrace.cpp (F1CE_QR_*)
void rumble_update();                                        // hle_rumble.cpp
// CellPadData: s32 len; u16 button[64]
HLE_FUNC(cellPadGetData) {
    u32 port = (u32)ARG(0), p = (u32)ARG(1);
    if (port != 0) { st32(p, 0); RET(CELL_OK); }
    host::PadState s = host::pad_state();
    quick_race_overrides(s.digital1, s.lx, s.ly);
    rumble_update();
    st32(p, 24);
    u16 b[24] = {};
    b[0] = 0;
    b[1] = (7 << 4) | 2;   // CELL_PAD_LEN_CHANGE_DEFAULT
    b[2] = s.digital1;
    b[3] = s.digital2;
    b[4] = s.rx; b[5] = s.ry; b[6] = s.lx; b[7] = s.ly;
    for (int i = 0; i < 12; i++) b[8 + i] = s.pressure[i];
    for (int i = 0; i < 24; i++) st16(p + 4 + i * 2, b[i]);
    RET(CELL_OK);
}
HLE_FUNC(cellPadInfoPressMode) { RET(1); }
HLE_FUNC(cellPadInfoSensorMode) { RET(0); }
HLE_FUNC(cellPadSetPressMode) { RET(CELL_OK); }
HLE_FUNC(cellPadSetSensorMode) { RET(CELL_OK); }

HLE_FUNC(cellKbInit) { RET(CELL_OK); }
HLE_FUNC(cellKbEnd) { RET(CELL_OK); }
HLE_FUNC(cellKbGetInfo) { memset(gmem((u32)ARG(0)), 0, 0x20); st32((u32)ARG(0), 1); RET(CELL_OK); }
HLE_FUNC(cellKbRead) { memset(gmem((u32)ARG(1)), 0, 16); RET(CELL_OK); }
HLE_FUNC(cellKbClearBuf) { RET(CELL_OK); }
HLE_FUNC(cellKbSetCodeType) { RET(CELL_OK); }
HLE_FUNC(cellKbSetReadMode) { RET(CELL_OK); }
HLE_FUNC(cellKbGetConfiguration) { memset(gmem((u32)ARG(1)), 0, 12); RET(CELL_OK); }
HLE_FUNC(cellKbCnvRawCode) { RET(0); }
HLE_FUNC(cellMouseInit) { RET(CELL_OK); }
HLE_FUNC(cellMouseEnd) { RET(CELL_OK); }
HLE_FUNC(cellMouseGetInfo) { memset(gmem((u32)ARG(0)), 0, 0x30); st32((u32)ARG(0), 1); RET(CELL_OK); }
HLE_FUNC(cellMouseGetData) { memset(gmem((u32)ARG(1)), 0, 8); RET(CELL_OK); }
HLE_FUNC(cellMouseClearBuf) { RET(CELL_OK); }

// ================================================================== sys_fs (cellFs*)
HLE_FUNC(cellFsOpen) { RET(fs::open(gstr((u32)ARG(0)), (s32)ARG(1), (u32)ARG(2))); }
HLE_FUNC(cellFsRead) { RET(fs::read((u32)ARG(0), (u32)ARG(1), ARG(2), (u32)ARG(3))); }
HLE_FUNC(cellFsWrite) { RET(fs::write((u32)ARG(0), (u32)ARG(1), ARG(2), (u32)ARG(3))); }
HLE_FUNC(cellFsClose) { RET(fs::close((u32)ARG(0))); }
HLE_FUNC(cellFsLseek) { RET(fs::lseek((u32)ARG(0), (s64)ARG(1), (u32)ARG(2), (u32)ARG(3))); }
HLE_FUNC(cellFsStat) { RET(fs::stat(gstr((u32)ARG(0)), (u32)ARG(1))); }
HLE_FUNC(cellFsFstat) { RET(fs::fstat((u32)ARG(0), (u32)ARG(1))); }

// ================================================================== cellSysutil
namespace {
struct SysutilCb { u32 func = 0, userdata = 0; };
std::array<SysutilCb, 4> g_sysutil_cbs;
std::mutex g_sysutil_mutex;
struct PendingCall { u32 func, a0, a1, a2; };
std::deque<PendingCall> g_pending;   // callbacks delivered on cellSysutilCheckCallback
}  // namespace

void sysutil_post_event(u32 status, u32 param) {
    std::lock_guard lk(g_sysutil_mutex);
    for (auto& cb : g_sysutil_cbs)
        if (cb.func) g_pending.push_back({cb.func, status, param, cb.userdata});
}

HLE_FUNC(cellSysutilRegisterCallback) {
    u32 slot = (u32)ARG(0);
    if (slot > 3) RET(CELL_EINVAL);
    std::lock_guard lk(g_sysutil_mutex);
    g_sysutil_cbs[slot] = {(u32)ARG(1), (u32)ARG(2)};
    RET(CELL_OK);
}
HLE_FUNC(cellSysutilUnregisterCallback) {
    u32 slot = (u32)ARG(0);
    if (slot > 3) RET(CELL_EINVAL);
    std::lock_guard lk(g_sysutil_mutex);
    g_sysutil_cbs[slot] = {};
    RET(CELL_OK);
}
HLE_FUNC(cellSysutilCheckCallback) {
    if (host::quit_requested()) {
        static bool sent = false;
        if (!sent) { sent = true; sysutil_post_event(0x0101, 0); }   // CELL_SYSUTIL_REQUEST_EXITGAME
    }
    for (;;) {
        PendingCall p;
        {
            std::lock_guard lk(g_sysutil_mutex);
            if (g_pending.empty()) break;
            p = g_pending.front();
            g_pending.pop_front();
        }
        guest_call(c, p.func, {p.a0, p.a1, p.a2});
    }
    RET(CELL_OK);
}
HLE_FUNC(cellSysutilGetSystemParamInt) {
    u32 id = (u32)ARG(0);
    s32 v = 0;
    switch (id) {
        case 0x0111: v = 1; break;   // language: English (US)
        case 0x0112: v = 1; break;   // enter button: cross
        case 0x0114: v = 0; break;   // date format
        case 0x0115: v = 1; break;   // time format 24h
        case 0x0116: v = 60; break;  // time zone (minutes)
        case 0x0117: v = 0; break;   // summer time
        case 0x0121: v = 9; break;   // game parental level
        default: LOG_W("cellSysutilGetSystemParamInt(0x%x) -> 0", id);
    }
    st32((u32)ARG(1), (u32)v);
    RET(CELL_OK);
}
HLE_FUNC(cellSysutilGetSystemParamString) {
    gstr_write((u32)ARG(1), (u32)ARG(0) == 0x0113 ? "Player" : "", (u32)ARG(2));
    RET(CELL_OK);
}

// CellVideoOutState: state, colorSpace, reserved[6], displayMode{resolutionId, scanMode, conversion, aspect, reserved[2], refreshRates}
HLE_FUNC(cellVideoOutGetState) {
    u32 p = (u32)ARG(2);
    u8 id, aspect;
    rsx::video_mode(&id, &aspect);
    memset(gmem(p), 0, 16);
    st8(p + 1, 1);       // RGB
    st8(p + 8, id);
    st8(p + 9, 0);       // progressive
    st8(p + 11, aspect);   // 1 4:3, 2 16:9
    st16(p + 14, 1);     // 59.94 Hz
    RET(CELL_OK);
}
HLE_FUNC(cellVideoOutGetResolution) {
    u32 id = (u32)ARG(0), p = (u32)ARG(1);
    u16 w = 1280, h = 720;
    switch (id) {
        case 1: w = 1920; h = 1080; break;
        case 4: w = 720; h = 480; break;
        case 5: w = 720; h = 576; break;
    }
    st16(p, w);
    st16(p + 2, h);
    RET(CELL_OK);
}
HLE_FUNC(cellVideoOutConfigure) { RET(CELL_OK); }
// CellAudioOutState: state, encoder, reserved[6], downMixer, soundMode{type, channel, fs, reserved, layout}
HLE_FUNC(cellAudioOutGetState) {
    u32 p = (u32)ARG(2);
    memset(gmem(p), 0, 20);
    st8(p + 16 - 4, 0);
    st8(p + 12, 0);   // LPCM
    st8(p + 13, 2);   // channels
    st8(p + 14, 4);   // 48 kHz
    st32(p + 16, 1);  // 2ch layout
    RET(CELL_OK);
}
HLE_FUNC(cellAudioOutGetSoundAvailability) { RET((u32)ARG(1) == 0 ? 2 : 0); }
HLE_FUNC(cellAudioOutConfigure) { RET(CELL_OK); }

// cellMsgDialogOpen(type, msg, callback, userdata, extparam) -- auto-confirm dialogs with buttons
HLE_FUNC(cellMsgDialogOpen) {
    u32 type = (u32)ARG(0);
    LOG_I("message dialog (type %x): %s", type, gstr((u32)ARG(1)).c_str());
    u32 cb = (u32)ARG(2);
    if (cb && (type & 0x70) != 0) {   // has YES/NO or OK buttons
        std::lock_guard lk(g_sysutil_mutex);
        g_pending.push_back({cb, 1 /* YES/OK */, (u32)ARG(3), 0});
    }
    RET(CELL_OK);
}
HLE_FUNC(cellMsgDialogAbort) { RET(CELL_OK); }

extern std::string g_title_id;
// CellDiscGameSystemFileParam: titleId[10], reserved[2], parentalLevel, reserved[16]
HLE_FUNC(cellDiscGameGetBootDiscInfo) {
    u32 p = (u32)ARG(0);
    memset(gmem(p), 0, 32);
    gstr_write(p, g_title_id, 10);
    st32(p + 12, 3);
    RET(CELL_OK);
}
HLE_FUNC(cellDiscGameRegisterDiscChangeCallback) { RET(CELL_OK); }

// ================================================================== cellUsbd (no USB wheel attached)
HLE_FUNC(cellUsbdInit) { RET(CELL_OK); }
HLE_FUNC(cellUsbdRegisterExtraLdd) { RET(0); }
HLE_FUNC(cellUsbdUnregisterExtraLdd) { RET(CELL_OK); }
HLE_FUNC(cellUsbdScanStaticDescriptor) { RET(0); }
HLE_FUNC(cellUsbdOpenPipe) { RET((s32)0x80110003); }   // CELL_USBD_ERROR_NO_MEMORY-style failure
HLE_FUNC(cellUsbdClosePipe) { RET(CELL_OK); }

// ================================================================== cellAudio
namespace {
constexpr u32 AUDIO_PORTS = 8, BLOCK_SAMPLES = 256;
constexpr double BLOCK_US = BLOCK_SAMPLES * 1e6 / 48000;   // 5.33 ms
struct AudioPort {
    bool open = false, started = false;
    u32 nchannel = 2, nblock = 8, addr = 0, size = 0;
    float level = 1.0f;
    u64 tag = 0;
    bool silent_when_due[16] = {};   // per block: nothing written when the read index passed it
};
std::mutex g_audio_mutex;
std::array<AudioPort, AUDIO_PORTS> g_ports;
u32 g_read_index_addr = 0;   // u64[AUDIO_PORTS]
u32 g_tag_addr = 0;
bool g_audio_init = false;
std::thread g_audio_thread;

bool block_silent(u32 ea, u32 bytes) {
    const u32* w = (const u32*)gmem(ea);
    for (u32 i = 0; i < bytes / 4; i++)
        if (w[i]) return false;
    return true;
}

// F1CE_AUDIO_WAV=<file>: the output as a 48 kHz float stereo WAV (sizes in the header updated with each block)
void wav_write(const float* stereo, u32 frames) {
    static FILE* wav = [] {
        const char* e = getenv("F1CE_AUDIO_WAV");
        FILE* f = e ? fopen(e, "wb") : nullptr;
        if (f) {
            const u32 rate = 48000, byte_rate = rate * 8;
            const u16 fmt = 3 /* IEEE float */, ch = 2, align = 8, bits = 32;
            const u32 fmt_size = 16, zero = 0;
            fwrite("RIFF", 1, 4, f); fwrite(&zero, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt_size, 4, 1, f);
            fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
            fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&zero, 4, 1, f);
        }
        return f;
    }();
    if (!wav) return;
    static u32 bytes = 0;
    fwrite(stereo, sizeof(float), frames * 2, wav);
    bytes += frames * 8;
    const u32 riff = bytes + 36;
    fseek(wav, 4, SEEK_SET);
    fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET);
    fwrite(&bytes, 4, 1, wav);
    fseek(wav, 0, SEEK_END);
}

// The game mixes (Audio_Thread, fn 0xAA4E8, polling about every 1.3 ms) only one or two blocks ahead of the read index:
// the next block is due 5-11 ms after it is mixed. If the read index gets past its write index it stops mixing until the
// ring comes round (~35 ms of silence). So:
// - one block per period, never a burst to catch up after a late wake (a burst runs past the write index): the time is
//   dropped instead and the SDL queue covers it
// - the period follows the SDL queue (~30 ms), so the output runs on the device's clock: the queue neither runs dry nor
//   grows to its limit over a long session (each block dropped there was a 5 ms gap)
// - a block is copied F1CE_AUDIO_LAG (default 2) periods after the read index passed it, so a mix that comes late still
//   plays. Safe below nblock - 3: the game writes up to two blocks past the read index, a block 7 behind it the earliest.
void audio_loop() {
    set_thread_name("audio out");
    raise_thread_priority(true);
    static const u32 lag_env = getenv("F1CE_AUDIO_LAG") ? (u32)atoi(getenv("F1CE_AUDIO_LAG")) : 2;
    constexpr double TARGET_MS = 30;
    double next = (double)time_usec(), period = BLOCK_US, queued_avg = TARGET_MS;
    std::vector<float> mix(BLOCK_SAMPLES * 2);
    struct { u32 late_wakes = 0, rescued = 0, missed = 0, underruns = 0, dropped = 0; double max_late = 0; int qmin = 1 << 30, qmax = -1; } st;
    u64 window = time_usec(), minute = window;
    double rate_sum = 0;   // period corrections over the minute: the device's clock against time_usec
    u32 rate_n = 0;
    bool pushed = false;
    for (;;) {
        next += period;
        sleep_until_precise((u64)next);
        const u64 now = time_usec();
        if (now > next + BLOCK_US) {
            st.late_wakes++;
            st.max_late = std::max(st.max_late, (now - next) / 1000.0);
            next = (double)now;
        }
        std::fill(mix.begin(), mix.end(), 0.0f);
        {
            std::lock_guard lk(g_audio_mutex);
            for (u32 i = 0; i < AUDIO_PORTS; i++) {
                auto& p = g_ports[i];
                if (!p.open || !p.started) continue;
                const u32 nb = std::min<u32>(p.nblock, 16), bytes = p.nchannel * BLOCK_SAMPLES * 4;
                const u32 lag = nb > 3 ? std::min(lag_env, nb - 4) : 0;
                auto block_ea = [&](u32 b) { return p.addr + b % nb * bytes; };
                const u32 idx = (u32)ld64(g_read_index_addr + i * 8) % nb;
                // written after its copy (the copy of the last period): too late even with the lag
                if (lag) {
                    const u32 prev = (idx + 2 * nb - 1 - lag) % nb;
                    if (!block_silent(block_ea(prev), bytes)) { st.missed++; memset(gmem(block_ea(prev)), 0, bytes); }
                }
                p.silent_when_due[idx] = block_silent(block_ea(idx), bytes);
                const u32 copy = (idx + nb - lag) % nb, block = block_ea(copy);
                if (lag && p.silent_when_due[copy] && !block_silent(block, bytes)) st.rescued++;
                for (u32 s = 0; s < BLOCK_SAMPLES; s++) {
                    float l = ldf32(block + (s * p.nchannel) * 4);
                    float r = p.nchannel > 1 ? ldf32(block + (s * p.nchannel + 1) * 4) : l;
                    mix[s * 2] += l * p.level;
                    mix[s * 2 + 1] += r * p.level;
                }
                memset(gmem(block), 0, bytes);
                p.tag++;
                st64(g_tag_addr + (i * 16 + idx) * 8, p.tag);
                st64(g_read_index_addr + i * 8, (idx + 1) % p.nblock);
            }
        }
        wav_write(mix.data(), BLOCK_SAMPLES);
        const int q = host::audio_queued_ms();
        if (pushed && q == 0) st.underruns++;
        if (!host::audio_push(mix.data(), BLOCK_SAMPLES)) st.dropped++;
        pushed = true;
        if (q >= 0) {
            // (the device takes the queue in chunks of ~10 ms: averaged over ~0.3 s; at most 2% faster or slower)
            queued_avg += (q - queued_avg) * 0.02;
            period = BLOCK_US * (1 + std::clamp((queued_avg - TARGET_MS) / TARGET_MS * 0.02, -0.02, 0.02));
            st.qmin = std::min(st.qmin, q);
            st.qmax = std::max(st.qmax, q);
            rate_sum += period / BLOCK_US - 1;
            rate_n++;
        }
        if (now - window >= 10000000) {
            if (st.late_wakes || st.rescued || st.missed || st.underruns || st.dropped)
                LOG_I("audio out: 10 s: late wakes %u (max %.1f ms), late mixes %u played (lag), %u lost, queue %d..%d ms, "
                      "underruns %u, dropped %u", st.late_wakes, st.max_late, st.rescued, st.missed, st.qmin, st.qmax,
                      st.underruns, st.dropped);
            st = {};
            window = now;
        }
        // (the old fixed period drifted by this much: its queue filled to the 100 ms limit, then lost a block at intervals)
        if (now - minute >= 60000000 && rate_n) {
            LOG_I("audio out: device clock %+.0f ppm, queue %d ms", -rate_sum / rate_n * 1e6, q);
            rate_sum = 0;
            rate_n = 0;
            minute = now;
        }
    }
}
}  // namespace

// F1CE_AUDIO_CMD_LOG=1: commands the sound engine runs (fn 0x5BBB38 dispatch, r11 = command), per second, every 10 s:
// a command sent from per-frame code doubles with F1CE_60FPS
// F1CE_AUDIO_CMD_TRACE=<s>[,<len>]: from s seconds after start, len (8) s of the vector commands (67/68: emitter vectors at
// object +20 and +36, 75-78: the listener matrix rows) and of command 15 (object +24 = x, sent per frame) as CSV in
// audio_cmds.csv: time, command, object, x, y, z, w (args at r27 + 8)
void audio_cmd(PPUContext& c) {
    static const char* trace_env = getenv("F1CE_AUDIO_CMD_TRACE");
    if (trace_env) {
        static const u64 from = (u64)(atof(trace_env) * 1e6);
        static FILE* f = fopen("audio_cmds.csv", "w");
        const u64 t = time_usec();
        const u32 cmd = (u32)c.r[11];
        static const u64 len = strchr(trace_env, ',') ? (u64)(atof(strchr(trace_env, ',') + 1) * 1e6) : 8000000;
        if (f && t >= from && t < from + len && (cmd == 15 || cmd == 67 || cmd == 68 || (cmd >= 75 && cmd <= 78))) {
            const u32 a = (u32)c.r[27] + 8;
            fprintf(f, "%llu,%u,%08x,%08x,%08x,%08x,%08x,%08x\n", (unsigned long long)t, cmd, ld32((u32)c.r[26]), ld32(a - 4),
                    ld32(a), ld32(a + 4), ld32(a + 8), ld32(a + 12));
        }
    }
    static const bool on = getenv("F1CE_AUDIO_CMD_LOG") != nullptr;
    if (!on) return;
    static std::mutex m;
    static u64 counts[256] = {}, window = time_usec();
    std::lock_guard lk(m);
    counts[(u32)c.r[11] & 255]++;
    const u64 now = time_usec();
    if (now - window < 10000000) return;
    std::string line;
    for (u32 i = 0; i < 256; i++)
        if (counts[i]) {
            char b[32];
            snprintf(b, sizeof(b), " %u:%.1f", i, counts[i] * 1e6 / (now - window));
            line += b;
            counts[i] = 0;
        }
    LOG_I("audio cmds/s:%s", line.c_str());
    window = now;
}

// fn 0x5C8260, r3 = sound source (position at +0x14, velocity at +0x24, fixed point 4096 = 1): from the listener it
// computes volume, pan and the Doppler pitch, (0x136000 - relative radial speed) / 0x136, and passes them to each voice
// of the source (virtual +0xC). The game sends it once per frame per source, so with F1CE_60FPS the voices get twice as
// many pitch changes as on the PS3. F1CE_AUDIO3D_HZ=<n> (experiment): a source is updated at most n times per second
// (on an n Hz grid: 30 = the original rate). Same window as F1CE_AUDIO_CMD_TRACE: each update as CSV in audio_src.csv:
// time, source, x, y, z, vx, vy, vz.
bool audio_src_hook(PPUContext& c) {
    const u32 s = (u32)c.r[3];
    const u64 t = time_usec();
    static const int hz = getenv("F1CE_AUDIO3D_HZ") ? atoi(getenv("F1CE_AUDIO3D_HZ")) : 0;
    if (hz > 0) {
        static std::mutex m;
        static std::unordered_map<u32, u64> last_slot;
        const u64 slot = t * (u64)hz / 1000000;
        std::lock_guard lk(m);
        auto [it, fresh] = last_slot.try_emplace(s, slot);
        if (!fresh && it->second == slot) return true;   // already updated in this slot: the voices keep their values
        it->second = slot;
    }
    static const char* trace_env = getenv("F1CE_AUDIO_CMD_TRACE");
    if (!trace_env) return false;
    static const u64 from = (u64)(atof(trace_env) * 1e6);
    static FILE* f = fopen("audio_src.csv", "w");
    static const u64 len = strchr(trace_env, ',') ? (u64)(atof(strchr(trace_env, ',') + 1) * 1e6) : 8000000;
    if (!f || t < from || t >= from + len) return false;
    fprintf(f, "%llu,%08x,%d,%d,%d,%d,%d,%d\n", (unsigned long long)t, s, (s32)ld32(s + 0x14), (s32)ld32(s + 0x18),
            (s32)ld32(s + 0x1C), (s32)ld32(s + 0x24), (s32)ld32(s + 0x28), (s32)ld32(s + 0x2C));
    return false;
}

// fn 0x4C6280 (virtual), r3 = a car's engine sound: fills the grain engine input at +0x7C (+0x7C..+0x8C: load,
// +0x80, rev ratio +0x84, +0x88, +0x8C) and runs the grain engines at +0x74/+0x78 (fn 0x5CF28: rev and gear change
// filters, all stepped by the constant dt 1/180 at 0x10110804, so per call). Per-call filters here too (+0x60: 0.95/0.05,
// random load wobble 0.25/0.75). F1CE_AUDIO_CMD_LOG counts its calls; F1CE_ENGINE_SND_HZ=<n> (experiment): each car's
// engine sound is updated at most n times per second (n Hz grid, 30 = the PS3's rate). Same window as
// F1CE_AUDIO_CMD_TRACE: the input of each call (the values the previous call left) as CSV in engine_snd.csv: time, object,
// +0x60, +0x7C, +0x80, +0x84, +0x88, +0x8C.
bool engine_snd_hook(PPUContext& c) {
    const u32 s = (u32)c.r[3];
    const u64 t = time_usec();
    static const int hz = getenv("F1CE_ENGINE_SND_HZ") ? atoi(getenv("F1CE_ENGINE_SND_HZ")) : 0;
    if (hz > 0) {
        static std::mutex m;
        static std::unordered_map<u32, u64> last_slot;
        const u64 slot = t * (u64)hz / 1000000;
        std::lock_guard lk(m);
        auto [it, fresh] = last_slot.try_emplace(s, slot);
        if (!fresh && it->second == slot) {   // already updated in this slot: the grain engines keep their state
            c.r[3] = 0;
            return true;
        }
        it->second = slot;
    }
    static const bool log = getenv("F1CE_AUDIO_CMD_LOG") != nullptr;
    if (log) {
        static std::mutex m;
        static std::unordered_map<u32, u32> calls;
        static u64 window = t;
        std::lock_guard lk(m);
        calls[s]++;
        if (t - window >= 10000000) {
            std::string line;
            for (auto& [obj, n] : calls) {
                char b[32];
                snprintf(b, sizeof(b), " %08x:%.1f", obj, n * 1e6 / (t - window));
                line += b;
            }
            LOG_I("engine sound updates/s:%s", line.c_str());
            calls.clear();
            window = t;
        }
    }
    static const char* trace_env = getenv("F1CE_AUDIO_CMD_TRACE");
    if (!trace_env) return false;
    static const u64 from = (u64)(atof(trace_env) * 1e6);
    static FILE* f = fopen("engine_snd.csv", "w");
    static const u64 len = strchr(trace_env, ',') ? (u64)(atof(strchr(trace_env, ',') + 1) * 1e6) : 8000000;
    if (!f || t < from || t >= from + len) return false;
    fprintf(f, "%llu,%08x,%g,%g,%g,%g,%g,%g\n", (unsigned long long)t, s, ldf32(s + 0x60), ldf32(s + 0x7C),
            ldf32(s + 0x80), ldf32(s + 0x84), ldf32(s + 0x88), ldf32(s + 0x8C));
    return false;
}

HLE_FUNC(cellAudioInit) {
    std::lock_guard lk(g_audio_mutex);
    if (g_audio_init) RET((s32)0x80310701);   // CELL_AUDIO_ERROR_ALREADY_INIT
    g_read_index_addr = mem::alloc(AUDIO_PORTS * 8, 128);
    g_tag_addr = mem::alloc(AUDIO_PORTS * 16 * 8, 128);
    g_audio_init = true;
    g_audio_thread = std::thread(audio_loop);
    g_audio_thread.detach();
    RET(CELL_OK);
}
HLE_FUNC(cellAudioQuit) { RET(CELL_OK); }
// cellAudioPortOpen(CellAudioPortParam* {u64 nChannel, nBlock, attr; float level}, u32* portNum)
HLE_FUNC(cellAudioPortOpen) {
    u32 prm = (u32)ARG(0);
    std::lock_guard lk(g_audio_mutex);
    for (u32 i = 0; i < AUDIO_PORTS; i++) {
        auto& p = g_ports[i];
        if (p.open) continue;
        p.open = true;
        p.started = false;
        p.nchannel = (u32)ld64(prm);
        p.nblock = (u32)ld64(prm + 8);
        u64 attr = ld64(prm + 16);
        p.level = (attr & 0x1000) ? ldf32(prm + 24) : 1.0f;   // CELL_AUDIO_PORTATTR_INITLEVEL
        p.size = p.nchannel * p.nblock * BLOCK_SAMPLES * 4;
        p.addr = mem::alloc(p.size, 128);
        st64(g_read_index_addr + i * 8, 0);
        st32((u32)ARG(1), i);
        LOG_I("audio port %u opened: %u ch, %u blocks @ %08x", i, p.nchannel, p.nblock, p.addr);
        RET(CELL_OK);
    }
    RET((s32)0x80310705);   // CELL_AUDIO_ERROR_PORT_FULL
}
// CellAudioPortConfig: readIndexAddr, status, nChannel(u64), nBlock(u64), portSize, portAddr
HLE_FUNC(cellAudioGetPortConfig) {
    u32 i = (u32)ARG(0), p = (u32)ARG(1);
    if (i >= AUDIO_PORTS) RET(CELL_EINVAL);
    std::lock_guard lk(g_audio_mutex);
    auto& port = g_ports[i];
    st32(p, g_read_index_addr + i * 8);
    st32(p + 4, port.open ? (port.started ? 2 : 1) : 0);
    st64(p + 8, port.nchannel);
    st64(p + 16, port.nblock);
    st32(p + 24, port.size);
    st32(p + 28, port.addr);
    RET(CELL_OK);
}
HLE_FUNC(cellAudioPortStart) {
    std::lock_guard lk(g_audio_mutex);
    g_ports[(u32)ARG(0) % AUDIO_PORTS].started = true;
    RET(CELL_OK);
}
HLE_FUNC(cellAudioPortStop) {
    std::lock_guard lk(g_audio_mutex);
    g_ports[(u32)ARG(0) % AUDIO_PORTS].started = false;
    RET(CELL_OK);
}
HLE_FUNC(cellAudioPortClose) {
    std::lock_guard lk(g_audio_mutex);
    g_ports[(u32)ARG(0) % AUDIO_PORTS].open = false;
    RET(CELL_OK);
}
// cellAudioGetPortBlockTag(port, blockNo, u64* tag)
HLE_FUNC(cellAudioGetPortBlockTag) {
    u32 i = (u32)ARG(0), b = (u32)ARG(1);
    st64((u32)ARG(2), ld64(g_tag_addr + (i * 16 + b) * 8));
    RET(CELL_OK);
}

