// System dialogs over the game (overlay.h). Laid out with Dear ImGui's draw lists, rasterised here on the CPU (the
// renderers only blend the finished image), so OpenGL and Vulkan share one implementation. The dialog runs on the RSX
// thread, once per presented frame; the save threads wait for its answer.
#include "overlay.h"

#include "host.h"
#include "kernel.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <ctime>
#include <unordered_map>

#include "font_black_italic.h"
#include "font_medium.h"
#include "font_semibold_italic.h"

namespace overlay {
namespace {
// ------------------------------------------------------------------ software rasteriser for ImGui draw data
struct SoftTex { int w = 0, h = 0; const u8* px = nullptr; };   // RGBA8, straight alpha

struct Target {
    u32 w = 0, h = 0;
    std::vector<u32> px;   // premultiplied RGBA8
};

inline void blend(u32& d, u32 sr, u32 sg, u32 sb, u32 sa) {   // source premultiplied, 0..255
    const u32 ia = 255 - sa;
    const u32 dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255, da = d >> 24;
    d = (sr + (dr * ia + 127) / 255) | (sg + (dg * ia + 127) / 255) << 8 | (sb + (db * ia + 127) / 255) << 16 |
        (sa + (da * ia + 127) / 255) << 24;
}

void sample(const SoftTex& t, float u, float v, float o[4]) {   // bilinear, clamped
    const float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    const int x1 = std::clamp(x0 + 1, 0, t.w - 1), y1 = std::clamp(y0 + 1, 0, t.h - 1);
    x0 = std::clamp(x0, 0, t.w - 1);
    y0 = std::clamp(y0, 0, t.h - 1);
    const u8 *p00 = t.px + (y0 * t.w + x0) * 4, *p10 = t.px + (y0 * t.w + x1) * 4;
    const u8 *p01 = t.px + (y1 * t.w + x0) * 4, *p11 = t.px + (y1 * t.w + x1) * 4;
    for (int c = 0; c < 4; c++) {
        const float top = p00[c] + (p10[c] - p00[c]) * fx, bot = p01[c] + (p11[c] - p01[c]) * fx;
        o[c] = (top + (bot - top) * fy) * (1.0f / 255.0f);
    }
}

void unpack(ImU32 c, float o[4]) {
    for (int i = 0; i < 4; i++) o[i] = ((c >> (i * 8)) & 255) * (1.0f / 255.0f);
}

// One triangle, in 1/16 pixel fixed point: shared edges are exact, and the tie rule gives their pixels to one side
// only (else the seams of translucent rectangles would be blended twice).
void raster_tri(Target& t, const ImDrawVert* v0, const ImDrawVert* v1, const ImDrawVert* v2, const SoftTex* tex,
                int cx0, int cy0, int cx1, int cy1) {
    constexpr s64 SUB = 16;
    const ImDrawVert* v[3] = {v0, v1, v2};
    s64 X[3], Y[3];
    for (int i = 0; i < 3; i++) { X[i] = std::llround(v[i]->pos.x * SUB); Y[i] = std::llround(v[i]->pos.y * SUB); }
    s64 area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) return;
    if (area < 0) { std::swap(v[1], v[2]); std::swap(X[1], X[2]); std::swap(Y[1], Y[2]); area = -area; }
    const int minx = std::max(cx0, (int)std::floor((double)std::min({X[0], X[1], X[2]}) / SUB));
    const int maxx = std::min(cx1 - 1, (int)std::ceil((double)std::max({X[0], X[1], X[2]}) / SUB));
    const int miny = std::max(cy0, (int)std::floor((double)std::min({Y[0], Y[1], Y[2]}) / SUB));
    const int maxy = std::min(cy1 - 1, (int)std::ceil((double)std::max({Y[0], Y[1], Y[2]}) / SUB));
    if (minx > maxx || miny > maxy) return;
    // edge k runs from vertex k+1 to k+2 (the one opposite vertex k): E(p) = dx * (py - ya) - dy * (px - xa)
    s64 dx[3], dy[3], bias[3];
    for (int k = 0; k < 3; k++) {
        const int a = (k + 1) % 3, b = (k + 2) % 3;
        dx[k] = X[b] - X[a];
        dy[k] = Y[b] - Y[a];
        bias[k] = dy[k] > 0 || (dy[k] == 0 && dx[k] < 0) ? 0 : -1;
    }
    auto edge = [&](int k, s64 px, s64 py) {
        const int a = (k + 1) % 3;
        return dx[k] * (py - Y[a]) - dy[k] * (px - X[a]);
    };
    float c[3][4], uv[3][2];
    for (int i = 0; i < 3; i++) { unpack(v[i]->col, c[i]); uv[i][0] = v[i]->uv.x; uv[i][1] = v[i]->uv.y; }
    const bool flat_col = v[0]->col == v[1]->col && v[0]->col == v[2]->col;
    const bool flat_uv = uv[0][0] == uv[1][0] && uv[0][0] == uv[2][0] && uv[0][1] == uv[1][1] && uv[0][1] == uv[2][1];
    const bool textured = tex && tex->px;
    u32 fr = 0, fg = 0, fb = 0, fa = 0;   // the colour of a flat triangle, premultiplied
    if (flat_col && (flat_uv || !textured)) {
        float s[4] = {1, 1, 1, 1};
        if (textured) sample(*tex, uv[0][0], uv[0][1], s);
        const float a = c[0][3] * s[3];
        if (a <= 0.0f) return;
        fr = (u32)std::lround(c[0][0] * s[0] * a * 255);
        fg = (u32)std::lround(c[0][1] * s[1] * a * 255);
        fb = (u32)std::lround(c[0][2] * s[2] * a * 255);
        fa = (u32)std::lround(a * 255);
    }
    const bool flat = flat_col && (flat_uv || !textured);
    const float inv_area = 1.0f / (float)area;
    for (int y = miny; y <= maxy; y++) {
        const s64 py = y * SUB + SUB / 2, px = minx * SUB + SUB / 2;
        s64 e[3] = {edge(0, px, py), edge(1, px, py), edge(2, px, py)};
        u32* row = &t.px[(size_t)y * t.w];
        for (int x = minx; x <= maxx; x++, e[0] -= dy[0] * SUB, e[1] -= dy[1] * SUB, e[2] -= dy[2] * SUB) {
            if (((e[0] + bias[0]) | (e[1] + bias[1]) | (e[2] + bias[2])) < 0) continue;
            if (flat) { blend(row[x], fr, fg, fb, fa); continue; }
            const float l0 = e[0] * inv_area, l1 = e[1] * inv_area, l2 = 1.0f - l0 - l1;
            float col[4];
            for (int i = 0; i < 4; i++) col[i] = c[0][i] * l0 + c[1][i] * l1 + c[2][i] * l2;
            if (textured) {
                float s[4];
                sample(*tex, uv[0][0] * l0 + uv[1][0] * l1 + uv[2][0] * l2, uv[0][1] * l0 + uv[1][1] * l1 + uv[2][1] * l2, s);
                for (int i = 0; i < 4; i++) col[i] *= s[i];
            }
            const float a = std::clamp(col[3], 0.0f, 1.0f);
            if (a <= 0.0f) continue;
            blend(row[x], (u32)(std::clamp(col[0], 0.0f, 1.0f) * a * 255 + 0.5f), (u32)(std::clamp(col[1], 0.0f, 1.0f) * a * 255 + 0.5f),
                  (u32)(std::clamp(col[2], 0.0f, 1.0f) * a * 255 + 0.5f), (u32)(a * 255 + 0.5f));
        }
    }
}

void raster(Target& t, const ImDrawData* dd) {
    std::fill(t.px.begin(), t.px.end(), 0u);
    for (const ImDrawList* dl : dd->CmdLists)
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, (int)std::floor(cmd.ClipRect.x)), cy0 = std::max(0, (int)std::floor(cmd.ClipRect.y));
            const int cx1 = std::min((int)t.w, (int)std::ceil(cmd.ClipRect.z)), cy1 = std::min((int)t.h, (int)std::ceil(cmd.ClipRect.w));
            if (cx0 >= cx1 || cy0 >= cy1) continue;
            const SoftTex* tex = (const SoftTex*)(uintptr_t)cmd.GetTexID();
            const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = dl->VtxBuffer.Data + cmd.VtxOffset;
            for (u32 i = 0; i + 2 < cmd.ElemCount; i += 3)
                raster_tri(t, &vtx[idx[i]], &vtx[idx[i + 1]], &vtx[idx[i + 2]], tex, cx0, cy0, cx1, cy1);
        }
}

u64 hash_draw_data(const ImDrawData* dd) {   // FNV-1a over everything that is drawn
    u64 h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const u8* b = (const u8*)p;
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const ImDrawList* dl : dd->CmdLists) {
        mix(dl->VtxBuffer.Data, (size_t)dl->VtxBuffer.Size * sizeof(ImDrawVert));
        mix(dl->IdxBuffer.Data, (size_t)dl->IdxBuffer.Size * sizeof(ImDrawIdx));
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            const ImTextureID id = cmd.GetTexID();
            mix(&cmd.ClipRect, sizeof(cmd.ClipRect));
            mix(&id, sizeof(id));
            mix(&cmd.ElemCount, sizeof(cmd.ElemCount));
            mix(&cmd.IdxOffset, sizeof(cmd.IdxOffset));
            mix(&cmd.VtxOffset, sizeof(cmd.VtxOffset));
        }
    }
    return h;
}

// ------------------------------------------------------------------ dialog state
struct Item {
    SaveEntry e;
    std::vector<u8> icon;   // decoded ICON0, RGBA8
    int iw = 0, ih = 0;
    SoftTex tex;
    std::string when, size;
};
enum class Kind { None, List, Message };
struct State {
    Kind kind = Kind::None;
    SaveMode mode = SaveMode::Load;
    std::vector<Item> items;
    int sel = 0, top = 0;   // focused item, first row shown
    bool asking = false;    // the overwrite / delete question is up
    int answer = 0;         // its focused button: 0 yes, 1 no
    std::string text;       // the message
    bool done = false;
    int result = -1;
    u64 serial = 0;
};
std::mutex g_m;
std::condition_variable g_cv;
State g_st;
u64 g_serial = 0;
std::atomic<bool> g_open{false};       // a dialog is up
std::atomic<bool> g_captured{false};   // the pad belongs to the dialog (until its buttons are released)
std::atomic<u64> g_last_frame{0};      // time_usec of the last presented frame

bool ui_enabled() {
    static const bool on = !(getenv("F1CE_SAVE_UI") && atoi(getenv("F1CE_SAVE_UI")) == 0);
    return on;
}
bool presenting(u64 within_us) {
    const u64 last = g_last_frame.load();
    return last && time_usec() - last < within_us;
}

void decode_icon(Item& it) {
    if (it.e.icon_png.empty()) return;
    SDL_Surface* s = SDL_LoadPNG_IO(SDL_IOFromConstMem(it.e.icon_png.data(), it.e.icon_png.size()), true);
    if (!s) return;
    if (SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32)) {
        it.iw = c->w;
        it.ih = c->h;
        it.icon.resize((size_t)c->w * c->h * 4);
        for (int y = 0; y < c->h; y++) memcpy(&it.icon[(size_t)y * c->w * 4], (const u8*)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
        SDL_DestroySurface(c);
    }
    SDL_DestroySurface(s);
    it.e.icon_png.clear();
}

std::string format_size(u64 b) {
    char s[32];
    if (b >= 1024 * 1024) snprintf(s, sizeof(s), "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(s, sizeof(s), "%llu KB", (unsigned long long)((b + 1023) / 1024));
    return s;
}
std::string format_time(s64 t) {
    if (!t) return "";
    const time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char s[32];
    strftime(s, sizeof(s), "%d/%m/%Y  %H:%M", &lt);
    return s;
}

// Opens the dialog and waits for its answer (UNAVAILABLE when nothing shows it)
int run(State st) {
    if (!ui_enabled()) return UNAVAILABLE;
    for (int i = 0; i < 30 && !presenting(2000000); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!presenting(2000000)) { LOG_W("overlay: no frames are presented, the save dialog is skipped"); return UNAVAILABLE; }
    std::unique_lock lk(g_m);
    g_st = std::move(st);
    g_st.serial = ++g_serial;
    g_st.done = false;
    g_captured = true;
    g_open = true;
    LOG_I("overlay: dialog %llu open (input time %.1f s)", (unsigned long long)g_st.serial, host::input_time());
    while (!g_st.done) {
        g_cv.wait_for(lk, std::chrono::milliseconds(250));
        if (!g_st.done && !presenting(5000000)) {   // the game stopped presenting: the dialog cannot be seen
            LOG_W("overlay: frames stopped while the save dialog was open; it is skipped");
            g_st.kind = Kind::None;
            g_open = false;
            return UNAVAILABLE;
        }
    }
    return g_st.result;
}

void finish(int result) {   // (g_m held)
    LOG_I("overlay: dialog %llu answered %d", (unsigned long long)g_st.serial, result);
    g_st.result = result;
    g_st.done = true;
    g_st.kind = Kind::None;
    g_open = false;
    g_cv.notify_all();
}

// ------------------------------------------------------------------ input
struct Buttons { bool up = false, down = false, left = false, right = false, ok = false, back = false; };
Buttons read_buttons() {
    const host::PadState p = host::pad_state();
    Buttons b;
    b.up = (p.digital1 & 0x10) || p.ly < 0x40;
    b.down = (p.digital1 & 0x40) || p.ly > 0xC0;
    b.left = (p.digital1 & 0x80) || p.lx < 0x40;
    b.right = (p.digital1 & 0x20) || p.lx > 0xC0;
    b.ok = (p.digital2 & 0x40) || (p.digital1 & 0x08);   // cross, start (Enter)
    b.back = (p.digital2 & 0x20) != 0;                  // circle
    const host::WheelInput w = host::wheel_input();
    if (w.connected) {
        b.up |= (w.buttons & host::WBTN_UP) != 0;
        b.down |= (w.buttons & host::WBTN_DOWN) != 0;
        b.left |= (w.buttons & host::WBTN_LEFT) != 0;
        b.right |= (w.buttons & host::WBTN_RIGHT) != 0;
        b.ok |= (w.buttons & (host::WBTN_CROSS | host::WBTN_START)) != 0;
        b.back |= (w.buttons & host::WBTN_CIRCLE) != 0;
    }
    return b;
}

// presses of this frame: new presses, and held directions repeating
Buttons pressed(u64 serial) {
    static u64 seen = 0;
    static Buttons prev;
    static u64 held_since[4] = {}, last_rep[4] = {};
    const Buttons b = read_buttons();
    Buttons out;
    if (serial != seen) {   // a new dialog: what is already held (the press that opened it) does not count
        seen = serial;
        prev = b;
        return out;
    }
    const u64 now = time_usec();
    const bool cur[4] = {b.up, b.down, b.left, b.right}, was[4] = {prev.up, prev.down, prev.left, prev.right};
    bool fire[4] = {};
    for (int i = 0; i < 4; i++) {
        if (cur[i] && !was[i]) { fire[i] = true; held_since[i] = last_rep[i] = now; }
        else if (cur[i] && now - held_since[i] > 400000 && now - last_rep[i] > 90000) { fire[i] = true; last_rep[i] = now; }
    }
    out.up = fire[0]; out.down = fire[1]; out.left = fire[2]; out.right = fire[3];
    out.ok = b.ok && !prev.ok;
    out.back = b.back && !prev.back;
    prev = b;
    return out;
}

int visible_rows = 4;   // of the last layout

void update(const Buttons& b) {   // (g_m held)
    State& s = g_st;
    if (b.up || b.down || b.left || b.right || b.ok || b.back)
        LOG_I("overlay: input%s%s%s%s%s%s at %.1f s", b.up ? " up" : "", b.down ? " down" : "", b.left ? " left" : "",
              b.right ? " right" : "", b.ok ? " ok" : "", b.back ? " back" : "", host::input_time());
    if (s.kind == Kind::Message) {
        if (b.ok || b.back) finish(0);
        return;
    }
    const int n = (int)s.items.size();
    if (s.asking) {
        if (b.left || b.right || b.up || b.down) s.answer ^= 1;
        if (b.back) s.asking = false;
        else if (b.ok) {
            if (s.answer == 0) finish(s.sel);
            else s.asking = false;
        }
        return;
    }
    if (b.up) s.sel = std::max(0, s.sel - 1);
    if (b.down) s.sel = std::min(n - 1, s.sel + 1);
    if (b.left) s.sel = std::max(0, s.sel - visible_rows);
    if (b.right) s.sel = std::min(n - 1, s.sel + visible_rows);
    if (b.back) { finish(-1); return; }
    if (b.ok && n) {
        const Item& it = s.items[s.sel];
        if (s.mode == SaveMode::Delete || (s.mode == SaveMode::Save && !it.e.is_new)) {
            s.asking = true;
            s.answer = s.mode == SaveMode::Delete ? 1 : 0;   // (deleting defaults to "No")
        } else {
            finish(s.sel);
        }
    }
}

// ------------------------------------------------------------------ drawing
struct Fonts { ImFont *black = nullptr, *semi = nullptr, *medium = nullptr; } F;
const ImU32 INK = IM_COL32(27, 27, 27, 255), GREY = IM_COL32(120, 120, 120, 255), RED = IM_COL32(211, 47, 47, 255),
            CARBON = IM_COL32(22, 22, 22, 255), WHITE = IM_COL32(255, 255, 255, 255);

float text_w(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s).x; }
void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, const std::string& s, float wrap = 0) {
    dl->AddText(f, size, p, c, s.c_str(), s.c_str() + s.size(), wrap);
}

// the pad symbol in a dark disc: 'x' cross, 'o' circle
void button_icon(ImDrawList* dl, ImVec2 c, float r, char sym) {
    dl->AddCircleFilled(c, r, CARBON, 32);
    if (sym == 'x') {
        const float d = r * 0.42f, th = std::max(1.5f, r * 0.2f);
        const ImU32 col = IM_COL32(124, 178, 232, 255);
        dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), col, th);
        dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), col, th);
    } else {
        dl->AddCircle(c, r * 0.48f, IM_COL32(255, 102, 102, 255), 32, std::max(1.5f, r * 0.18f));
    }
}
// hints right-aligned at (right, cy): returns the left end
float hints(ImDrawList* dl, float right, float cy, float S, std::initializer_list<std::pair<char, const char*>> items) {
    float x = right;
    for (auto it = std::rbegin(items); it != std::rend(items); ++it) {
        const float tw = text_w(F.semi, 17 * S, it->second);
        x -= tw;
        text(dl, F.semi, 17 * S, ImVec2(x, cy - 11 * S), INK, it->second);
        x -= 20 * S;
        button_icon(dl, ImVec2(x, cy), 12 * S, it->first);
        x -= 32 * S;
    }
    return x;
}

void draw_icon(ImDrawList* dl, Item& it, ImVec2 p, ImVec2 size) {
    if (!it.icon.empty()) {
        it.tex = {it.iw, it.ih, it.icon.data()};
        // the icon's aspect kept (ICON0 is 320x176), centred in the box
        const float a = (float)it.iw / it.ih, ba = size.x / size.y;
        ImVec2 s = a > ba ? ImVec2(size.x, size.x / a) : ImVec2(size.y * a, size.y);
        ImVec2 q(p.x + (size.x - s.x) / 2, p.y + (size.y - s.y) / 2);
        dl->AddImage((ImTextureID)(uintptr_t)&it.tex, q, ImVec2(q.x + s.x, q.y + s.y));
        return;
    }
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(205, 205, 205, 255));
    if (it.e.is_new) {   // a plus sign
        const ImVec2 c(p.x + size.x / 2, p.y + size.y / 2);
        const float d = size.y * 0.22f, th = size.y * 0.07f;
        dl->AddRectFilled(ImVec2(c.x - d, c.y - th), ImVec2(c.x + d, c.y + th), WHITE);
        dl->AddRectFilled(ImVec2(c.x - th, c.y - d), ImVec2(c.x + th, c.y + d), WHITE);
    }
}

// a box over the list with a question (yes/no) or a message (OK)
void draw_box(ImDrawList* dl, float W, float H, float S, const std::string& msg, Item* it, bool yes_no, int answer) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 110));
    const float bw = std::min(W - 48 * S, 640 * S), bh = it ? 300 * S : 210 * S;
    const ImVec2 a((W - bw) / 2, (H - bh) / 2), b(a.x + bw, a.y + bh);
    dl->AddRectFilled(ImVec2(a.x + 6 * S, a.y + 8 * S), ImVec2(b.x + 6 * S, b.y + 8 * S), IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(a, b, WHITE);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + 6 * S), RED);
    float y = a.y + 30 * S;
    text(dl, F.semi, 23 * S, ImVec2(a.x + 32 * S, y), INK, msg, bw - 64 * S);
    y += 50 * S;
    if (it) {
        draw_icon(dl, *it, ImVec2(a.x + 32 * S, y), ImVec2(160 * S, 88 * S));
        dl->PushClipRect(ImVec2(a.x, y), ImVec2(b.x - 24 * S, y + 90 * S), true);
        text(dl, F.semi, 20 * S, ImVec2(a.x + 212 * S, y + 6 * S), INK, it->e.title);
        text(dl, F.medium, 16 * S, ImVec2(a.x + 212 * S, y + 36 * S), GREY, it->e.subtitle);
        text(dl, F.medium, 14 * S, ImVec2(a.x + 212 * S, y + 60 * S), GREY, it->when);
        dl->PopClipRect();
    }
    const float btn_w = 150 * S, btn_h = 46 * S, by = b.y - btn_h - 26 * S;
    auto button = [&](float x, const char* label, bool focus) {
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + btn_w, by + btn_h), focus ? CARBON : IM_COL32(236, 236, 236, 255));
        if (focus) dl->AddRectFilled(ImVec2(x, by + btn_h - 4 * S), ImVec2(x + btn_w, by + btn_h), RED);
        const float tw = text_w(F.semi, 19 * S, label);
        text(dl, F.semi, 19 * S, ImVec2(x + (btn_w - tw) / 2, by + 11 * S), focus ? WHITE : INK, label);
    };
    if (yes_no) {
        button(a.x + bw / 2 - btn_w - 10 * S, "Yes", answer == 0);
        button(a.x + bw / 2 + 10 * S, "No", answer == 1);
    } else {
        button(a.x + (bw - btn_w) / 2, "OK", true);
    }
}

void draw(ImDrawList* dl, float W, float H) {
    State& s = g_st;
    const float S = std::min(W / 1280.0f, H / 720.0f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 150));
    if (s.kind == Kind::Message) {
        draw_box(dl, W, H, S, s.text, nullptr, false, 0);
        return;
    }
    const float pw = std::min(W - 48 * S, 1000 * S), ph = std::min(H - 48 * S, 620 * S);
    const ImVec2 a((W - pw) / 2, (H - ph) / 2), b(a.x + pw, a.y + ph);
    dl->AddRectFilled(ImVec2(a.x + 8 * S, a.y + 10 * S), ImVec2(b.x + 8 * S, b.y + 10 * S), IM_COL32(0, 0, 0, 80));
    dl->AddRectFilled(a, b, WHITE);
    // header
    const float hh = 70 * S;
    dl->AddRectFilled(a, ImVec2(b.x, a.y + hh), CARBON);
    dl->AddRectFilled(ImVec2(a.x, a.y + hh), ImVec2(b.x, a.y + hh + 5 * S), RED);
    const char* title = s.mode == SaveMode::Save ? "SAVE" : s.mode == SaveMode::Load ? "LOAD" : "DELETE";
    const char* sub = s.mode == SaveMode::Save ? "Select the data to save to" : s.mode == SaveMode::Load ? "Select the data to load"
                                                                                                        : "Select the data to delete";
    text(dl, F.black, 34 * S, ImVec2(a.x + 30 * S, a.y + 14 * S), WHITE, title);
    const float tw = text_w(F.black, 34 * S, title);
    text(dl, F.semi, 18 * S, ImVec2(a.x + 30 * S + tw + 22 * S, a.y + 26 * S), IM_COL32(189, 189, 189, 255), sub);
    // list
    const int n = (int)s.items.size();
    const float row_h = 114 * S, lx0 = a.x + 16 * S, lx1 = b.x - 16 * S, ly0 = a.y + hh + 5 * S + 14 * S, ly1 = b.y - 64 * S;
    visible_rows = std::max(1, (int)((ly1 - ly0) / row_h));
    if (s.sel < s.top) s.top = s.sel;
    if (s.sel >= s.top + visible_rows) s.top = s.sel - visible_rows + 1;
    s.top = std::clamp(s.top, 0, std::max(0, n - visible_rows));
    dl->PushClipRect(ImVec2(lx0, ly0), ImVec2(lx1, ly1), true);
    for (int i = s.top; i < n && i < s.top + visible_rows + 1; i++) {
        Item& it = s.items[i];
        const float y = ly0 + (i - s.top) * row_h;
        const bool sel = i == s.sel;
        const ImVec2 r0(lx0, y), r1(lx1 - (n > visible_rows ? 12 * S : 0), y + row_h - 4 * S);
        dl->AddRectFilled(r0, r1, sel ? CARBON : i % 2 ? IM_COL32(238, 238, 238, 255) : IM_COL32(247, 247, 247, 255));
        if (sel) dl->AddRectFilled(r0, ImVec2(r0.x + 6 * S, r1.y), RED);
        draw_icon(dl, it, ImVec2(r0.x + 18 * S, y + 11 * S), ImVec2(160 * S, 88 * S));
        const float tx = r0.x + 196 * S, info_w = 190 * S;
        const ImU32 main = sel ? WHITE : INK, dim = sel ? IM_COL32(190, 190, 190, 255) : GREY;
        if (!it.e.is_new) {
            const float wx = r1.x - 18 * S;
            text(dl, F.medium, 15 * S, ImVec2(wx - text_w(F.medium, 15 * S, it.when.c_str()), y + 12 * S), dim, it.when);
            text(dl, F.medium, 15 * S, ImVec2(wx - text_w(F.medium, 15 * S, it.size.c_str()), y + 34 * S), dim, it.size);
        }
        dl->PushClipRect(ImVec2(tx, y), ImVec2(it.e.is_new ? r1.x - 18 * S : r1.x - info_w, r1.y), true);
        text(dl, F.semi, 21 * S, ImVec2(tx, y + 6 * S), main, it.e.title);
        text(dl, F.medium, 16 * S, ImVec2(tx, y + 33 * S), main, it.e.subtitle);
        text(dl, F.medium, 13 * S, ImVec2(tx, y + 56 * S), dim, it.e.detail, r1.x - 18 * S - tx);
        dl->PopClipRect();
    }
    dl->PopClipRect();
    if (n > visible_rows) {   // scroll bar
        const float sx = lx1 - 6 * S, track = ly1 - ly0;
        dl->AddRectFilled(ImVec2(sx, ly0), ImVec2(lx1, ly1), IM_COL32(230, 230, 230, 255));
        const float t0 = ly0 + track * s.top / n, t1 = ly0 + track * std::min(n, s.top + visible_rows) / n;
        dl->AddRectFilled(ImVec2(sx, t0), ImVec2(lx1, t1), IM_COL32(120, 120, 120, 255));
    }
    // footer
    const float fy = b.y - 32 * S;
    if (n) {
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d / %d", s.sel + 1, n);
        text(dl, F.medium, 16 * S, ImVec2(a.x + 30 * S, fy - 10 * S), GREY, cnt);
    }
    hints(dl, b.x - 30 * S, fy, S, {{'x', "Select"}, {'o', "Back"}});
    if (s.asking && n) {
        const char* q = s.mode == SaveMode::Delete ? "Delete this data?" : "Do you want to overwrite the saved data?";
        draw_box(dl, W, H, S, q, &s.items[s.sel], true, s.answer);
    }
}

// ------------------------------------------------------------------ replay director bar
ReplayBar g_bar;   // (g_m)
std::atomic<bool> g_bar_active{false};
std::atomic<u64> g_bar_at{0};   // last set_replay_bar: the director updates the bar every frame of a replay
ReplayBarInput g_bar_input;   // (g_m) clicks not yet taken
struct BarMouse {
    float x = -1, y = -1;      // pixels, -1 outside the window
    bool down = false;         // left button
    bool drag = false;         // pressed on the timeline: the knob follows the mouse, released = seek
    bool press_play = false;   // pressed on the play / pause symbol
} g_bm;   // (g_m)

std::string clock_text(float s) {
    const int t = (int)(s * 10 + 0.5f);
    char b[24];
    snprintf(b, sizeof b, "%d:%02d.%d", t / 600, t / 10 % 60, t % 10);
    return b;
}

// a strip at the bottom of the central 16:9 rect: play state, speed, timeline, time code, car watched, button hints
struct BarLayout {
    float S, l, r, y0, y1;   // the strip
    float sx, sy, ss;        // play / pause symbol: centre and half size
    float tl, tr, ty;        // timeline ends and height
};
BarLayout bar_layout(float W, float H) {
    BarLayout L;
    L.S = std::min(W / 1280.0f, H / 720.0f);
    const float S = L.S, cw = 1280 * S, x0 = (W - cw) / 2;
    // (below the game's own replay captions: the driver's name box ends at y 630 of 720)
    const float bh = 70 * S;
    L.y0 = H - (H - 720 * S) / 2 - bh - 8 * S;
    L.y1 = L.y0 + bh;
    L.l = x0 + 40 * S;
    L.r = x0 + cw - 40 * S;
    L.sx = L.l + 30 * S;
    L.sy = L.y0 + 24 * S;
    L.ss = 12 * S;
    // (a fixed width for the time code, so the timeline does not move with its digits)
    const float tcw = text_w(F.semi, 18 * S, "88:88.8 / 88:88.8");
    L.tl = L.sx + 130 * S;
    L.tr = L.r - 300 * S - tcw;
    L.ty = L.sy;
    return L;
}
bool on_timeline(const BarLayout& L, float x, float y) {
    return x >= L.tl - 10 * L.S && x <= L.tr + 10 * L.S && std::fabs(y - L.ty) <= 14 * L.S;
}
bool on_play(const BarLayout& L, float x, float y) { return std::fabs(x - L.sx) <= 22 * L.S && std::fabs(y - L.sy) <= 18 * L.S; }
bool on_bar(const BarLayout& L, float x, float y) { return x >= L.l && x <= L.r && y >= L.y0 && y <= L.y1; }

// the mouse of this frame (g_m held): presses and releases on the bar become g_bar_input
void bar_mouse(float W, float H) {
    const host::MouseState m = host::mouse_state();
    const BarLayout L = bar_layout(W, H);
    const bool down = (m.buttons & 1) != 0, in = m.x >= 0;
    const float x = m.x * W, y = m.y * H;
    if (down && !g_bm.down && in && g_bar.active) {   // press
        if (on_timeline(L, x, y)) g_bm.drag = true;
        else if (on_play(L, x, y)) g_bm.press_play = true;
        LOG_I("overlay: replay bar press at %.0f, %.0f of %.0fx%.0f (%s)", x, y, W, H,
              g_bm.drag ? "timeline" : g_bm.press_play ? "play / pause" : "elsewhere");
    }
    if (!down && g_bm.down) {   // release
        if (g_bm.drag && in) g_bar_input.seek = std::clamp((x - L.tl) / (L.tr - L.tl), 0.0f, 1.0f);
        if (g_bm.press_play && in && on_play(L, x, y)) g_bar_input.toggle_pause = true;
        g_bm.drag = g_bm.press_play = false;
    }
    if (!in) g_bm.drag = g_bm.press_play = false;
    g_bm.down = down;
    g_bm.x = in ? x : -1;
    g_bm.y = in ? y : -1;
}

// shown for the pad (a director button lately), while the mouse moves over the window or rests on the bar, while dragging
bool bar_visible(float W, float H) {
    if (!g_bar.active) return false;
    if (g_bar.shown || g_bm.drag) return true;
    const host::MouseState m = host::mouse_state();
    if (m.x < 0) return false;
    if (time_usec() - m.moved_at < 3000000) return true;
    return on_bar(bar_layout(W, H), m.x * W, m.y * H);
}

void draw_bar(ImDrawList* dl, float W, float H) {
    const ReplayBar& b = g_bar;
    const BarLayout L = bar_layout(W, H);
    const float S = L.S, sx = L.sx, sy = L.sy, ss = L.ss, tl = L.tl, tr = L.tr, ty = L.ty, r = L.r;
    dl->AddRectFilled(ImVec2(L.l, L.y0), ImVec2(r, L.y1), IM_COL32(16, 16, 16, 200));
    dl->AddRectFilled(ImVec2(L.l, L.y0), ImVec2(L.l + 6 * S, L.y1), RED);
    // play / pause symbol (red under the mouse)
    const ImU32 sym = g_bm.x >= 0 && on_play(L, g_bm.x, g_bm.y) ? RED : WHITE;
    if (b.paused) {
        dl->AddRectFilled(ImVec2(sx - ss * 0.8f, sy - ss), ImVec2(sx - ss * 0.25f, sy + ss), sym);
        dl->AddRectFilled(ImVec2(sx + ss * 0.25f, sy - ss), ImVec2(sx + ss * 0.8f, sy + ss), sym);
    } else {
        dl->AddTriangleFilled(ImVec2(sx - ss * 0.7f, sy - ss), ImVec2(sx + ss, sy), ImVec2(sx - ss * 0.7f, sy + ss), sym);
    }
    text(dl, F.black, 22 * S, ImVec2(sx + 26 * S, sy - 14 * S), WHITE, b.speed);
    // timeline; while dragging, the knob and the time code follow the mouse
    float f = b.length > 0 ? std::clamp(b.pos / b.length, 0.0f, 1.0f) : 0;
    if (g_bm.drag && g_bm.x >= 0) f = std::clamp((g_bm.x - tl) / (tr - tl), 0.0f, 1.0f);
    const std::string tc = clock_text(g_bm.drag ? f * b.length : b.pos) + " / " + clock_text(b.length);
    dl->AddRectFilled(ImVec2(tl, ty - 3 * S), ImVec2(tr, ty + 3 * S), IM_COL32(90, 90, 90, 255));
    const float px = std::floor(tl + (tr - tl) * f);   // (whole pixels: the image is rasterised again only when it changes)
    dl->AddRectFilled(ImVec2(tl, ty - 3 * S), ImVec2(px, ty + 3 * S), RED);
    // the time under the mouse, above the bar
    if (!g_bm.drag && g_bm.x >= 0 && on_timeline(L, g_bm.x, g_bm.y) && b.length > 0) {
        const float hx = std::floor(std::clamp(g_bm.x, tl, tr));
        const std::string ht = clock_text((hx - tl) / (tr - tl) * b.length);
        const float hw = text_w(F.semi, 15 * S, ht.c_str());
        dl->AddRectFilled(ImVec2(hx - 1 * S, ty - 9 * S), ImVec2(hx + 1 * S, ty + 9 * S), WHITE);
        dl->AddRectFilled(ImVec2(hx - hw / 2 - 6 * S, L.y0 - 26 * S), ImVec2(hx + hw / 2 + 6 * S, L.y0 - 4 * S),
                          IM_COL32(16, 16, 16, 220));
        text(dl, F.semi, 15 * S, ImVec2(hx - hw / 2, L.y0 - 24 * S), WHITE, ht);
    }
    dl->AddCircleFilled(ImVec2(px, ty), (g_bm.drag ? 10 : 8) * S, WHITE, 24);
    text(dl, F.semi, 18 * S, ImVec2(tr + 16 * S, ty - 11 * S), WHITE, tc);
    // car watched
    const float dw = text_w(F.semi, 20 * S, b.driver.c_str());
    text(dl, F.semi, 20 * S, ImVec2(r - 24 * S - dw, ty - 13 * S), WHITE, b.driver);
    // camera, under the car
    const float cw = text_w(F.semi, 15 * S, b.camera.c_str());
    text(dl, F.semi, 15 * S, ImVec2(r - 24 * S - cw, L.y1 - 24 * S), RED, b.camera);
    // hints
    const char* h = "SQUARE  play / pause     L2 / R2  speed     L1 / R1  car     CIRCLE  standard cameras     TRIANGLE  replay"
                    " camera     SELECT  TV mode     MOUSE  timeline";
    text(dl, F.medium, 14 * S, ImVec2(sx - ss, L.y1 - 24 * S), IM_COL32(180, 180, 180, 255), h);
}

// ------------------------------------------------------------------ FPS counter
std::atomic<bool> g_fps_on{[] {
    const char* e = getenv("F1CE_SHOW_FPS");
    return e && *e && atoi(e) != 0;
}()};
u64 g_fps_win_start = 0;
u32 g_fps_win_frames = 0;
int g_fps_disp = 0;
float g_ms_disp = 0.0f;
bool g_fps_valid = false;

void update_fps(u64 now) {
    if (!g_fps_win_start) {
        g_fps_win_start = now;
        g_fps_win_frames = 0;
        return;
    }
    g_fps_win_frames++;
    const u64 elapsed = now - g_fps_win_start;
    if (elapsed >= 500000) {
        g_fps_disp = (int)std::lround(g_fps_win_frames * 1e6 / (double)elapsed);
        g_ms_disp = (float)((double)elapsed / (g_fps_win_frames * 1000.0));
        g_fps_valid = true;
        g_fps_win_start = now;
        g_fps_win_frames = 0;
    }
}

// 2006 F1 / FOM broadcast HUD primitives (metallic white header, steel-blue bar, dark translucent timing cells)
void f1_grad_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot) {
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
}
void f1_shadow_text(ImDrawList* dl, ImFont* f, float sz, ImVec2 p, ImU32 col, const std::string& s, float S) {
    text(dl, f, sz, ImVec2(p.x + 1.0f * S, p.y + 1.0f * S), IM_COL32(0, 0, 0, 175), s);
    text(dl, f, sz, p, col, s);
}
void f1_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float S) {
    // Subtle outer drop shadow + 1 px top specular gloss + metallic grey border
    dl->AddRectFilled(ImVec2(a.x + 2 * S, b.y), ImVec2(b.x + 2 * S, b.y + 2 * S), IM_COL32(0, 0, 0, 95));
    dl->AddRectFilled(ImVec2(b.x, a.y + 2 * S), ImVec2(b.x + 2 * S, b.y + 2 * S), IM_COL32(0, 0, 0, 95));
    dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, a.y + 1 * S), IM_COL32(255, 255, 255, 65));
    dl->AddRect(a, b, IM_COL32(172, 178, 190, 165), 0.0f, 0, std::max(1.0f, 1.0f * S));
}

void draw_fps(ImDrawList* dl, float W, float H) {
    const float S = std::min(W / 1280.0f, H / 720.0f);
    char num[16], ms[24];
    if (g_fps_valid) {
        snprintf(num, sizeof(num), "%d", g_fps_disp);
        snprintf(ms, sizeof(ms), "%.1f ms", g_ms_disp);
    } else {
        snprintf(num, sizeof(num), "--");
        snprintf(ms, sizeof(ms), "--.- ms");
    }
    const float sz_tag = 13 * S, sz_num = 16 * S, sz_ms = 13 * S;
    const float tag_w = 38 * S;
    const float num_w = std::max(text_w(F.medium, sz_num, "120") + 16 * S, 44 * S);
    const float ms_w = std::max(text_w(F.medium, sz_ms, "88.8 ms") + 16 * S, 62 * S);
    const float bw = tag_w + num_w + ms_w, bh = 24 * S;
    const float x0 = 14 * S, x1 = x0 + bw;
    const float y1 = H - 14 * S, y0 = y1 - bh;

    // 2006 F1 broadcast style: metallic white label cell | dark charcoal FPS value | steel-grey frametime cell
    f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x0 + tag_w, y1), IM_COL32(242, 244, 248, 240), IM_COL32(204, 209, 218, 240));
    f1_grad_rect(dl, ImVec2(x0 + tag_w, y0), ImVec2(x0 + tag_w + num_w, y1), IM_COL32(34, 37, 44, 228), IM_COL32(14, 16, 20, 238));
    f1_grad_rect(dl, ImVec2(x0 + tag_w + num_w, y0), ImVec2(x1, y1), IM_COL32(84, 91, 108, 224), IM_COL32(58, 63, 77, 234));
    dl->AddLine(ImVec2(x0 + tag_w, y0), ImVec2(x0 + tag_w, y1), IM_COL32(20, 22, 28, 200), 1.0f);
    dl->AddLine(ImVec2(x0 + tag_w + num_w, y0), ImVec2(x0 + tag_w + num_w, y1), IM_COL32(20, 22, 28, 180), 1.0f);
    f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);

    const float tw = text_w(F.medium, sz_tag, "FPS");
    text(dl, F.medium, sz_tag, ImVec2(x0 + (tag_w - tw) * 0.5f, y0 + 5 * S), IM_COL32(22, 25, 32, 255), "FPS");
    const float nw = text_w(F.medium, sz_num, num);
    f1_shadow_text(dl, F.medium, sz_num, ImVec2(x0 + tag_w + (num_w - nw) * 0.5f, y0 + 3.5f * S), WHITE, num, S);
    const float mw = text_w(F.medium, sz_ms, ms);
    f1_shadow_text(dl, F.medium, sz_ms, ImVec2(x0 + tag_w + num_w + (ms_w - mw) * 0.5f, y0 + 5 * S), IM_COL32(228, 232, 240, 255), ms, S);
}

// ------------------------------------------------------------------ Fastest Lap / timing pill (F4 / F1CE_SHOW_LAPTIMES)
std::atomic<bool> g_lap_on{[] {
    const char* e = getenv("F1CE_SHOW_LAPTIMES");
    return e && *e && atoi(e) != 0;
}()};

struct LapSnapshot {
    bool in_session = false;
    int ai_best_ticks = 0;
    int ai_best_team = -1;
    int human_best_ticks = 0;
    float dt = 0.0f;
    float wetness = 0.0f;
};
LapSnapshot g_lap;
u64 g_lap_sample_at = 0;

constexpr const char* kTeamTag[11] = {
    "REN", "MCL", "FER", "TOY", "WIL", "HON", "RBR", "BMW", "MF1", "STR", "SAF"
};

std::string fmt_lap_time(int ticks, float dt) {
    if (ticks <= 0 || dt <= 0.0f) return "--:--.---";
    const double sec = (double)ticks * (double)dt;
    const int total_ms = (int)std::lround(sec * 1000.0);
    const int mins = total_ms / 60000;
    const int secs = (total_ms / 1000) % 60;
    const int ms = total_ms % 1000;
    char b[24];
    snprintf(b, sizeof(b), "%d:%02d.%03d", mins, secs, ms);
    return b;
}

void sample_laptimes(u64 now) {
    if (g_lap_sample_at && now - g_lap_sample_at < 500000) return;
    g_lap_sample_at = now;
    LapSnapshot cur;
    if (!mem::readable(0x100DB3B8, 4) || !mem::readable(0x10041BA8, 4)) {
        g_lap = cur;
        return;
    }
    const u32 tl = ld32(0x100DB3B8);
    if (!tl || !mem::readable(tl + 0x7F0, 4) || !mem::readable(tl + 0x1AC8, 11 * 12)) {
        g_lap = cur;
        return;
    }
    const u32 ti = ld32(tl + 0x7F0);
    if (!ti || !mem::readable(ti, 0x20)) {
        g_lap = cur;
        return;
    }
    const float dt = std::bit_cast<float>(ld32(0x10041BA8));
    if (!(dt > 0.005f && dt < 0.05f)) {
        g_lap = cur;
        return;
    }
    cur.in_session = true;
    cur.dt = dt;
    if (mem::readable(tl + 0x80C, 4)) {
        const u32 w = ld32(tl + 0x80C);
        if (w && mem::readable(w + 0x24, 4))
            cur.wetness = std::clamp(std::bit_cast<float>(ld32(w + 0x24)), 0.0f, 1.0f);
    }

    for (int t = 0; t < 11; t++) {
        for (int s = 0; s < 2; s++) {
            const u32 car = ld32(tl + 0x1AC8 + (u32)(t * 12 + s * 4));
            if (!car || !mem::readable(car + 0xCC, 4)) continue;
            const s16 best = (s16)ld16(car + 0xC4 + 4);
            if (best <= 0) continue;
            bool is_human = (ld32(car + 0x64) & 2u) != 0;
            if (!is_human) {
                const u32 slot = ld32(car + 0x6C);
                if (slot < 22 && mem::readable(tl + 0x1A5C + 4 * slot, 4) && ld32(tl + 0x1A5C + 4 * slot) == 0)
                    is_human = true;
            }
            if (is_human) {
                if (cur.human_best_ticks == 0 || best < cur.human_best_ticks) cur.human_best_ticks = best;
            } else {
                if (cur.ai_best_ticks == 0 || best < cur.ai_best_ticks) {
                    cur.ai_best_ticks = best;
                    cur.ai_best_team = t;
                }
            }
        }
    }

    // Log whenever AI best lap or player best lap improves (useful for A/B benchmarks!)
    static int s_logged_ai = 0, s_logged_hum = 0;
    if (!cur.in_session || (cur.ai_best_ticks == 0 && cur.human_best_ticks == 0)) {
        s_logged_ai = s_logged_hum = 0;
    } else if (cur.ai_best_ticks != s_logged_ai || cur.human_best_ticks != s_logged_hum) {
        s_logged_ai = cur.ai_best_ticks;
        s_logged_hum = cur.human_best_ticks;
        const char* tag = (cur.ai_best_team >= 0 && cur.ai_best_team < 11) ? kTeamTag[cur.ai_best_team] : "---";
        std::string ai_s = fmt_lap_time(cur.ai_best_ticks, cur.dt);
        std::string hu_s = fmt_lap_time(cur.human_best_ticks, cur.dt);
        if (cur.ai_best_ticks > 0 && cur.human_best_ticks > 0) {
            const double delta = (double)(cur.human_best_ticks - cur.ai_best_ticks) * (double)cur.dt;
            LOG_I("laptimes: [wet %.0f%%] AI FL %s (%s) | YOU %s (%+.3fs)",
                  cur.wetness * 100.0f, ai_s.c_str(), tag, hu_s.c_str(), delta);
        } else {
            LOG_I("laptimes: [wet %.0f%%] AI FL %s (%s) | YOU %s",
                  cur.wetness * 100.0f, ai_s.c_str(), tag, hu_s.c_str());
        }
    }
    g_lap = cur;
}

void draw_laptimes(ImDrawList* dl, float W, float H) {
    if (!g_lap.in_session) return;
    const float S = std::min(W / 1280.0f, H / 720.0f);
    const ImU32 GREEN = IM_COL32(124, 224, 138, 255);
    const ImU32 AMBER = IM_COL32(244, 198, 96, 255);
    const ImU32 SILVER = IM_COL32(220, 224, 232, 255);

    const std::string ai_t = fmt_lap_time(g_lap.ai_best_ticks, g_lap.dt);
    const std::string hu_t = fmt_lap_time(g_lap.human_best_ticks, g_lap.dt);
    const char* tag = (g_lap.ai_best_team >= 0 && g_lap.ai_best_team < 11) ? kTeamTag[g_lap.ai_best_team] : "AI";
    char delta_buf[24] = "";
    ImU32 delta_col = SILVER;
    if (g_lap.ai_best_ticks > 0 && g_lap.human_best_ticks > 0) {
        const double d = (double)(g_lap.human_best_ticks - g_lap.ai_best_ticks) * (double)g_lap.dt;
        snprintf(delta_buf, sizeof(delta_buf), "%+.2f", d);
        delta_col = d <= 0.0 ? GREEN : AMBER;
    }

    // 2006 F1 CE "Melhor Volta" broadcast timing tower style
    const float sz_hdr = 12.5f * S, sz_tag = 13 * S, sz_val = 15.5f * S, sz_sub = 13 * S;
    const float hdr_h = 19 * S, row_h = 22 * S, cell_w = 44 * S;
    const float bw = 204 * S, bh = hdr_h + row_h * 2;
    const float x1 = W - 14 * S, x0 = x1 - bw;
    // Positioned below the game's native top-right lap timer so they never overlap
    const float y0 = 92 * S, y1 = y0 + bh;
    const float r1_y0 = y0 + hdr_h, r1_y1 = r1_y0 + row_h;
    const float r2_y0 = r1_y1, r2_y1 = y1;

    // Header bar: 2006 FOM steel-blue metallic gradient ("FASTEST LAP")
    f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x1, r1_y0), IM_COL32(108, 118, 144, 236), IM_COL32(72, 79, 100, 240));
    const float hw = text_w(F.medium, sz_hdr, "FASTEST LAP");
    f1_shadow_text(dl, F.medium, sz_hdr, ImVec2(x0 + (bw - hw) * 0.5f, y0 + 3 * S), WHITE, "FASTEST LAP", S);

    // Row 1 (AI Best): classic 2006 grey index cell + dark translucent timing cell
    f1_grad_rect(dl, ImVec2(x0, r1_y0), ImVec2(x0 + cell_w, r1_y1), IM_COL32(118, 124, 138, 236), IM_COL32(84, 89, 102, 240));
    f1_grad_rect(dl, ImVec2(x0 + cell_w, r1_y0), ImVec2(x1, r1_y1), IM_COL32(34, 37, 44, 226), IM_COL32(16, 18, 22, 236));
    const float tw1 = text_w(F.medium, sz_tag, tag);
    f1_shadow_text(dl, F.medium, sz_tag, ImVec2(x0 + (cell_w - tw1) * 0.5f, r1_y0 + 4 * S), WHITE, tag, S);
    f1_shadow_text(dl, F.medium, sz_val, ImVec2(x0 + cell_w + 10 * S, r1_y0 + 2.5f * S), WHITE, ai_t, S);
    const float aiw = text_w(F.medium, sz_sub, "AI");
    f1_shadow_text(dl, F.medium, sz_sub, ImVec2(x1 - 8 * S - aiw, r1_y0 + 4 * S), SILVER, "AI", S);

    // Row 2 (Player Best): metallic white driver cell + dark translucent timing cell + delta
    f1_grad_rect(dl, ImVec2(x0, r2_y0), ImVec2(x0 + cell_w, r2_y1), IM_COL32(244, 246, 250, 242), IM_COL32(206, 211, 220, 242));
    f1_grad_rect(dl, ImVec2(x0 + cell_w, r2_y0), ImVec2(x1, r2_y1), IM_COL32(28, 30, 36, 228), IM_COL32(12, 14, 18, 238));
    const float tw2 = text_w(F.medium, sz_tag, "YOU");
    text(dl, F.medium, sz_tag, ImVec2(x0 + (cell_w - tw2) * 0.5f, r2_y0 + 4 * S), IM_COL32(22, 25, 32, 255), "YOU");
    f1_shadow_text(dl, F.medium, sz_val, ImVec2(x0 + cell_w + 10 * S, r2_y0 + 2.5f * S), WHITE, hu_t, S);
    if (*delta_buf) {
        const float dw = text_w(F.medium, sz_sub, delta_buf);
        f1_shadow_text(dl, F.medium, sz_sub, ImVec2(x1 - 8 * S - dw, r2_y0 + 4 * S), delta_col, delta_buf, S);
    }

    // Crisp 1 px internal dividers + metallic border
    dl->AddLine(ImVec2(x0, r1_y0), ImVec2(x1, r1_y0), IM_COL32(18, 20, 26, 210), 1.0f);
    dl->AddLine(ImVec2(x0, r2_y0), ImVec2(x1, r2_y0), IM_COL32(18, 20, 26, 210), 1.0f);
    dl->AddLine(ImVec2(x0 + cell_w, r1_y0), ImVec2(x0 + cell_w, y1), IM_COL32(18, 20, 26, 200), 1.0f);
    f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);
}

// ------------------------------------------------------------------ Steward / Penalty banner (hle_penalties.cpp)
PenaltyBanner g_pen;
std::atomic<bool> g_pen_active{false};
std::atomic<u64> g_pen_expires{0};

void draw_penalty_banner(ImDrawList* dl, float W, float H) {
    if (!g_pen.active || g_pen.title.empty()) return;
    const float S = std::min(W / 1280.0f, H / 720.0f);

    // 2006 F1 FIA Race Control two-bar header graphic (matches the game's Grand Prix title + subtitle banner)
    ImU32 hdr_top = IM_COL32(246, 247, 250, 244), hdr_bot = IM_COL32(210, 215, 224, 244);
    ImU32 title_col = IM_COL32(20, 23, 30, 255);
    ImU32 sub_top = IM_COL32(96, 104, 126, 236), sub_bot = IM_COL32(62, 68, 86, 240);
    ImU32 flag_col = IM_COL32(236, 192, 42, 255);
    bool dark_title = true;

    switch (g_pen.severity) {
        case PenaltySeverity::Info:
            flag_col = IM_COL32(165, 174, 190, 255);
            break;
        case PenaltySeverity::Warning:
            flag_col = IM_COL32(240, 196, 40, 255);   // FIA yellow warning flag
            break;
        case PenaltySeverity::FinalWarning:
            flag_col = IM_COL32(235, 118, 32, 255);   // FIA black-and-white / final warning
            sub_top = IM_COL32(82, 86, 102, 238);
            sub_bot = IM_COL32(50, 53, 66, 242);
            break;
        case PenaltySeverity::DriveThrough:
            hdr_top = IM_COL32(194, 40, 40, 244);     // Classic F1 crimson broadcast alert bar
            hdr_bot = IM_COL32(142, 24, 24, 246);
            title_col = WHITE;
            dark_title = false;
            sub_top = IM_COL32(42, 45, 54, 236);
            sub_bot = IM_COL32(20, 22, 28, 242);
            flag_col = IM_COL32(245, 245, 248, 255);
            break;
        case PenaltySeverity::Serving:
            hdr_top = IM_COL32(92, 116, 154, 242);
            hdr_bot = IM_COL32(60, 78, 110, 244);
            title_col = WHITE;
            dark_title = false;
            sub_top = IM_COL32(36, 40, 48, 234);
            sub_bot = IM_COL32(18, 20, 26, 240);
            flag_col = IM_COL32(110, 208, 242, 255);
            break;
        case PenaltySeverity::Served:
            flag_col = IM_COL32(72, 196, 102, 255);   // Green flag cleared
            break;
        case PenaltySeverity::BlackFlag:
            hdr_top = IM_COL32(36, 38, 44, 248);      // Solid FIA Black Flag header
            hdr_bot = IM_COL32(14, 15, 18, 250);
            title_col = WHITE;
            dark_title = false;
            sub_top = IM_COL32(158, 32, 32, 240);
            sub_bot = IM_COL32(114, 20, 20, 244);
            flag_col = IM_COL32(18, 18, 20, 255);
            break;
    }

    const float sz_t = 15.5f * S, sz_s = 13 * S;
    const float flag_w = 24 * S;
    const float tw = text_w(F.medium, sz_t, g_pen.title.c_str());
    const float sw = g_pen.subtitle.empty() ? 0.0f : text_w(F.medium, sz_s, g_pen.subtitle.c_str());
    const float bw = std::max(std::max(tw + flag_w + 26 * S, sw + 28 * S), 268 * S);
    const float hdr_h = 25 * S, sub_h = g_pen.subtitle.empty() ? 0.0f : 22 * S;
    const float bh = hdr_h + sub_h;
    const float x0 = (W - bw) * 0.5f, x1 = x0 + bw;
    const float y0 = 16 * S, y1 = y0 + bh;

    // Top bar (Title + FIA flag square on left)
    f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x1, y0 + hdr_h), hdr_top, hdr_bot);
    // Small crisp FIA status flag badge on the left of the header bar
    const float fx0 = x0 + 6 * S, fy0 = y0 + 5 * S, fx1 = x0 + flag_w - 4 * S, fy1 = y0 + hdr_h - 5 * S;
    if (g_pen.severity == PenaltySeverity::FinalWarning) {
        // Authentic FIA black-and-white diagonal unsportsmanlike-conduct warning flag!
        dl->AddTriangleFilled(ImVec2(fx0, fy0), ImVec2(fx1, fy0), ImVec2(fx0, fy1), IM_COL32(246, 246, 248, 255));
        dl->AddTriangleFilled(ImVec2(fx1, fy0), ImVec2(fx1, fy1), ImVec2(fx0, fy1), IM_COL32(22, 24, 28, 255));
    } else {
        dl->AddRectFilled(ImVec2(fx0, fy0), ImVec2(fx1, fy1), flag_col);
    }
    dl->AddRect(ImVec2(fx0, fy0), ImVec2(fx1, fy1),
                g_pen.severity == PenaltySeverity::BlackFlag ? IM_COL32(230, 232, 238, 220) : IM_COL32(24, 26, 32, 170),
                0.0f, 0, 1.0f);

    if (dark_title) {
        text(dl, F.medium, sz_t, ImVec2(x0 + flag_w + 6 * S, y0 + 4.5f * S), title_col, g_pen.title);
    } else {
        f1_shadow_text(dl, F.medium, sz_t, ImVec2(x0 + flag_w + 6 * S, y0 + 4.5f * S), title_col, g_pen.title, S);
    }

    // Bottom bar (Subtitle)
    if (!g_pen.subtitle.empty()) {
        f1_grad_rect(dl, ImVec2(x0, y0 + hdr_h), ImVec2(x1, y1), sub_top, sub_bot);
        dl->AddLine(ImVec2(x0, y0 + hdr_h), ImVec2(x1, y0 + hdr_h), IM_COL32(18, 20, 26, 210), 1.0f);
        f1_shadow_text(dl, F.medium, sz_s, ImVec2(x0 + 10 * S, y0 + hdr_h + 4 * S), IM_COL32(240, 242, 248, 255), g_pen.subtitle, S);
    }

    f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);
}

// ------------------------------------------------------------------ On-track MFD (hle_mfd.cpp: 4-way Pit Tyre MFD)
MfdState g_mfd_ov;
std::atomic<bool> g_mfd_visible{false};
std::atomic<int> g_mfd_tyre_code{0};
std::atomic<u64> g_mfd_popup_until{0};
std::atomic<u64> g_mfd_aid_until{0};

ImU32 mfd_tyre_color(int code) {
    switch (code) {
        case 1: return IM_COL32(240, 242, 246, 255);   // HARD: crisp white
        case 2: return IM_COL32(246, 200, 56, 255);    // SOFT: F1 gold/yellow
        case 3: return IM_COL32(78, 206, 112, 255);    // INTER: green
        case 4: return IM_COL32(74, 168, 244, 255);    // WET: blue
        default: return IM_COL32(174, 182, 198, 255);  // AUTO: metallic silver
    }
}

void draw_mfd(ImDrawList* dl, float W, float H, u64 now, bool lap_on) {
    if (!g_mfd_ov.visible) return;
    const bool expanded = g_mfd_ov.popup_until_us != 0 && now < g_mfd_ov.popup_until_us;
    const bool pinned_tyre = g_mfd_ov.tyre_code != 0;
    const bool aid_toast = g_mfd_ov.aid_until_us != 0 && now < g_mfd_ov.aid_until_us && !g_mfd_ov.aid_tag.empty();
    if (!expanded && !pinned_tyre && !aid_toast) return;

    const float S = std::min(W / 1280.0f, H / 720.0f);
    const ImU32 GREEN = IM_COL32(124, 224, 138, 255);
    const ImU32 AMBER = IM_COL32(244, 198, 96, 255);
    const ImU32 SILVER = IM_COL32(218, 222, 232, 255);
    const ImU32 tyre_col = mfd_tyre_color(g_mfd_ov.tyre_code);

    // Position on the right edge below the native lap timer (and below F4 Fastest Lap when active)
    const float bw = 204 * S;
    const float x1 = W - 14 * S, x0 = x1 - bw;
    float cur_y = (lap_on ? 164.0f : 92.0f) * S;

    if (expanded) {
        // Expanded 4-way 2006 F1 broadcast Pit Tyre MFD card (UP=SOFT, DN=HARD, LT=INTER, RT=WET)
        const float sz_hdr = 12.5f * S, sz_dir = 12.0f * S, sz_val = 14.0f * S, sz_note = 11.5f * S;
        const float hdr_h = 19 * S, row_h = 21 * S, cell_w = 42 * S;
        const float note_h = g_mfd_ov.note.empty() ? 0.0f : 18 * S;
        const float y0 = cur_y, y1 = y0 + hdr_h + row_h * 4 + note_h;

        // Header bar: 2006 FOM steel-blue metallic gradient
        f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x1, y0 + hdr_h), IM_COL32(108, 118, 144, 238), IM_COL32(72, 79, 100, 242));
        const float hw = text_w(F.medium, sz_hdr, "PIT TYRES");
        f1_shadow_text(dl, F.medium, sz_hdr, ImVec2(x0 + (bw - hw) * 0.5f, y0 + 3 * S), WHITE, "PIT TYRES", S);

        struct TyreRow { int code; const char* dir; const char* name; };
        constexpr TyreRow rows[4] = {
            {2, "UP", "SOFT"},
            {1, "DN", "HARD"},
            {3, "LT", "INTER"},
            {4, "RT", "WET"},
        };
        for (int i = 0; i < 4; i++) {
            const float ry0 = y0 + hdr_h + i * row_h;
            const float ry1 = ry0 + row_h;
            const bool active = (g_mfd_ov.tyre_code == rows[i].code);
            const ImU32 col = mfd_tyre_color(rows[i].code);
            // Left direction cell: metallic white when selected, steel-grey otherwise
            if (active) f1_grad_rect(dl, ImVec2(x0, ry0), ImVec2(x0 + cell_w, ry1), IM_COL32(244, 246, 250, 244), IM_COL32(206, 211, 220, 244));
            else f1_grad_rect(dl, ImVec2(x0, ry0), ImVec2(x0 + cell_w, ry1), IM_COL32(118, 124, 138, 236), IM_COL32(84, 89, 102, 240));
            // Right compound cell
            if (active) f1_grad_rect(dl, ImVec2(x0 + cell_w, ry0), ImVec2(x1, ry1), IM_COL32(46, 52, 64, 238), IM_COL32(24, 27, 36, 244));
            else f1_grad_rect(dl, ImVec2(x0 + cell_w, ry0), ImVec2(x1, ry1), IM_COL32(28, 30, 36, 224), IM_COL32(12, 14, 18, 236));
            // Colored compound stripe on the right edge
            dl->AddRectFilled(ImVec2(x1 - (active ? 6.0f : 3.5f) * S, ry0 + 2 * S), ImVec2(x1 - 1 * S, ry1 - 2 * S), col);

            const float dw = text_w(F.medium, sz_dir, rows[i].dir);
            if (active) text(dl, F.medium, sz_dir, ImVec2(x0 + (cell_w - dw) * 0.5f, ry0 + 4.5f * S), IM_COL32(22, 25, 32, 255), rows[i].dir);
            else f1_shadow_text(dl, F.medium, sz_dir, ImVec2(x0 + (cell_w - dw) * 0.5f, ry0 + 4.5f * S), WHITE, rows[i].dir, S);

            f1_shadow_text(dl, F.medium, sz_val, ImVec2(x0 + cell_w + 10 * S, ry0 + 3 * S), active ? col : SILVER, rows[i].name, S);
            if (active) {
                const float sw = text_w(F.medium, sz_note, "NEXT");
                f1_shadow_text(dl, F.medium, sz_note, ImVec2(x1 - 11 * S - sw, ry0 + 4.5f * S), col, "NEXT", S);
            }
            dl->AddLine(ImVec2(x0, ry0), ImVec2(x1, ry0), IM_COL32(18, 20, 26, 210), 1.0f);
        }
        dl->AddLine(ImVec2(x0 + cell_w, y0 + hdr_h), ImVec2(x0 + cell_w, y0 + hdr_h + row_h * 4), IM_COL32(18, 20, 26, 200), 1.0f);

        if (!g_mfd_ov.note.empty()) {
            const float ny0 = y0 + hdr_h + row_h * 4;
            f1_grad_rect(dl, ImVec2(x0, ny0), ImVec2(x1, y1), IM_COL32(62, 68, 86, 236), IM_COL32(42, 46, 60, 240));
            dl->AddLine(ImVec2(x0, ny0), ImVec2(x1, ny0), IM_COL32(18, 20, 26, 210), 1.0f);
            const float nw = text_w(F.medium, sz_note, g_mfd_ov.note.c_str());
            f1_shadow_text(dl, F.medium, sz_note, ImVec2(x0 + (bw - nw) * 0.5f, ny0 + 3 * S), SILVER, g_mfd_ov.note, S);
        }
        f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);
        cur_y = y1 + 6 * S;
    } else if (pinned_tyre) {
        // Compact 1-row 2006 F1 reminder badge when a non-AUTO pit tyre is queued
        const float bh = 23 * S, tag_w = 64 * S;
        const float y0 = cur_y, y1 = y0 + bh;
        f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x0 + tag_w, y1), IM_COL32(242, 244, 248, 240), IM_COL32(204, 209, 218, 240));
        f1_grad_rect(dl, ImVec2(x0 + tag_w, y0), ImVec2(x1, y1), IM_COL32(30, 33, 40, 228), IM_COL32(14, 16, 20, 238));
        dl->AddRectFilled(ImVec2(x1 - 5 * S, y0 + 2 * S), ImVec2(x1 - 1 * S, y1 - 2 * S), tyre_col);
        dl->AddLine(ImVec2(x0 + tag_w, y0), ImVec2(x0 + tag_w, y1), IM_COL32(18, 20, 26, 200), 1.0f);

        const float sz_tag = 12.5f * S, sz_val = 14.5f * S;
        const float tw = text_w(F.medium, sz_tag, "NEXT PIT");
        text(dl, F.medium, sz_tag, ImVec2(x0 + (tag_w - tw) * 0.5f, y0 + 4.5f * S), IM_COL32(22, 25, 32, 255), "NEXT PIT");
        f1_shadow_text(dl, F.medium, sz_val, ImVec2(x0 + tag_w + 10 * S, y0 + 3.5f * S), tyre_col, g_mfd_ov.tyre_label, S);
        f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);
        cur_y = y1 + 6 * S;
    }

    if (aid_toast) {
        // Optional keyboard-only TC / ABS quick toast (F6 / F7)
        const float bh = 23 * S, tag_w = 48 * S;
        const float y0 = cur_y, y1 = y0 + bh;
        f1_grad_rect(dl, ImVec2(x0, y0), ImVec2(x0 + tag_w, y1), IM_COL32(242, 244, 248, 240), IM_COL32(204, 209, 218, 240));
        f1_grad_rect(dl, ImVec2(x0 + tag_w, y0), ImVec2(x1, y1), IM_COL32(30, 33, 40, 228), IM_COL32(14, 16, 20, 238));
        dl->AddLine(ImVec2(x0 + tag_w, y0), ImVec2(x0 + tag_w, y1), IM_COL32(18, 20, 26, 200), 1.0f);

        const float sz_tag = 12.5f * S, sz_val = 14.5f * S;
        const float tw = text_w(F.medium, sz_tag, g_mfd_ov.aid_tag.c_str());
        text(dl, F.medium, sz_tag, ImVec2(x0 + (tag_w - tw) * 0.5f, y0 + 4.5f * S), IM_COL32(22, 25, 32, 255), g_mfd_ov.aid_tag);
        f1_shadow_text(dl, F.medium, sz_val, ImVec2(x0 + tag_w + 10 * S, y0 + 3.5f * S), g_mfd_ov.aid_green ? GREEN : AMBER, g_mfd_ov.aid_val, S);
        f1_frame(dl, ImVec2(x0, y0), ImVec2(x1, y1), S);
    }
}

// ------------------------------------------------------------------ frame
Target g_target;
std::unordered_map<ImTextureData*, SoftTex> g_texes;   // ImGui's textures (the font atlas): their pixels as ImGui keeps them
u64 g_hash = 0, g_image_serial = 0;

void imgui_init() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendRendererName = "f1ce_soft";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    auto add = [&](const unsigned char* data, unsigned size) {
        ImFontConfig c;
        c.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF((void*)data, (int)size, 20.0f, &c);
    };
    F.black = add(font_black_italic, font_black_italic_size);
    F.semi = add(font_semibold_italic, font_semibold_italic_size);
    F.medium = add(font_medium, font_medium_size);
}
}  // namespace

void toggle_fps() {
    const bool now_on = !g_fps_on.load();
    g_fps_on.store(now_on);
    LOG_I("overlay: FPS counter %s", now_on ? "on" : "off");
}

void toggle_laptimes() {
    const bool now_on = !g_lap_on.load();
    g_lap_on.store(now_on);
    LOG_I("overlay: fastest lap overlay %s", now_on ? "on" : "off");
}

void set_penalty_banner(const PenaltyBanner& b) {
    std::lock_guard lk(g_m);
    g_pen = b;
    g_pen_expires.store(b.expires_at_us);
    g_pen_active.store(b.active && !b.title.empty());
}

void set_mfd_state(const MfdState& s) {
    std::lock_guard lk(g_m);
    g_mfd_ov = s;
    g_mfd_tyre_code.store(s.tyre_code);
    g_mfd_popup_until.store(s.popup_until_us);
    g_mfd_aid_until.store(s.aid_until_us);
    g_mfd_visible.store(s.visible);
}

bool frame(u32 win_w, u32 win_h, Image* out) {
    static u64 last = 0;
    const u64 now = time_usec();
    g_last_frame = now;
    update_fps(now);
    sample_laptimes(now);
    if (!win_w || !win_h) return false;
    if (g_bar_active.load() && now - g_bar_at.load() > 500000) {   // no update: the race loop was left (menus)
        std::lock_guard lk(g_m);
        g_bar = {};
        g_bar_active = false;
        g_bar_input = {};
    }
    const bool fps_on = g_fps_on.load();
    const bool lap_on = g_lap_on.load() && g_lap.in_session;
    const u64 pen_exp = g_pen_expires.load();
    const bool pen_was_active = g_pen_active.load();
    bool pen_on = pen_was_active && g_lap.in_session && (!pen_exp || now < pen_exp);
    if (pen_was_active && (!g_lap.in_session || (pen_exp && now >= pen_exp))) {
        std::lock_guard lk(g_m);
        g_pen = {};
        g_pen_active = false;
        g_pen_expires = 0;
        pen_on = false;
    }
    const u64 mfd_until = g_mfd_popup_until.load();
    const u64 mfd_aid_until = g_mfd_aid_until.load();
    const bool mfd_vis = g_mfd_visible.load() && g_lap.in_session;
    const bool mfd_on = mfd_vis && ((mfd_until && now < mfd_until) || (mfd_aid_until && now < mfd_aid_until) || g_mfd_tyre_code.load() != 0);
    if (g_mfd_visible.load() && !g_lap.in_session) {
        std::lock_guard lk(g_m);
        g_mfd_ov = {};
        g_mfd_visible = false;
        g_mfd_tyre_code = 0;
        g_mfd_popup_until = 0;
        g_mfd_aid_until = 0;
    }
    if (!g_open.load() && !g_bar_active.load() && !fps_on && !lap_on && !pen_on && !mfd_on) return false;
    std::lock_guard lk(g_m);
    static bool inited = false;
    bool dialog = g_open.load() && g_st.kind != Kind::None;
    if (dialog) {
        if (!inited) { imgui_init(); inited = true; }
        update(pressed(g_st.serial));
        dialog = g_st.kind != Kind::None;   // (answered)
    }
    bool show_bar = false;
    if (!dialog && g_bar_active.load()) {
        if (!inited) { imgui_init(); inited = true; }   // (the layout measures text)
        bar_mouse((float)win_w, (float)win_h);
        show_bar = bar_visible((float)win_w, (float)win_h);
    }
    if (!dialog && !show_bar && !fps_on && !lap_on && !pen_on && !mfd_on) return false;
    if (!inited) { imgui_init(); inited = true; }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)win_w, (float)win_h);
    io.DeltaTime = last && now > last ? std::min((now - last) / 1e6f, 0.25f) : 1.0f / 60.0f;
    last = now;
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dialog) draw(dl, (float)win_w, (float)win_h);
    else if (show_bar) draw_bar(dl, (float)win_w, (float)win_h);
    if (fps_on) draw_fps(dl, (float)win_w, (float)win_h);
    if (lap_on && !dialog) draw_laptimes(dl, (float)win_w, (float)win_h);
    if (mfd_on && !dialog) draw_mfd(dl, (float)win_w, (float)win_h, now, lap_on);
    if (pen_on && !dialog) draw_penalty_banner(dl, (float)win_w, (float)win_h);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    bool tex_changed = false;
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                SoftTex& st = g_texes[tex];
                st = {tex->Width, tex->Height, (const u8*)tex->Pixels};   // (RGBA32, ImGui's default format)
                tex->SetTexID((ImTextureID)(uintptr_t)&st);
                tex->SetStatus(ImTextureStatus_OK);
                tex_changed = true;
            } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
                g_texes.erase(tex);
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    const u64 h = hash_draw_data(dd);
    if (h != g_hash || tex_changed || g_target.w != win_w || g_target.h != win_h) {
        g_target.w = win_w;
        g_target.h = win_h;
        g_target.px.resize((size_t)win_w * win_h);
        raster(g_target, dd);
        g_hash = h;
        g_image_serial++;
    }
    out->w = g_target.w;
    out->h = g_target.h;
    out->px = g_target.px.data();
    out->serial = g_image_serial;
    return true;
}

bool input_captured() {
    if (g_open.load()) return true;
    if (!g_captured.load()) return false;
    const host::PadState p = host::pad_state();
    const host::WheelInput w = host::wheel_input();
    if (p.digital1 || p.digital2 || (w.connected && w.buttons)) return true;
    g_captured = false;
    return false;
}

int save_list(SaveMode mode, const std::vector<SaveEntry>& entries, int focus) {
    State st;
    st.kind = Kind::List;
    st.mode = mode;
    st.items.resize(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        Item& it = st.items[i];
        it.e = entries[i];
        decode_icon(it);
        it.when = format_time(it.e.mtime);
        it.size = format_size(it.e.size);
    }
    st.sel = std::clamp(focus, 0, std::max(0, (int)entries.size() - 1));
    return run(std::move(st));
}

bool message(const std::string& text) {
    State st;
    st.kind = Kind::Message;
    st.text = text;
    return run(std::move(st)) != UNAVAILABLE;
}

void set_replay_bar(const ReplayBar& bar) {
    std::lock_guard lk(g_m);
    g_bar = bar;
    g_bar_active = bar.active;
    g_bar_at = time_usec();
    if (!bar.active) g_bar_input = {};
}

ReplayBarInput take_replay_input() {
    std::lock_guard lk(g_m);
    const ReplayBarInput in = g_bar_input;
    g_bar_input = {};
    return in;
}
}  // namespace overlay
