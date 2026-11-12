// SDL3 host backend: OpenGL window, keyboard/gamepad input, audio output.
//
// The guest's main thread runs on the process main thread, so the window and its event loop live
// on a dedicated UI thread. The RSX thread owns the OpenGL context (created for that window).
#include "host.h"
#include "kernel.h"
#include "rsx.h"
#include "input_config.h"
#include "wheel_config.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>
#include <vector>
void set_thread_name(const char* name);   // core.cpp

namespace host {
void wheel_poll();                      // wheel_sdl.cpp (UI thread)
void wheel_close();
bool wheel_owns(SDL_JoystickID id);
namespace {
std::mutex g_pad_mutex;
PadState g_pad;
std::atomic<u32> g_rumble{0};   // low << 16 | high, 0..0xFFFF each (pad_rumble), applied on the UI thread
std::atomic<u32> g_debug_keys{0};   // DK_* bits (debug_keys), sampled with the pad
std::atomic<bool> g_quit{false};

std::mutex g_win_mutex;
std::condition_variable g_win_cv;
SDL_Window* g_window = nullptr;
bool g_window_failed = false;
SDL_GLContext g_gl = nullptr;

SDL_AudioStream* g_audio = nullptr;

// CELL_PAD_CTRL_* bit layout
enum : u16 {
    D1_LEFT = 0x80, D1_DOWN = 0x40, D1_RIGHT = 0x20, D1_UP = 0x10, D1_START = 0x08, D1_R3 = 0x04, D1_L3 = 0x02,
    D1_SELECT = 0x01,
    D2_SQUARE = 0x80, D2_CROSS = 0x40, D2_CIRCLE = 0x20, D2_TRIANGLE = 0x10, D2_R1 = 0x08, D2_L1 = 0x04,
    D2_R2 = 0x02, D2_L2 = 0x01,
};

u16 axis8(float v) { return (u16)std::clamp((int)std::floor(128 + v * 128), 0, 255); }   // -1..1: 0..255, 128 centred

// the keyboard and gamepad bindings ([keyboard], [gamepad] of the launcher's ini: runtime/input_config.h)
inputcfg::Config g_in;
void load_input() {
    const char* path = getenv("F1CE_WHEEL_INI") ? getenv("F1CE_WHEEL_INI") : "f1ce_launcher.ini";
    const wheelcfg::Section kb = wheelcfg::read_section(path, "keyboard"), gp = wheelcfg::read_section(path, "gamepad");
    inputcfg::load(g_in, [&](const char* s, const char* k) -> const char* {
        const wheelcfg::Section& m = !strcmp(s, "keyboard") ? kb : gp;
        auto it = m.find(k);
        return it == m.end() ? nullptr : it->second.c_str();
    });
}

void update_pad(SDL_Gamepad* gp) {
    using namespace inputcfg;
    const bool* k = SDL_GetKeyboardState(nullptr);
    // each control's value, 0..1: its keys, or its gamepad input (a button, or a stick / trigger half)
    float key[CONTROLS] = {}, pad[PAD_CONTROLS] = {};
    for (int c = 0; c < CONTROLS; c++)
        for (int sc : g_in.keys[c])
            if (sc > 0 && sc < SDL_SCANCODE_COUNT && k[sc]) key[c] = 1;
    if (gp)
        for (int c = 0; c < PAD_CONTROLS; c++) {
            const int in = g_in.pad[c];
            if (in == NONE) continue;
            if (!is_axis(in)) {
                pad[c] = SDL_GetGamepadButton(gp, (SDL_GamepadButton)(in - 1)) ? 1.0f : 0.0f;
                continue;
            }
            const int a = (in - AXIS) / 2;
            const float v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a) / 32767.0f;
            const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            pad[c] = std::clamp(trigger ? v : (in - AXIS) % 2 ? v : -v, 0.0f, 1.0f);
        }
    PadState p;
    // digital buttons and their pressure (CELL_PAD_CTRL_* order of the pressure array)
    static const struct { int c, word; u16 bit; int idx; } buttons[] = {
        {UP, 1, D1_UP, 2}, {DOWN, 1, D1_DOWN, 3}, {LEFT, 1, D1_LEFT, 1}, {RIGHT, 1, D1_RIGHT, 0},
        {CROSS, 2, D2_CROSS, 6}, {CIRCLE, 2, D2_CIRCLE, 5}, {SQUARE, 2, D2_SQUARE, 7}, {TRIANGLE, 2, D2_TRIANGLE, 4},
        {L1, 2, D2_L1, 8}, {R1, 2, D2_R1, 9}, {L2, 2, D2_L2, 10}, {R2, 2, D2_R2, 11},
        {L3, 1, D1_L3, -1}, {R3, 1, D1_R3, -1}, {START, 1, D1_START, -1}, {SELECT, 1, D1_SELECT, -1},
    };
    const float threshold = g_in.trigger / 100.0f;
    for (const auto& b : buttons) {
        const bool analog = gp && is_axis(g_in.pad[b.c]);
        const float v = std::max(key[b.c], pad[b.c]);
        const bool on = key[b.c] > 0 || (analog ? pad[b.c] > threshold : pad[b.c] > 0.5f);
        if (on) (b.word == 1 ? p.digital1 : p.digital2) |= b.bit;
        if (b.idx >= 0) p.pressure[b.idx] = on ? (u16)std::clamp((int)std::lround(v * 255), 1, 255) : 0;
    }
    // sticks: the gamepad's (dead zone; steering with the response curve), else the keys (steering ramped)
    auto stick = [&](int neg, int pos, int curve) {
        return inputcfg::shape(pad[pos] - pad[neg], g_in.deadzone, curve);
    };
    float lx = stick(LS_LEFT, LS_RIGHT, g_in.curve), ly = stick(LS_UP, LS_DOWN, 100);
    float rx = stick(RS_LEFT, RS_RIGHT, 100), ry = stick(RS_UP, RS_DOWN, 100);
    {   // keyboard steering: towards full lock in steer_ms, back to the centre in centre_ms
        static float kb_steer = 0;
        static Uint64 last = SDL_GetTicks();
        const Uint64 now = SDL_GetTicks();
        const float dt = (now - last) / 1000.0f, target = key[LS_RIGHT] - key[LS_LEFT];
        last = now;
        const int ms = target != 0 && (kb_steer == 0 || (target > 0) == (kb_steer > 0)) ? g_in.steer_ms : g_in.centre_ms;
        const float step = ms > 0 ? dt * 1000.0f / ms : 2.0f;
        kb_steer = kb_steer < target ? std::min(kb_steer + step, target) : std::max(kb_steer - step, target);
        if (lx == 0) lx = kb_steer;
    }
    if (ly == 0) ly = key[LS_DOWN] - key[LS_UP];
    if (rx == 0) rx = key[RS_RIGHT] - key[RS_LEFT];
    if (ry == 0) ry = key[RS_DOWN] - key[RS_UP];
    if (g_in.invert_look) ry = -ry;
    p.lx = axis8(lx);
    p.ly = axis8(ly);
    p.rx = axis8(rx);
    p.ry = axis8(ry);
    // TV camera tuning keys (hle_camera.cpp) and F8 (hle_aidrive.cpp): host::debug_keys
    u32 dk = 0;
    for (int c = TUNE_UP; c <= AI_TOGGLE; c++)
        if (key[c] > 0) dk |= 1u << (c - TUNE_UP);
    g_debug_keys = dk;
    std::lock_guard lk(g_pad_mutex);
    g_pad = p;
}

void ui_thread() {
    set_thread_name("ui");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // F1CE_FULLSCREEN=1 starts in (desktop) full screen; F11 or Alt+Enter toggles it at any time
    const bool full = getenv("F1CE_FULLSCREEN") && atoi(getenv("F1CE_FULLSCREEN")) != 0;
    int win_w = 1280, win_h = 720;   // F1CE_WINDOW=<w>x<h>: initial window size
    if (const char* e = getenv("F1CE_WINDOW")) {
        int a = 0, b = 0;
        if (sscanf(e, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 180) { win_w = a; win_h = b; }
    }
    SDL_Window* win = SDL_CreateWindow("Formula One: Championship Edition (recompiled)", win_w, win_h,
                                       (rsx::use_vulkan() ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE |
                                           (full ? SDL_WINDOW_FULLSCREEN : 0));
    {
        std::lock_guard lk(g_win_mutex);
        g_window = win;
        g_window_failed = !win;
    }
    g_win_cv.notify_all();
    if (!win) { LOG_E("SDL window creation failed: %s", SDL_GetError()); return; }
    load_input();
    SDL_Gamepad* gp = nullptr;
    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) g_quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) == 0);
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !gp && !wheel_owns(e.gdevice.which)) gp = SDL_OpenGamepad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED && gp && SDL_GetGamepadID(gp) == e.gdevice.which) {
                SDL_CloseGamepad(gp);
                gp = nullptr;
            }
        }
        wheel_poll();
        if (gp && wheel_owns(SDL_GetGamepadID(gp))) {   // a wheel SDL also maps as a gamepad: it is the G25 only
            SDL_CloseGamepad(gp);
            gp = nullptr;
        }
        update_pad(gp);
        if (gp) {   // refreshed every 100 ms with a 200 ms duration, so the motors stop if the game stops updating them
            static u32 sent = 0;
            static u64 sent_at = 0;
            const u32 r = g_rumble.load();
            const u64 now = time_usec();
            if (r != sent || (r && now - sent_at > 100000)) {
                SDL_RumbleGamepad(gp, (Uint16)(r >> 16), (Uint16)r, r ? 200 : 0);
                sent = r;
                sent_at = now;
            }
        }
        SDL_Delay(4);
    }
    wheel_close();   // (no force left playing on the wheel)
}
}  // namespace

void init() {
    // (Linux: the app id names the window class / Wayland app id, matched by the launcher's .desktop file, and the
    // PulseAudio/PipeWire stream)
    SDL_SetAppMetadata("Formula One: Championship Edition (recompiled)", nullptr, "f1ce-recomp");
    // (wheels: the input of a second monitor's game window keeps working while another window has the focus)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
        LOG_E("SDL_Init failed: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    else LOG_W("SDL audio unavailable: %s", SDL_GetError());
    std::thread(ui_thread).detach();
    LOG_I("host: SDL3 backend (keyboard: arrows, Z=cross X=circle A=square S=triangle, Enter=start, F11=full screen)");
}

// Scripted input for automated tests: F1CE_INPUT="12:cross,16.5:down,20:start" presses each button
// for 200 ms at the given time (seconds since start); "60:r2:120" holds R2 for 120 s; "f8" = the F8 key (debug_keys).
static std::atomic<u64> input_clock_start{0};
void input_clock_reset() { input_clock_start = time_usec(); }
double input_time() { return (time_usec() - input_clock_start.load()) / 1e6; }

namespace {
struct ScriptedPress { double t; int word; u16 bit; double dur; };
std::vector<ScriptedPress> parse_script() {
    std::vector<ScriptedPress> v;
    const char* env = getenv("F1CE_INPUT");
    if (!env) return v;
    static const struct { const char* n; int word; u16 bit; } names[] = {
        {"left", 1, D1_LEFT}, {"right", 1, D1_RIGHT}, {"up", 1, D1_UP}, {"down", 1, D1_DOWN}, {"start", 1, D1_START},
        {"select", 1, D1_SELECT}, {"cross", 2, D2_CROSS}, {"circle", 2, D2_CIRCLE}, {"square", 2, D2_SQUARE},
        {"triangle", 2, D2_TRIANGLE}, {"l1", 2, D2_L1}, {"r1", 2, D2_R1}, {"l2", 2, D2_L2}, {"r2", 2, D2_R2},
        {"f8", 0, (u16)DK_AI_TOGGLE},   // word 0: a debug key
    };
    std::string s = env;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            double t = atof(item.substr(0, colon).c_str());
            std::string b = item.substr(colon + 1);
            double dur = 0.2;
            if (size_t c2 = b.find(':'); c2 != std::string::npos) { dur = atof(b.substr(c2 + 1).c_str()); b = b.substr(0, c2); }
            for (auto& n : names)
                if (b == n.n) v.push_back({t, n.word, n.bit, dur});
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    LOG_I("host: %zu scripted button presses", v.size());
    return v;
}
const std::vector<ScriptedPress>& script() {
    static const std::vector<ScriptedPress> s = parse_script();
    return s;
}
}  // namespace

u32 debug_keys() {
    u32 dk = g_debug_keys;
    const double now = (time_usec() - input_clock_start.load()) / 1e6;
    for (auto& e : script())
        if (e.word == 0 && now >= e.t && now < e.t + e.dur) dk |= e.bit;
    return dk;
}

void pad_rumble(float low, float high) {
    auto q = [](float v) { return (u32)std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    g_rumble = q(low) << 16 | q(high);
}

PadState pad_state() {
    PadState p;
    {
        std::lock_guard lk(g_pad_mutex);
        p = g_pad;
    }
    // (scripted times count from the end of the start-up shader phase, whose length varies)
    double now = (time_usec() - input_clock_start.load()) / 1e6;
    {   // F1CE_INPUT_RECORD=<file>: writes the player's presses in F1CE_INPUT syntax ("t:button:duration,") for replay
        static FILE* rec = getenv("F1CE_INPUT_RECORD") ? fopen(getenv("F1CE_INPUT_RECORD"), "w") : nullptr;
        if (rec) {
            static const struct { const char* n; int word; u16 bit; } names[] = {
                {"left", 1, D1_LEFT}, {"right", 1, D1_RIGHT}, {"up", 1, D1_UP}, {"down", 1, D1_DOWN}, {"start", 1, D1_START},
                {"select", 1, D1_SELECT}, {"cross", 2, D2_CROSS}, {"circle", 2, D2_CIRCLE}, {"square", 2, D2_SQUARE},
                {"triangle", 2, D2_TRIANGLE}, {"l1", 2, D2_L1}, {"r1", 2, D2_R1}, {"l2", 2, D2_L2}, {"r2", 2, D2_R2},
            };
            static double down_at[14] = {};
            static std::mutex m;
            std::lock_guard lk(m);
            for (int i = 0; i < 14; i++) {
                bool on = ((names[i].word == 1 ? p.digital1 : p.digital2) & names[i].bit) != 0;
                if (on && down_at[i] == 0) down_at[i] = now;
                else if (!on && down_at[i] != 0) {
                    fprintf(rec, "%.2f:%s:%.2f,", down_at[i], names[i].n, std::max(0.1, now - down_at[i]));
                    fflush(rec);
                    LOG_I("input record: %.2f %s (%.2f s)", down_at[i], names[i].n, now - down_at[i]);
                    down_at[i] = 0;
                }
            }
        }
    }
    {   // debug: F1CE_MEM_DUMP=<s>,<s>,.. saves the data segment and user memory (mem_<s>.bin: 16 MB at 10000000, then
        // 192 MB at 30000000) at those input times, to find game variables by comparing snapshots
        static std::vector<double> at = [] {
            std::vector<double> v;
            if (const char* e = getenv("F1CE_MEM_DUMP"))
                for (const char* q = e; *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) v.push_back(atof(q));
            return v;
        }();
        static size_t next = 0;
        static std::mutex m;
        std::lock_guard lk(m);
        if (next < at.size() && now >= at[next]) {
            // (copied here, written by a thread: a slow write would hold up input polling and lose scripted presses)
            char nb[48];
            snprintf(nb, sizeof(nb), "mem_%g.bin", at[next]);
            std::string name = nb;
            auto* img = new std::vector<u8>(0x0D000000);
            memcpy(img->data(), gmem(0x10000000), 0x01000000);
            memcpy(img->data() + 0x01000000, gmem(mem::USER_BASE), 0x0C000000);
            std::thread([img, name] {
                if (FILE* f = fopen(name.c_str(), "wb")) { fwrite(img->data(), 1, img->size(), f); fclose(f); }
                delete img;
            }).detach();
            LOG_I("memory dump %s", name.c_str());
            next++;
        }
    }
    for (auto& e : script())
        if (e.word != 0 && now >= e.t && now < e.t + e.dur) {
            if (e.word == 1) p.digital1 |= e.bit;
            else p.digital2 |= e.bit;
            // full pressure too: the game reads analog values (the throttle is the pressure of cross)
            static const struct { int word; u16 bit; int idx; } press[] = {
                {1, D1_RIGHT, 0}, {1, D1_LEFT, 1}, {1, D1_UP, 2}, {1, D1_DOWN, 3}, {2, D2_TRIANGLE, 4}, {2, D2_CIRCLE, 5},
                {2, D2_CROSS, 6}, {2, D2_SQUARE, 7}, {2, D2_L1, 8}, {2, D2_R1, 9}, {2, D2_L2, 10}, {2, D2_R2, 11},
            };
            for (auto& q : press)
                if (q.word == e.word && q.bit == e.bit) p.pressure[q.idx] = 255;
        }
    return p;
}

bool quit_requested() { return g_quit; }
void request_quit() { g_quit = true; }

bool audio_push(const float* stereo, u32 frames) {
    if (!g_audio) return true;
    // (cellAudio paces itself on audio_queued_ms; this only bounds the latency if that fails: more than 250 ms queued)
    if (SDL_GetAudioStreamQueued(g_audio) > 48000 * 8 / 4) return false;
    SDL_PutAudioStreamData(g_audio, stereo, (int)(frames * 2 * sizeof(float)));
    return true;
}

int audio_queued_ms() {
    if (!g_audio) return -1;
    return SDL_GetAudioStreamQueued(g_audio) / (48 * 8);   // (bytes: 48 frames of 8 bytes per ms)
}

static u32 g_dump_count = 0;
bool dump_frame_due() {
    // debug aid: F1CE_DUMP_FRAMES=N dumps every N-th frame as raw ARGB (tools/frames_to_png.py)
    static const char* env = getenv("F1CE_DUMP_FRAMES");
    // F1CE_DUMP_FROM / F1CE_DUMP_COUNT limit the dump to a window of frames (e.g. to catch flicker)
    static const u32 from = getenv("F1CE_DUMP_FROM") ? (u32)atoi(getenv("F1CE_DUMP_FROM")) : 0;
    static const u32 limit = getenv("F1CE_DUMP_COUNT") ? (u32)atoi(getenv("F1CE_DUMP_COUNT")) : ~0u;
    if (!env || atoi(env) <= 0 || ++g_dump_count % (u32)atoi(env) != 0) return false;
    return g_dump_count >= from && g_dump_count - from < limit;
}

void dump_frame(const u32* argb, u32 width, u32 height) {
    const u32 count = g_dump_count;
    char name[64];
    snprintf(name, sizeof(name), "frame_%05u_%ux%u.argb", count, width, height);
    if (FILE* f = fopen(name, "wb")) {
        fwrite(argb, 4, (size_t)width * height, f);
        fclose(f);
    }
    LOG_I("frame dump %u at input time %.2f s", count, (time_usec() - input_clock_start.load()) / 1e6);
}

void present(u32 ea, u32 pitch, u32 width, u32 height) {
    // CPU path (used only without the GPU renderer): just make the frame available for dumping
    if (!width || !height || width > 4096 || height > 4096) return;
    std::vector<u32> img((size_t)width * height);
    for (u32 y = 0; y < height; y++)
        for (u32 x = 0; x < width; x++) img[(size_t)y * width + x] = ld32(ea + y * pitch + x * 4) | 0xFF000000u;
    if (dump_frame_due()) dump_frame(img.data(), width, height);
}

bool gl_begin() {
    std::unique_lock lk(g_win_mutex);
    g_win_cv.wait(lk, [] { return g_window || g_window_failed; });
    if (!g_window) return false;
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) { LOG_E("OpenGL 3.3 context creation failed: %s", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(g_window, g_gl);
    // F1CE_VSYNC: 1 = wait for the display's vblank (no tearing; the game still paces itself on the guest vblank),
    // 0 = off, -1 = adaptive (late frames tear instead of waiting a whole refresh)
    const char* vs = getenv("F1CE_VSYNC");
    int interval = vs ? atoi(vs) : 1;
    if (!SDL_GL_SetSwapInterval(interval)) {
        LOG_W("swap interval %d: %s", interval, SDL_GetError());
        if (interval < 0 && !SDL_GL_SetSwapInterval(1)) LOG_W("swap interval 1: %s", SDL_GetError());
    }
    int got = 0;
    SDL_GL_GetSwapInterval(&got);
    LOG_I("vsync: swap interval %d", got);
    return true;
}
void* gl_proc(const char* name) { return (void*)SDL_GL_GetProcAddress(name); }
void* gl_create_shared_context(bool share) {
    if (!g_window || !g_gl) return nullptr;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, share ? 1 : 0);
    SDL_GLContext ctx = SDL_GL_CreateContext(g_window);   // becomes current: switch back
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(g_window, g_gl);
    if (!ctx) LOG_W("shared GL context: %s", SDL_GetError());
    return ctx;
}
bool gl_make_current(void* ctx) { return SDL_GL_MakeCurrent(g_window, (SDL_GLContext)ctx); }
void gl_swap() { if (g_window) SDL_GL_SwapWindow(g_window); }
void gl_drawable_size(int* w, int* h) { SDL_GetWindowSizeInPixels(g_window, w, h); }

void* vk_begin() {
    std::unique_lock lk(g_win_mutex);
    g_win_cv.wait(lk, [] { return g_window || g_window_failed; });
    if (!g_window) return nullptr;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) { LOG_E("Vulkan library: %s", SDL_GetError()); return nullptr; }
    return (void*)SDL_Vulkan_GetVkGetInstanceProcAddr();
}
const char* const* vk_instance_extensions(u32* count) { return SDL_Vulkan_GetInstanceExtensions(count); }
void* vk_create_surface(void* instance) {
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(g_window, (VkInstance)instance, nullptr, &surface)) {
        LOG_E("Vulkan surface: %s", SDL_GetError());
        return nullptr;
    }
    return (void*)surface;
}
}  // namespace host
