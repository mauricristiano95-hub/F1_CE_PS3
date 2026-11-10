// Headless host backend: no window, no audio, neutral pad. Used for bring-up and CI.
#include "host.h"
#include "kernel.h"

namespace host {
void init() { LOG_I("host: headless backend"); }
PadState pad_state() { return {}; }
u32 debug_keys() { return 0; }
void pad_rumble(float, float) {}
void input_clock_reset() {}
bool quit_requested() { return false; }
void request_quit() {}
bool audio_push(const float*, u32) { return true; }
int audio_queued_ms() { return -1; }
void present(u32 ea, u32 pitch, u32 width, u32 height) {
    static u32 frames = 0;
    if (++frames % 60 == 1) LOG_I("present #%u: fb %08x %ux%u pitch %u", frames, ea, width, height, pitch);
}
bool gl_begin() { return false; }
void* gl_proc(const char*) { return nullptr; }
void gl_swap() {}
void* gl_create_shared_context(bool) { return nullptr; }
bool gl_make_current(void*) { return false; }
void gl_drawable_size(int* w, int* h) { *w = *h = 0; }
bool dump_frame_due() { return false; }
void dump_frame(const u32*, u32, u32) {}
}  // namespace host
