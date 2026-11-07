// HLE: sys_io (pad/keyboard/mouse), sys_fs (cellFs*), cellSysutil, cellAudio, cellUsbd.
#include "host.h"
#include "kernel.h"
#include "lv2_fs.h"
#include "rsx.h"

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
struct AudioPort {
    bool open = false, started = false;
    u32 nchannel = 2, nblock = 8, addr = 0, size = 0;
    float level = 1.0f;
    u64 tag = 0;
};
std::mutex g_audio_mutex;
std::array<AudioPort, AUDIO_PORTS> g_ports;
u32 g_read_index_addr = 0;   // u64[AUDIO_PORTS]
u32 g_tag_addr = 0;
bool g_audio_init = false;
std::thread g_audio_thread;

void audio_loop() {
    set_thread_name("audio out");
    u64 next = time_usec();
    std::vector<float> mix(BLOCK_SAMPLES * 2);
    for (;;) {
        next += BLOCK_SAMPLES * 1000000ull / 48000;
        u64 now = time_usec();
        if (next > now) sleep_usec(next - now);
        std::fill(mix.begin(), mix.end(), 0.0f);
        std::lock_guard lk(g_audio_mutex);
        for (u32 i = 0; i < AUDIO_PORTS; i++) {
            auto& p = g_ports[i];
            if (!p.open || !p.started) continue;
            u32 idx = (u32)ld64(g_read_index_addr + i * 8);
            u32 block = p.addr + idx * p.nchannel * BLOCK_SAMPLES * 4;
            for (u32 s = 0; s < BLOCK_SAMPLES; s++) {
                float l = ldf32(block + (s * p.nchannel) * 4);
                float r = p.nchannel > 1 ? ldf32(block + (s * p.nchannel + 1) * 4) : l;
                mix[s * 2] += l * p.level;
                mix[s * 2 + 1] += r * p.level;
            }
            memset(gmem(block), 0, p.nchannel * BLOCK_SAMPLES * 4);
            p.tag++;
            st64(g_tag_addr + (i * 16 + idx) * 8, p.tag);
            st64(g_read_index_addr + i * 8, (idx + 1) % p.nblock);
        }
        host::audio_push(mix.data(), BLOCK_SAMPLES);
    }
}
}  // namespace

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

