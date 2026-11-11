// Output format (F1CE_SCALE). 480p and 576p are the PS3 video modes: the game finds them in cellVideoOutGetState and lays
// out its picture and interface for them. Any other format keeps the game on 720p with the render targets scaled:
// in a 3D scene the cameras take the format's aspect ratio (below) and the interface stays 16:9 in the middle of the
// image; everything else (menus, loading screens) is the 16:9 picture between black bars.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel.h"
#include "rsx.h"

void sub_003FB310(PPUContext& __restrict c);

namespace rsx {
namespace {
struct Format { float x = 1, y = 1; u8 lines = 0; bool narrow = false; };
const Format& format() {
    static const Format v = [] {
        Format f;
        const char* e = getenv("F1CE_SCALE");
        int a = 0, b = 0;
        if (!e) return f;
        if (strchr(e, 'p')) {   // 480p, 576p, 480p:4x3, 576p:4x3
            f.lines = atoi(e) == 576 ? 2 : 1;
            f.narrow = strstr(e, ":4x3") != nullptr;
        } else if (sscanf(e, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) {
            f.x = std::clamp(a / 1280.0f, 0.5f, 4.0f);
            f.y = std::clamp(b / 720.0f, 0.5f, 4.0f);
        } else {
            f.x = f.y = std::clamp((float)atof(e), 1.0f, 4.0f);
        }
        return f;
    }();
    return v;
}
u32 g_display[8];   // offsets the game has flipped
}  // namespace

float res_scale_x() { static const float v = format().x; return v; }
float res_scale_y() { static const float v = format().y; return v; }
bool res_scaled() { return res_scale_x() != 1 || res_scale_y() != 1; }
// The shadow maps (the game's 1024x2048 depth targets, no other target has that size) follow the internal resolution
// like every target, or with F1CE_SHADOW_RES=<k> are k times their original size whatever it is (0.5 = 512x1024, a
// lighter GPU load; 2 = 2048x4096, sharper edges). Their depth buffers take the same scale (get_depth).
// The reflection cube map's faces (the only 256x256 targets in races, and their shared depth buffer) likewise with
// F1CE_CUBE_RES=<k> (0.5 = 128x128, 2 = 512x512); the cube textures built from them take the faces' size.
namespace {
float env_res(const char* name) {
    return getenv(name) ? std::clamp((float)atof(getenv(name)), 0.25f, 4.0f) : 0;
}
float fixed_scale(u32 w, u32 h) {
    static const float shadow = env_res("F1CE_SHADOW_RES"), cube = env_res("F1CE_CUBE_RES");
    return w == 1024 && h == 2048 ? shadow : w == 256 && h == 256 ? cube : 0;
}
}  // namespace
float target_scale_x(u32 w, u32 h) { const float k = fixed_scale(w, h); return k ? k : res_scale_x(); }
float target_scale_y(u32 w, u32 h) { const float k = fixed_scale(w, h); return k ? k : res_scale_y(); }
void video_mode(u8* id, u8* aspect) {
    *id = format().lines == 1 ? 4 : format().lines == 2 ? 5 : 2;
    *aspect = format().narrow ? 1 : 2;
}
// A 3D scene builds its picture camera (16:9) every frame; the 2D screens build none and make at most ~50 draws. The draw
// count alone (>= 800) flickered: a lone car on track in the rain, as in the career driver tests, crosses it all the time,
// and each crossing switched the picture between the wide format and 16:9 between bars. So the scene starts with the
// first frame that builds a picture camera or makes 800 draws, and ends only after half a second of frames with neither.
std::atomic<u32> g_cameras{0};   // picture cameras built since the last frame end
std::atomic<bool> g_wide{false};
bool wide_frame() { return g_wide.load(std::memory_order_relaxed); }
void note_frame_scene(u64 draws) {
    static int quiet = 0;
    if (res_scale_x() == res_scale_y()) return;
    const bool scene = g_cameras.exchange(0) > 0 || draws >= 800;
    quiet = scene ? 0 : quiet + 1;
    if (scene != g_wide.load(std::memory_order_relaxed) && (scene || quiet >= 15)) {
        static int logged = 0;
        if (logged++ < 200) LOG_I("aspect: %s the wide format at %.2f s (%u draws)", scene ? "entering" : "leaving", time_usec() / 1e6, (u32)draws);
        g_wide = scene;
    }
}
double present_aspect(u32 w, u32 h) {
    if (format().lines) return format().narrow ? 4.0 / 3.0 : 16.0 / 9.0;   // (the PS3 modes have non-square pixels)
    return wide_frame() ? (double)w * res_scale_x() / (h * res_scale_y()) : (double)w / h;
}
void note_display(u32 offset) {
    for (u32& d : g_display) {
        if (d == offset) return;
        if (!d) { d = offset; return; }
    }
}
// The interface is the blended 2D work on the display buffer; the picture under it arrives there unblended.
bool hud_draw(u32 offset, bool blend, bool depth_test) {
    if (!blend || depth_test || !wide_frame()) return false;
    for (u32 d : g_display)
        if (d == offset) return true;
    return false;
}
void hud_rect(u32 pw, u32 ph, float* x, float* y, float* w, float* h) {
    *w = (float)pw; *h = (float)ph; *x = *y = 0;
    if ((double)pw / ph > 16.0 / 9.0) { *w = ph * 16.0f / 9.0f; *x = (pw - *w) / 2; }
    else { *h = pw * 9.0f / 16.0f; *y = (ph - *h) / 2; }
}
}  // namespace rsx

// Every camera builds its projection and culling frustum in 0x3FB310 from a vertical field of view (+0x238) and an aspect
// ratio (+0x23C); bit 0 of +0x234 asks for a rebuild. The picture camera is the one at 16:9. It is built for the format
// from the same fields, so the frustum follows and nothing is culled at the new edges: a wider format keeps the vertical
// field and opens the sides, a narrower one keeps the horizontal field and opens the top and bottom. The game's values
// are put back: it reads them again for its own purposes.
bool camera_projection_hook(PPUContext& c) {
    static thread_local bool inside = false;
    const u32 cam = (u32)c.r[3];
    if (inside) return false;
    const float aspect = std::bit_cast<float>(ld32(cam + 0x23C)), base = 16.0f / 9.0f;
    if (std::fabs(aspect - base) > 0.001f) return false;
    rsx::g_cameras++;
    if (!rsx::wide_frame()) return false;
    const float target = base * rsx::res_scale_x() / rsx::res_scale_y();
    const u32 fov = ld32(cam + 0x238), aspect_bits = ld32(cam + 0x23C), flags = ld32(cam + 0x234);
    if (target < base) st32(cam + 0x238, std::bit_cast<u32>(2 * std::atan(std::tan(std::bit_cast<float>(fov) / 2) * base / target)));
    st32(cam + 0x23C, std::bit_cast<u32>(target));
    st32(cam + 0x234, flags | 1);
    inside = true;
    sub_003FB310(c);
    inside = false;
    st32(cam + 0x238, fov);
    st32(cam + 0x23C, aspect_bits);
    return true;
}
