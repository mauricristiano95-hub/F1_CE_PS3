// Reflection cube map (foaDynamicCubeMap). The game renders all six faces every frame: at 30 fps that is 30 updates per
// second. With F1CE_60FPS the pass would run 60 times per second and costs ~20% of a grid frame (its culling and
// transform jobs on the SPUs, ~500 draws). So a frame starting less than 3/4 of the update interval after the last
// update keeps the previous faces (the cube's render target is not reused in between). F1CE_CUBE_HZ=<n> sets the
// updates per second (default 30 with F1CE_60FPS, as the original game at 30 fps, and no limit without it; 0: every
// frame); at 30 per second reflections refresh every other frame at 60 fps and every frame below ~40 fps. The main
// view's wait for the faces' job fences (0x2F0390) is skipped with the pass, since the fences would then be the
// previous frame's. F1CE_CUBE_SKIP=1 (experiment) never renders it. Hooks in tools/recomp.py HOOKS.
// F1CE_CUBE_STATIC=1 (static reflections): the faces are rendered during the first 2 s of a scene and then kept until
// the next one (a scene starts when the pass was not called for over a second: loading, menus). 2 s and not a single
// frame, since programs compiled in the background and streamed textures are missing from a scene's first frames.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "kernel.h"

namespace {
const bool g_never = getenv("F1CE_CUBE_SKIP") != nullptr;
const u64 g_interval = [] {
    const char* e = getenv("F1CE_CUBE_HZ");
    const int hz = e ? atoi(e) : getenv("F1CE_60FPS") ? 30 : 0;
    return hz > 0 ? (u64)(750000 / hz) : 0;
}();
const bool g_static = getenv("F1CE_CUBE_STATIC") && atoi(getenv("F1CE_CUBE_STATIC")) != 0;
u64 g_last_render = 0;
u64 g_last_call = 0, g_scene_start = 0;
bool g_skipped = false;   // the last pass was skipped: its fence wait goes too
u32 g_renders = 0, g_skips = 0;
u64 g_window = 0;

void report(u64 now) {
    if (!g_window) g_window = now;
    if (now - g_window < 5000000) return;
    LOG_I("cube map: %.1f updates/s, %.1f skipped/s", g_renders * 1e6 / (now - g_window), g_skips * 1e6 / (now - g_window));
    g_window = now;
    g_renders = g_skips = 0;
}
}  // namespace

// 0x2F0D14 foaDynamicCubeMap::Render (main thread, once per frame)
bool cube_render_hook(PPUContext&) {
    const u64 now = time_usec();
    report(now);
    if (now - g_last_call > 1000000) g_scene_start = now;
    g_last_call = now;
    g_skipped = g_never || (g_static ? now - g_scene_start > 2000000 : g_interval && now - g_last_render < g_interval);
    if (g_skipped) { g_skips++; return true; }
    g_last_render = now;
    g_renders++;
    return false;
}
// 0x2F3B34: the other Render (seen once, at load)
bool cube_render2_hook(PPUContext&) { return false; }
// 0x2F0390: waits for the six faces' job fences
bool cube_wait_hook(PPUContext&) {
    if (!g_skipped) return false;
    g_skipped = false;
    return true;
}
