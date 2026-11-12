// System dialogs over the game (overlay.h). Laid out with Dear ImGui's draw lists, rasterised here on the CPU (the
// renderers only blend the finished image), so OpenGL and Vulkan share one implementation. The dialog runs on the RSX
// thread, once per presented frame; the save threads wait for its answer.
#include "overlay.h"

#include "host.h"
#include "kernel.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
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

bool frame(u32 win_w, u32 win_h, Image* out) {
    static u64 last = 0;
    const u64 now = time_usec();
    g_last_frame = now;
    if (!g_open.load() || !win_w || !win_h) return false;
    std::lock_guard lk(g_m);
    if (g_st.kind == Kind::None) return false;
    static bool inited = false;
    if (!inited) { imgui_init(); inited = true; }
    update(pressed(g_st.serial));
    if (g_st.kind == Kind::None) return false;   // answered
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)win_w, (float)win_h);
    io.DeltaTime = last && now > last ? std::min((now - last) / 1e6f, 0.25f) : 1.0f / 60.0f;
    last = now;
    ImGui::NewFrame();
    draw(ImGui::GetForegroundDrawList(), (float)win_w, (float)win_h);
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
}  // namespace overlay
