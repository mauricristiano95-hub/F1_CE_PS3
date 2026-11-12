// Host platform layer: window/presentation, input and audio output.
#pragma once
#include "ppu_runtime.h"

namespace host {
struct PadState {
    u16 digital1 = 0, digital2 = 0;          // CELL_PAD_CTRL_* bit layout
    u16 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    u16 pressure[12] = {};
};

void init();
PadState pad_state();
// debug / tuning keys held right now (numpad: hle_camera.cpp's TV camera tuning; F8: hle_aidrive.cpp), DK_* bits
enum DebugKey : u32 { DK_UP = 1, DK_DOWN = 2, DK_LEFT = 4, DK_RIGHT = 8, DK_BACK = 16, DK_FWD = 32, DK_PITCH_UP = 64,
                      DK_PITCH_DOWN = 128, DK_FOV_UP = 256, DK_FOV_DOWN = 512, DK_RESET = 1024, DK_PRINT = 2048,
                      DK_AI_TOGGLE = 4096 };
u32 debug_keys();
void pad_rumble(float low, float high);   // motor strengths 0..1 (low = heavy motor), held until the next call

// Steering wheel (wheel_sdl.cpp): a wheel seen by SDL, set up by the [wheel] section of f1ce_launcher.ini (or the file
// named by F1CE_WHEEL_INI). The game sees it as a Logitech G25 (hle_usbd.cpp).
enum WheelButton : u32 {   // logical buttons, the G25's (WBTN_* bit i = the [wheel] key btn_<WHEEL_BUTTON_KEYS[i]>)
    WBTN_CROSS = 1, WBTN_SQUARE = 2, WBTN_CIRCLE = 4, WBTN_TRIANGLE = 8, WBTN_SHIFT_UP = 16, WBTN_SHIFT_DOWN = 32, WBTN_R2 = 64,
    WBTN_L2 = 128, WBTN_SELECT = 256, WBTN_START = 512, WBTN_R3 = 1024, WBTN_L3 = 2048, WBTN_UP = 4096, WBTN_DOWN = 8192,
    WBTN_LEFT = 16384, WBTN_RIGHT = 32768,
};
constexpr int WHEEL_BUTTONS = 16;
extern const char* const WHEEL_BUTTON_KEYS[WHEEL_BUTTONS];
struct WheelInput {
    bool connected = false;
    float steer = 0;                             // -1 .. 1: the game's steering lock (beyond it: clamped)
    float throttle = 0, brake = 0, clutch = 0;   // 0 released .. 1 fully pressed
    u32 buttons = 0;                             // WBTN_* (the hat included as WBTN_UP..WBTN_RIGHT)
    s8 gear = 0;                                 // H-shifter: 0 neutral, 1..6, -1 reverse
    bool hshifter = false;                       // gear buttons set up (else the paddles shift)
};
WheelInput wheel_input();
// the game's force feedback, as the wheel's own effects; positions and levels in the game's steering units (-1 .. 1,
// positive to the right)
struct WheelForces {
    float constant = 0;                                        // pushes towards +1 when positive
    bool spring = false;                                       // pulls towards spring_center, outside the dead band
    bool autocenter = false;                                   // the spring is the wheel's default centring spring
    float spring_center = 0, spring_deadband = 0, spring_left = 0, spring_right = 0, spring_sat = 1;
    bool damper = false;                                       // resists speed
    float damper_left = 0, damper_right = 0, damper_sat = 1;
    bool friction = false;
    float friction_coef = 0, friction_sat = 1;
};
void wheel_forces(const WheelForces& f);
void wheel_lock_deg(int degrees);   // a steering lock the game chose (Logitech range command); 0: none
bool wheel_ffb();                   // a wheel with force feedback is in use (and the settings want it)
void input_clock_reset();   // scripted input times count from this call (end of the start-up shader phase)
double input_time();         // seconds since then
bool quit_requested();
void request_quit();   // as closing the window (Alt+F4): the game gets CELL_SYSUTIL_REQUEST_EXITGAME and shuts down
bool audio_push(const float* stereo, u32 frames);   // 48 kHz interleaved stereo; false: dropped (queue full)
int audio_queued_ms();   // output not yet played, -1 without an audio device
// Presents a guest framebuffer (A8R8G8B8 big-endian in guest memory). CPU path (no GPU renderer).
void present(u32 ea, u32 pitch, u32 width, u32 height);

// GPU path: makes an OpenGL 3.3 core context current on the calling thread (waits for the
// window). Returns false if no GL is available (headless).
bool gl_begin();
void* gl_proc(const char* name);
void gl_swap();
// A second context sharing objects with the one made current by gl_begin (call on that thread); a worker thread
// then makes it current with gl_make_current. Returns null if unavailable.
void* gl_create_shared_context(bool share = true);   // share = false: a separate share group
bool gl_make_current(void* ctx);
void gl_drawable_size(int* w, int* h);
// Vulkan path: waits for the window and loads the Vulkan library. Returns vkGetInstanceProcAddr, or null if unavailable.
void* vk_begin();
const char* const* vk_instance_extensions(u32* count);   // what the window system needs
void* vk_create_surface(void* instance);                  // VkSurfaceKHR of the window, null on failure
// Saves a frame (host ARGB, top-down) for F1CE_DUMP_FRAMES debugging; dump_frame_due() counts a frame and says whether
// to dump it (call it once per frame, then dump_frame only when it is true).
bool dump_frame_due();
void dump_frame(const u32* argb, u32 width, u32 height);
}  // namespace host
