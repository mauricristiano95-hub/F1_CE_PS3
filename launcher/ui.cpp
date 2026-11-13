// The launcher's window (Dear ImGui on SDL3's renderer, Windows and Linux): the game menu's look, heavy italic type
// (Exo 2) and carbon bands on a white body, with sections on the left and one row per setting, made for a gamepad as
// much as for the mouse. Up / down picks a row, left / right changes it, ✕ (A, Enter) opens its list or toggles it;
// L1 / R1 (Tab) switch section, △ (Y, F2) picks the game folder, Start (F5) starts the game, ○ (B, Esc) quits.
// The settings themselves: options.cpp; the Steering Wheel section: wheel_page.cpp.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // (WinMain on Windows)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/imgui-1.93.0/backends/imgui_impl_sdl3.h"
#include "../third_party/imgui-1.93.0/backends/imgui_impl_sdlrenderer3.h"
#include "../third_party/imgui-1.93.0/imgui.h"
#include "../runtime/input_config.h"
#include "ini.h"
#include "input_page.h"
#include "options.h"
#include "ui.h"
#include "version.h"
#include "wheel_page.h"

#include "font_black_italic.h"
#include "font_extrabold_italic.h"
#include "font_medium.h"
#include "font_semibold_italic.h"
#include "logo_png.h"

namespace ui {
// ---------------------------------------------------------------- look
Fonts F;
float S = 1;   // pixels per design unit: the design is 1280x760, scaled to the window (as a game menu)

const ImU32 INK = IM_COL32(27, 27, 27, 255), GREY = IM_COL32(154, 154, 154, 255), RED = IM_COL32(211, 47, 47, 255),
            HELP = IM_COL32(95, 95, 95, 255), WHITE = IM_COL32(255, 255, 255, 255);

namespace {
SDL_Window* g_window;
SDL_Renderer* g_renderer;
SDL_Texture *g_logo, *g_carbon;
Ini g_ini;
Uint32 g_exit_event;
bool g_quit = false, g_game_running = false;

int g_page = opts::GENERAL;
int g_focus[opts::PAGES] = {};   // the focused row of each section (index among its focusable rows)
bool g_focus_request = true;     // focus g_focus[g_page] (no row has the focus: a section change, a click outside)
bool g_modal_last = false;       // a list or a text edit was open last frame (its ○ / Esc is not Quit)
std::string g_help;              // the focused (else hovered) row's description
const char* g_help_hover = nullptr;

// last input device, for the button prompts
enum Prompts { KEYBOARD, PLAYSTATION, XBOX } g_prompts = KEYBOARD;

// the folder picker answers from another thread
std::mutex g_folder_m;
std::string g_folder_pick;
bool g_folder_new = false;

// the open choice list / text edit, under / over the row that opened it
struct List { std::vector<std::string> items; int sel; std::function<void(int)> pick; ImVec2 pos; float w; };
struct Edit { std::function<void(const std::string&)> done; ImVec2 pos; float w; };
List g_list;
Edit g_edit;
bool g_list_on = false, g_list_open_request = false, g_edit_on = false, g_edit_open_request = false;
char g_edit_buf[1024];

// waiting for a key / pad input to bind (input_page.cpp)
struct CaptureState { bool on = false, armed = false; Capture kind; int tag = -1; std::function<void(int)> done; Uint64 t0 = 0; };
CaptureState g_cap;
bool g_pad_quiet = false;   // after a pad capture: pad navigation off until its inputs are let go

std::string upper(const std::string& s) {
    std::string u = s;
    for (char& c : u)
        if ((unsigned char)c < 0x80) c = (char)toupper((unsigned char)c);
    return u;
}

// ---------------------------------------------------------------- textures
// carbon fibre: a 2x2 twill tile, horizontal and vertical tows in a checker, each lit across its width
void make_carbon() {
    constexpr int N = 32, H = N / 2;
    Uint32 px[N * N];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const bool horiz = ((x / H) + (y / H)) % 2 == 0;
            const float t = (horiz ? (y % H) : (x % H)) / float(H - 1);
            const float lit = std::sin(t * 3.14159f);   // 0 at the tow's edges, 1 in its middle
            const int v = horiz ? (int)(22 + 26 * lit) : (int)(14 + 14 * lit);
            px[y * N + x] = 0xff000000u | (Uint32)v << 16 | (Uint32)v << 8 | (Uint32)v;
        }
    g_carbon = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    SDL_UpdateTexture(g_carbon, nullptr, px, N * 4);
    SDL_SetTextureScaleMode(g_carbon, SDL_SCALEMODE_LINEAR);
}
void make_logo() {
    SDL_Surface* s = SDL_LoadPNG_IO(SDL_IOFromConstMem(logo_png, logo_png_size), true);
    if (!s) return;
    g_logo = SDL_CreateTextureFromSurface(g_renderer, s);
    SDL_SetTextureScaleMode(g_logo, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(s);
}
ImTextureRef tex(SDL_Texture* t) { return ImTextureRef((ImTextureID)(intptr_t)t); }
}  // namespace

// carbon over a rectangle (tiles of 16 design units), with a gloss from the top
void carbon(ImDrawList* dl, ImVec2 a, ImVec2 b, float gloss) {
    dl->AddRectFilled(a, b, IM_COL32(22, 22, 22, 255));
    dl->PushClipRect(a, b, true);
    const float t = 16 * S;
    for (float y = a.y; y < b.y; y += t)
        for (float x = a.x; x < b.x; x += t) dl->AddImage(tex(g_carbon), ImVec2(x, y), ImVec2(x + t, y + t));
    const float mid = a.y + (b.y - a.y) * 0.45f;
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, mid), IM_COL32(255, 255, 255, (int)(26 * gloss)),
                                IM_COL32(255, 255, 255, (int)(26 * gloss)), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
    dl->AddRectFilledMultiColor(ImVec2(a.x, mid), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90),
                                IM_COL32(0, 0, 0, 90));
    dl->PopClipRect();
}

void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& s, float wrap) {
    dl->AddText(f, size * S, p, col, s.c_str(), nullptr, wrap);
}
float text_w(ImFont* f, float size, const std::string& s) {
    return f->CalcTextSizeA(size * S, FLT_MAX, 0, s.c_str()).x;
}

namespace {
// the chrome edge under / over a carbon band
void chrome(ImDrawList* dl, float y, float w, bool top) {
    const float h = 10 * S;
    const ImU32 c0 = IM_COL32(140, 140, 140, 255), c1 = IM_COL32(244, 244, 244, 255), c2 = IM_COL32(181, 181, 181, 255),
                c3 = IM_COL32(233, 233, 233, 255);
    const ImU32 a = top ? c0 : c3, b = top ? c1 : c2, c = top ? c2 : c1, d = top ? c3 : c0;
    dl->AddRectFilledMultiColor(ImVec2(0, y), ImVec2(w, y + h * 0.45f), a, a, b, b);
    dl->AddRectFilledMultiColor(ImVec2(0, y + h * 0.45f), ImVec2(w, y + h * 0.75f), b, b, c, c);
    dl->AddRectFilledMultiColor(ImVec2(0, y + h * 0.75f), ImVec2(w, y + h), c, c, d, d);
}

// a chevron (two arms), pointing right
void chevron(ImDrawList* dl, ImVec2 o, float w, float h, ImU32 col) {
    const ImVec2 up[4] = {o, ImVec2(o.x + w * 0.45f, o.y), ImVec2(o.x + w, o.y + h / 2), ImVec2(o.x + w * 0.55f, o.y + h / 2)};
    const ImVec2 dn[4] = {ImVec2(o.x + w * 0.55f, o.y + h / 2), ImVec2(o.x + w, o.y + h / 2), ImVec2(o.x + w * 0.45f, o.y + h),
                          ImVec2(o.x, o.y + h)};
    dl->AddConvexPolyFilled(up, 4, col);
    dl->AddConvexPolyFilled(dn, 4, col);
}

// ---------------------------------------------------------------- button prompts
enum Action { A_SECTION, A_CHANGE, A_SELECT, A_FOLDER, A_QUIT, A_START };
// draws the prompt at p (vertically centred on p.y); returns its width
float prompt(ImDrawList* dl, Action a, ImVec2 p) {
    const float r = 13 * S;
    auto cap = [&](const char* t, ImU32 border, ImU32 col) {   // a key cap / shoulder button
        const float w = std::max(text_w(F.semi, 13, t) + 12 * S, 2 * r);
        dl->AddRect(ImVec2(p.x, p.y - r * 0.8f), ImVec2(p.x + w, p.y + r * 0.8f), border, 4 * S, 0, 2 * S);
        text(dl, F.semi, 13, ImVec2(p.x + (w - text_w(F.semi, 13, t)) / 2, p.y - 8 * S), col, t);
        return w;
    };
    auto ring = [&](ImU32 col) { dl->AddCircle(ImVec2(p.x + r, p.y), r, col, 0, 2 * S); };
    auto ball = [&](ImU32 col, const char* t) {
        dl->AddCircleFilled(ImVec2(p.x + r, p.y), r, col);
        text(dl, F.xbold, 15, ImVec2(p.x + r - text_w(F.xbold, 15, t) / 2, p.y - 10 * S), WHITE, t);
        return 2 * r;
    };
    auto arrows = [&]() {
        const float s = 7 * S;
        dl->AddTriangleFilled(ImVec2(p.x, p.y), ImVec2(p.x + s, p.y - s), ImVec2(p.x + s, p.y + s), RED);
        dl->AddTriangleFilled(ImVec2(p.x + 3 * s, p.y), ImVec2(p.x + 2 * s, p.y - s), ImVec2(p.x + 2 * s, p.y + s), RED);
        return 3 * s;
    };
    const ImU32 cap_c = IM_COL32(158, 158, 158, 255);
    if (a == A_CHANGE) return arrows();
    if (g_prompts == KEYBOARD) {
        switch (a) {
            case A_SECTION: return cap("TAB", cap_c, WHITE);
            case A_SELECT: return cap("ENTER", cap_c, WHITE);
            case A_FOLDER: return cap("F2", cap_c, WHITE);
            case A_QUIT: return cap("ESC", cap_c, WHITE);
            default: return cap("F5", cap_c, WHITE);
        }
    }
    if (g_prompts == XBOX) {
        switch (a) {
            case A_SECTION: { const float w = cap("LB", cap_c, WHITE); p.x += w + 4 * S; return w + 4 * S + cap("RB", cap_c, WHITE); }
            case A_SELECT: return ball(IM_COL32(16, 124, 16, 255), "A");
            case A_FOLDER: return ball(IM_COL32(196, 160, 0, 255), "Y");
            case A_QUIT: return ball(IM_COL32(200, 40, 40, 255), "B");
            default: {   // the menu button: three lines
                const float w = 2 * r;
                dl->AddRect(ImVec2(p.x, p.y - r * 0.8f), ImVec2(p.x + w, p.y + r * 0.8f), cap_c, 4 * S, 0, 2 * S);
                for (int i = -1; i <= 1; i++)
                    dl->AddLine(ImVec2(p.x + 7 * S, p.y + i * 4 * S), ImVec2(p.x + w - 7 * S, p.y + i * 4 * S), WHITE, 2 * S);
                return w;
            }
        }
    }
    switch (a) {   // PlayStation
        case A_SECTION: { const float w = cap("L1", cap_c, WHITE); p.x += w + 4 * S; return w + 4 * S + cap("R1", cap_c, WHITE); }
        case A_SELECT: {
            const ImU32 c = IM_COL32(100, 181, 246, 255);
            ring(c);
            const float k = r * 0.42f;
            dl->AddLine(ImVec2(p.x + r - k, p.y - k), ImVec2(p.x + r + k, p.y + k), c, 2 * S);
            dl->AddLine(ImVec2(p.x + r - k, p.y + k), ImVec2(p.x + r + k, p.y - k), c, 2 * S);
            return 2 * r;
        }
        case A_FOLDER: {
            const ImU32 c = IM_COL32(129, 199, 132, 255);
            ring(c);
            const float k = r * 0.5f;
            dl->AddTriangle(ImVec2(p.x + r, p.y - k), ImVec2(p.x + r + k, p.y + k * 0.7f), ImVec2(p.x + r - k, p.y + k * 0.7f), c, 2 * S);
            return 2 * r;
        }
        case A_QUIT: {
            const ImU32 c = IM_COL32(229, 115, 115, 255);
            ring(c);
            dl->AddCircle(ImVec2(p.x + r, p.y), r * 0.45f, c, 0, 2 * S);
            return 2 * r;
        }
        default: return cap("OPTIONS", cap_c, WHITE);
    }
}

// (drawn far outside the window, clipped away)
float prompt_width(ImDrawList* dl, Action a) { return prompt(dl, a, ImVec2(-10000, -10000)); }

// ---------------------------------------------------------------- rows

bool key_pressed(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return ImGui::IsKeyPressed(a, true) || ImGui::IsKeyPressed(b, true) || ImGui::IsKeyPressed(c, true);
}
}  // namespace

RowOut row_widget(const char* id, const std::string& label, const std::string& value, int index, bool arrows,
                  bool experimental, const char* help, bool* focused_out) {
    RowOut out;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 36 * S, slant = 10 * S;
    if (g_focus_request && index == g_focus[g_page]) {
        ImGui::SetKeyboardFocusHere();
        g_focus_request = false;
    }
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered();
    out.focused = focused;
    if (focused) {
        g_focus[g_page] = index;
        if (focused_out) *focused_out = true;
        g_help = help ? help : "";
    } else if (hovered) {
        g_help_hover = help;
    }
    // value box: right part of the row, arrows at its ends
    const float vw = std::max(250 * S, w * 0.42f), ax0 = p.x + w - vw - 18 * S, ax1 = p.x + w - 18 * S;
    if (pressed) {
        const float mx = ImGui::GetIO().MousePos.x;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && arrows && mx >= ax0 && mx < ax0 + 34 * S) out.delta = -1;
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && arrows && mx >= ax1 - 34 * S && mx < ax1) out.delta = 1;
        else out.activated = true;
    }
    if (focused && arrows && !g_list_on && !g_edit_on) {
        if (key_pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft)) out.delta = -1;
        if (key_pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight)) out.delta = 1;
    }
    // background: a parallelogram, carbon when focused
    const ImVec2 q[4] = {ImVec2(p.x + slant, p.y), ImVec2(p.x + w, p.y), ImVec2(p.x + w - slant, p.y + h), ImVec2(p.x, p.y + h)};
    if (focused) {
        carbon(dl, ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h), 1);
        dl->AddTriangleFilled(ImVec2(p.x, p.y), ImVec2(p.x + slant, p.y), ImVec2(p.x, p.y + h), WHITE);
        dl->AddTriangleFilled(ImVec2(p.x + w, p.y), ImVec2(p.x + w, p.y + h), ImVec2(p.x + w - slant, p.y + h), WHITE);
    } else {
        dl->AddConvexPolyFilled(q, 4, hovered ? IM_COL32(226, 226, 226, 255)
                                      : index % 2 ? IM_COL32(247, 247, 247, 255) : IM_COL32(238, 238, 238, 255));
    }
    const ImU32 fg = focused ? WHITE : INK;
    text(dl, F.xbold, 17, ImVec2(p.x + 22 * S, p.y + 7 * S), fg, upper(label));
    if (experimental) {
        const std::string t = "EXPERIMENTAL";
        const float bx = ax0 - text_w(F.xbold, 12, t) - 34 * S;
        dl->AddRectFilled(ImVec2(bx, p.y + 9 * S), ImVec2(bx + text_w(F.xbold, 12, t) + 16 * S, p.y + 27 * S),
                          IM_COL32(242, 184, 75, 255));
        text(dl, F.xbold, 12, ImVec2(bx + 8 * S, p.y + 10 * S), IM_COL32(26, 19, 0, 255), t);
    }
    if (arrows) {
        const float s = 7 * S, cy = p.y + h / 2;
        dl->AddTriangleFilled(ImVec2(ax0 + 6 * S, cy), ImVec2(ax0 + 6 * S + s * 1.3f, cy - s), ImVec2(ax0 + 6 * S + s * 1.3f, cy + s), RED);
        dl->AddTriangleFilled(ImVec2(ax1 - 6 * S, cy), ImVec2(ax1 - 6 * S - s * 1.3f, cy - s), ImVec2(ax1 - 6 * S - s * 1.3f, cy + s), RED);
    }
    const float vx0 = ax0 + 30 * S, vx1 = ax1 - 30 * S, tw = text_w(F.semi, 18, value);
    dl->PushClipRect(ImVec2(vx0, p.y), ImVec2(vx1, p.y + h), true);
    text(dl, F.semi, 18, ImVec2(tw < vx1 - vx0 ? (vx0 + vx1 - tw) / 2 : vx0, p.y + 6 * S), fg, value);
    dl->PopClipRect();
    return out;
}

void header_widget(const std::string& title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 30 * S));
    text(dl, F.black, 15, ImVec2(p.x + 22 * S, p.y + 9 * S), GREY, upper(title));
}

// the value box of the last row (ImGui's last item)
void value_box(ImVec2& pos, float& w) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    w = std::max(250 * S, (b.x - a.x) * 0.42f);
    pos = ImVec2(b.x - w - 18 * S, b.y);
}
void open_list(std::vector<std::string> items, int sel, std::function<void(int)> pick) {
    g_list = {std::move(items), sel, std::move(pick), {}, 0};
    value_box(g_list.pos, g_list.w);
    g_list_on = g_list_open_request = true;
}
void open_edit(const std::string& text, std::function<void(const std::string&)> done) {
    g_edit = {std::move(done), {}, 0};
    value_box(g_edit.pos, g_edit.w);
    g_edit.pos.y -= 32 * S;
    snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", text.c_str());
    g_edit_on = g_edit_open_request = true;
}

void start_capture(Capture kind, int tag, std::function<void(int)> done) {
    g_cap = {true, false, kind, tag, std::move(done), SDL_GetTicks()};
}
bool capturing() { return g_cap.on; }
int capture_tag() { return g_cap.on ? g_cap.tag : -1; }

namespace {
void finish_capture(int code) {
    if (!g_cap.on) return;
    auto done = std::move(g_cap.done);
    if (g_cap.kind == Capture::PAD) g_pad_quiet = true;
    g_cap = {};
    done(code);
}
// any button or axis of the open gamepads held
bool pad_active() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    bool any = false;
    for (int i = 0; i < n && !any; i++)
        if (SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i])) {
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !any; b++) any = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !any; a++) any = std::abs(SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a)) > 10000;
        }
    SDL_free(ids);
    return any;
}
// the capture's view of an event; true when it took it (ImGui does not see it)
bool capture_event(const SDL_Event& e) {
    if (!g_cap.on) return false;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) finish_capture(0);
        else if (g_cap.kind == Capture::KEY) finish_capture(e.key.scancode);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        finish_capture(0);
        return true;
    }
    if (g_cap.kind != Capture::PAD) return false;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (g_cap.armed) finish_capture(1 + e.gbutton.button);
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (g_cap.armed && std::abs(e.gaxis.value) > 20000) {
            const int a = e.gaxis.axis;
            const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            finish_capture(inputcfg::AXIS + 2 * a + (trigger || e.gaxis.value > 0 ? 1 : 0));
        }
        return true;
    }
    return e.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
}

// one row per option of the section from row index first; returns the index after the last one
int option_rows(int page, int first) {
    int index = first;
    for (opts::Option& o : opts::all()) {
        if (o.page != page) continue;
        if (o.kind == opts::HEADER) {
            header_widget(o.label);
            continue;
        }
        ImGui::PushID(&o);
        const RowOut r = row_widget("row", o.label, o.value_label(), index++, o.kind != opts::TEXT, o.experimental,
                                    o.help, nullptr);
        if (r.delta && o.kind == opts::CHOICE) {
            o.sel = (o.sel + r.delta + (int)o.choices.size()) % (int)o.choices.size();
            opts::changed(o, g_ini);
        } else if (r.delta && o.kind == opts::TOGGLE) {
            o.sel = !o.sel;
        } else if (r.activated) {
            if (o.kind == opts::TOGGLE || (o.kind == opts::CHOICE && o.choices.size() == 2)) {
                o.sel = o.kind == opts::TOGGLE ? !o.sel : 1 - o.sel;
                if (o.kind == opts::CHOICE) opts::changed(o, g_ini);
            } else if (o.kind == opts::CHOICE) {
                std::vector<std::string> items;
                for (const opts::Choice& c : o.choices) items.push_back(c.label);
                open_list(items, o.sel, [&o](int i) {
                    o.sel = i;
                    opts::changed(o, g_ini);
                });
            } else if (o.kind == opts::TEXT) {
                open_edit(o.text, [&o](const std::string& t) { o.text = t; });
            }
        }
        ImGui::PopID();
    }
    return index;
}

// the open choice list, under its row (above it when there is no room)
void choice_list() {
    if (!g_list_on) return;
    if (g_list_open_request) {
        ImGui::OpenPopup("choices");
        g_list_open_request = false;
    }
    const float row_h = 34 * S;
    const int n = (int)g_list.items.size();
    const float h = std::min(n, 9) * (row_h + 2 * S) + 12 * S;
    ImVec2 pos = g_list.pos;
    if (pos.y + h > ImGui::GetIO().DisplaySize.y) pos.y = std::max(0.0f, pos.y - 36 * S - h);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(g_list.w, h));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(250, 250, 250, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(22, 22, 22, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 2 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * S));
    if (ImGui::BeginPopup("choices", ImGuiWindowFlags_NoScrollbar)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int picked = -1;
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            if (ImGui::InvisibleButton("c", ImVec2(w, row_h), ImGuiButtonFlags_EnableNav)) picked = i;
            if (ImGui::IsWindowAppearing() && i == g_list.sel) {
                ImGui::SetItemDefaultFocus();
                ImGui::SetScrollHereY(0.5f);
            }
            const bool f = ImGui::IsItemFocused(), hv = ImGui::IsItemHovered();
            if (f) carbon(dl, p, ImVec2(p.x + w, p.y + row_h), 1);
            else if (hv) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + row_h), IM_COL32(226, 226, 226, 255));
            const std::string& t = g_list.items[i];
            ImFont* font = i == g_list.sel ? F.xbold : F.semi;
            const float tw = text_w(font, 17, t);
            dl->PushClipRect(p, ImVec2(p.x + w, p.y + row_h), true);
            text(dl, font, 17, ImVec2(tw < w - 12 * S ? p.x + (w - tw) / 2 : p.x + 6 * S, p.y + 6 * S), f ? WHITE : INK, t);
            dl->PopClipRect();
            ImGui::PopID();
        }
        if (picked >= 0) {
            ImGui::CloseCurrentPopup();
            g_list_on = false;
            g_focus_request = true;
            g_list.pick(picked);
        }
        ImGui::EndPopup();
    } else {
        g_list_on = false;
        g_focus_request = true;
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// the open text field, over its row's value box: Enter keeps it, Esc or a click elsewhere drops it
void text_edit() {
    if (!g_edit_on) return;
    if (g_edit_open_request) ImGui::OpenPopup("edit");
    ImGui::SetNextWindowPos(g_edit.pos);
    ImGui::SetNextWindowSize(ImVec2(g_edit.w, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(22, 22, 22, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 2 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * S, 4 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * S, 4 * S));
    if (ImGui::BeginPopup("edit", ImGuiWindowFlags_NoScrollbar)) {
        ImGui::PushFont(F.medium, 16 * S);
        ImGui::SetNextItemWidth(-1);
        if (g_edit_open_request) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##edit", g_edit_buf, sizeof(g_edit_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            g_edit.done(g_edit_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    if (!g_edit_open_request && !ImGui::IsPopupOpen("edit")) {
        g_edit_on = false;
        g_focus_request = true;
    }
    g_edit_open_request = false;
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

void pick_folder() {
    static const std::string start = opts::game_path();
    SDL_ShowOpenFolderDialog(
        [](void*, const char* const* list, int) {
            if (!list || !list[0]) return;
            std::lock_guard<std::mutex> l(g_folder_m);
            g_folder_pick = list[0];
            g_folder_new = true;
        },
        nullptr, g_window, opts::game_path().c_str(), false);
}

void save_all() {
    opts::save(g_ini);
    input::save(g_ini);
    wheel::save(g_ini);
    g_ini.set("game", "window_maximized", (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) ? "1" : "0");
    g_ini.save(opts::ini_path());
}

void start_game() {
    save_all();
    wheel::release();   // (the game opens the wheel's force feedback itself; one process at a time)
    const std::string err = opts::start_game(g_ini, [](unsigned code) {
        SDL_Event e{};
        e.type = g_exit_event;
        e.user.code = (Sint32)code;
        SDL_PushEvent(&e);
    });
    if (!err.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F1 CE", err.c_str(), g_window);
        wheel::resume();
        return;
    }
    g_game_running = true;
    SDL_HideWindow(g_window);
}

// ---------------------------------------------------------------- the frame
const char* const SECTION_NAMES[opts::PAGES] = {"GENERAL",  "VIDEO",          "QUICK RACE", "KEYBOARD",
                                                "GAMEPAD", "STEERING WHEEL", "DEVELOPER"};
std::vector<int> sections() {
    std::vector<int> v;
    for (int i = 0; i < opts::PAGES; i++)
        if (i != opts::DEVELOPER || opts::dev_mode()) v.push_back(i);
    return v;
}
void step_section(int d) {
    const std::vector<int> v = sections();
    const int at = (int)(std::find(v.begin(), v.end(), g_page) - v.begin());
    g_page = v[(at + d + (int)v.size()) % (int)v.size()];
    g_focus_request = true;
}

void frame() {
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    S = std::max(std::min(W / 1280.0f, H / 760.0f), 0.3f);
    const bool modal = g_list_on || g_edit_on || g_modal_last || g_cap.on || g_pad_quiet;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                                      ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), WHITE);

    // global buttons (not while a list or a text field has them)
    if (!modal) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyShift))
            step_section(1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && io.KeyShift))
            step_section(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) pick_folder();
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_quit = true;
    }

    // top band: logo, name, version, wheel and GPU
    const float top_h = 92 * S, bot_h = 86 * S, body_y = top_h + 10 * S, body_y1 = H - bot_h - 10 * S;
    carbon(dl, ImVec2(0, 0), ImVec2(W, top_h), 1);
    chrome(dl, top_h, W, true);
    float x = 28 * S;
    if (g_logo) {
        dl->AddImage(tex(g_logo), ImVec2(x, 8 * S), ImVec2(x + 76 * S, 84 * S));
        x += 76 * S + 16 * S;
    }
    text(dl, F.black, 30, ImVec2(x, 26 * S), WHITE, "FORMULA ONE");
    x += text_w(F.black, 30, "FORMULA ONE") + 18 * S;
    text(dl, F.semi, 18, ImVec2(x, 35 * S), IM_COL32(189, 189, 189, 255), "CHAMPIONSHIP EDITION");
    {
        const std::string ver = "LAUNCHER v" F1CE_VERSION_SHORT;
        std::string status = wheel::name();
        status += (status.empty() ? "" : " · ") + opts::gpu_summary();
        text(dl, F.xbold, 13, ImVec2(W - 44 * S - text_w(F.xbold, 13, ver), 24 * S), WHITE, ver);
        text(dl, F.semi, 15, ImVec2(W - 44 * S - text_w(F.semi, 15, status), 46 * S), IM_COL32(158, 158, 158, 255), status);
    }

    // body: chevrons and sections on the left
    chevron(dl, ImVec2(40 * S, body_y + 30 * S), 300 * S, std::min(470 * S, body_y1 - body_y - 60 * S), IM_COL32(240, 240, 240, 255));
    chevron(dl, ImVec2(200 * S, body_y + 30 * S), 300 * S, std::min(470 * S, body_y1 - body_y - 60 * S), IM_COL32(245, 245, 245, 255));
    {
        const std::vector<int> list = sections();
        const float item_h = std::min(54 * S, (body_y1 - body_y - 40 * S) / list.size() - 8 * S), gap = 8 * S;
        const float total = list.size() * item_h + (list.size() - 1) * gap;
        float y = (body_y + body_y1 - total) / 2;
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i : list) {
            ImGui::SetCursorScreenPos(ImVec2(70 * S, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("section", ImVec2(330 * S, item_h)) && g_page != i) {
                g_page = i;
                g_focus_request = true;
            }
            const bool sel = i == g_page, hv = ImGui::IsItemHovered();
            ImGui::PopID();
            const float size = sel ? 40 : 32;
            if (sel) chevron(dl, ImVec2(70 * S, y + 14 * S), 18 * S, 26 * S, RED);
            const float ty = y + (item_h - size * S) / 2 - 2 * S;
            text(dl, F.black, size, ImVec2(114 * S + 2 * S, ty + 2 * S), IM_COL32(0, 0, 0, 30), SECTION_NAMES[i]);
            text(dl, F.black, size, ImVec2(114 * S, ty), sel ? IM_COL32(17, 17, 17, 255) : hv ? IM_COL32(110, 110, 110, 255) : GREY,
                 SECTION_NAMES[i]);
            y += item_h + gap;
        }
        ImGui::PopItemFlag();
    }

    // the section's rows, scrolled; position and description under them
    g_help.clear();
    g_help_hover = nullptr;
    const float rx = 420 * S, rw = W - rx - 56 * S, ry = body_y + 22 * S, help_h = 74 * S;
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 3 * S));
    ImGui::BeginChild(SECTION_NAMES[g_page], ImVec2(rw, body_y1 - ry - help_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    int rows = 0;
    const bool focus_first = g_focus_request && g_focus[g_page] == 0;
    wheel::frame(g_page == opts::WHEEL);
    if (g_page == opts::WHEEL) rows = wheel::rows();
    else if (g_page == opts::KEYBOARD) rows = input::keyboard_rows(0);
    else if (g_page == opts::GAMEPAD) rows = input::gamepad_rows(option_rows(opts::GAMEPAD, 0));
    else rows = option_rows(g_page, 0);
    if (focus_first) ImGui::SetScrollY(0);   // (the first row: the top of the section shows, the wheel's live view too)
    const float scroll = ImGui::GetScrollY(), scroll_max = ImGui::GetScrollMaxY();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (g_focus_request && rows == 0) g_focus_request = false;
    {
        const float hy = body_y1 - help_h + 6 * S;
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", std::min(g_focus[g_page] + 1, rows), rows);
        text(dl, F.xbold, 14, ImVec2(rx + 18 * S, hy), GREY, pos);
        if (scroll < scroll_max - 1) {
            const int more = (int)std::ceil((scroll_max - scroll) / (39 * S));
            const std::string m = std::to_string(more) + " MORE";
            const float mw = text_w(F.xbold, 14, m);
            text(dl, F.xbold, 14, ImVec2(rx + rw - 36 * S - mw, hy), GREY, m);
            const float tx = rx + rw - 28 * S, ty = hy + 6 * S, s = 5 * S;
            dl->AddTriangleFilled(ImVec2(tx, ty), ImVec2(tx + 2 * s, ty), ImVec2(tx + s, ty + s * 1.4f), GREY);
        }
        std::string help = g_help.empty() && g_help_hover ? g_help_hover : g_help;
        float y = hy + 22 * S;
        if (const std::string problem = opts::game_path_problem(); !problem.empty()) {
            const std::string line = "GAME FOLDER: " + problem;
            text(dl, F.xbold, 14, ImVec2(rx + 18 * S, y), RED, line, rw - 36 * S);
            y += F.xbold->CalcTextSizeA(14 * S, FLT_MAX, rw - 36 * S, line.c_str()).y + 4 * S;   // (wraps)
        }
        text(dl, F.medium, 14, ImVec2(rx + 18 * S, y), HELP, help, rw - 36 * S);
    }

    // bottom band: the buttons
    chrome(dl, body_y1, W, false);
    carbon(dl, ImVec2(0, H - bot_h), ImVec2(W, H), 0.8f);
    {
        const float cy = H - bot_h / 2;
        float bx = 44 * S;
        auto hint = [&](Action a, const char* label) {
            bx += prompt(dl, a, ImVec2(bx, cy)) + 10 * S;
            text(dl, F.xbold, 19, ImVec2(bx, cy - 12 * S), WHITE, label);
            bx += text_w(F.xbold, 19, label) + 30 * S;
        };
        hint(A_SECTION, "SECTION");
        hint(A_CHANGE, "CHANGE");
        hint(A_SELECT, "SELECT");
        const float left_end = bx;
        // right: start, quit (clickable)
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        auto button = [&](Action a, const char* label, float right) {   // returns its left edge; sets clicked
            const float pw = prompt_width(dl, a);
            const float w = pw + 10 * S + text_w(F.xbold, 19, label);
            const float l = right - w;
            ImGui::SetCursorScreenPos(ImVec2(l - 8 * S, cy - 22 * S));
            ImGui::PushID(label);
            const bool clicked = ImGui::InvisibleButton("b", ImVec2(w + 16 * S, 44 * S));
            if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(l - 8 * S, cy - 22 * S), ImVec2(right + 8 * S, cy + 22 * S), IM_COL32(255, 255, 255, 28), 4 * S);
            ImGui::PopID();
            prompt(dl, a, ImVec2(l, cy));
            text(dl, F.xbold, 19, ImVec2(l + pw + 10 * S, cy - 12 * S), WHITE, label);
            return std::make_pair(l, clicked);
        };
        auto [start_l, start_clicked] = button(A_START, "START GAME", W - 44 * S);
        auto [quit_l, quit_clicked] = button(A_QUIT, "QUIT", start_l - 34 * S);
        if (start_clicked) start_game();
        if (quit_clicked) g_quit = true;
        // the game folder, between them: △ or a click picks it
        const float fx0 = left_end + 10 * S, fx1 = quit_l - 30 * S;
        if (fx1 - fx0 > 200 * S) {
            const std::string problem = opts::game_path_problem();
            const float bw = std::min(fx1 - fx0, 470 * S), bx0 = (fx0 + fx1 - bw) / 2;
            ImGui::SetCursorScreenPos(ImVec2(bx0, cy - 26 * S));
            const bool clicked = ImGui::InvisibleButton("folder", ImVec2(bw, 52 * S));
            const bool hv = ImGui::IsItemHovered();
            dl->AddRectFilled(ImVec2(bx0, cy - 26 * S), ImVec2(bx0 + bw, cy + 26 * S), IM_COL32(255, 255, 255, hv ? 28 : 15));
            dl->AddRect(ImVec2(bx0, cy - 26 * S), ImVec2(bx0 + bw, cy + 26 * S),
                        problem.empty() ? IM_COL32(255, 255, 255, 36) : RED, 0, 0, problem.empty() ? 1 : 2 * S);
            const float pw = prompt_width(dl, A_FOLDER);
            const float px = bx0 + bw - 16 * S - text_w(F.xbold, 16, "CHANGE") - 10 * S - pw;
            prompt(dl, A_FOLDER, ImVec2(px, cy));
            text(dl, F.xbold, 16, ImVec2(px + pw + 10 * S, cy - 10 * S), WHITE, "CHANGE");
            text(dl, F.xbold, 11, ImVec2(bx0 + 16 * S, cy - 19 * S), problem.empty() ? IM_COL32(158, 158, 158, 255) : RED,
                 problem.empty() ? "GAME FOLDER (PS3_GAME)" : "GAME FOLDER (PS3_GAME): NOT USABLE");
            dl->PushClipRect(ImVec2(bx0, cy - 26 * S), ImVec2(px - 10 * S, cy + 26 * S), true);
            text(dl, F.medium, 14, ImVec2(bx0 + 16 * S, cy - 3 * S), WHITE, opts::game_path());
            dl->PopClipRect();
            if (clicked) pick_folder();
        }
        ImGui::PopItemFlag();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    choice_list();
    text_edit();
    // start: the gamepad's Start, F5 (also while nothing is modal)
    if (!modal && (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false))) start_game();
    g_modal_last = g_list_on || g_edit_on;
}

void style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.FrameRounding = s.PopupRounding = s.ChildRounding = 0;
    s.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   // (the focused row is drawn in carbon instead)
    s.Colors[ImGuiCol_WindowBg] = ImVec4(1, 1, 1, 1);
    s.Colors[ImGuiCol_Text] = ImVec4(0.1f, 0.1f, 0.1f, 1);
    s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.83f, 0.18f, 0.18f, 0.35f);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
    s.FramePadding = ImVec2(8, 4);
}
ImFont* add_font(const unsigned char* data, unsigned size) {
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)data, (int)size, 18.0f, &c);
}
}  // namespace

int run() {
    const char* base = SDL_GetBasePath();
    opts::init(base ? base : ".");
    g_ini.load(opts::ini_path());
    opts::load(g_ini);
    input::load(g_ini);

#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");   // (f1ce_launcher.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F1 CE", SDL_GetError(), nullptr);
        return 1;
    }
    // 1280x760 at the display's scale, within its usable area
    const SDL_DisplayID disp = SDL_GetPrimaryDisplay();
    float scale = SDL_GetDisplayContentScale(disp);
    if (scale <= 0) scale = 1;
    SDL_Rect usable{0, 0, 1280, 760};
    SDL_GetDisplayUsableBounds(disp, &usable);
    const float fit = std::min({scale, usable.w * 0.92f / 1280, usable.h * 0.9f / 760});
    g_window = SDL_CreateWindow("Formula One: Championship Edition", (int)(1280 * fit), (int)(760 * fit),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_SetWindowMinimumSize(g_window, 640, 380);
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_window || !g_renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F1 CE", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(g_renderer, 1);
    g_exit_event = SDL_RegisterEvents(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;   // (drawn transparent: the focused row is the cursor)
    F.black = add_font(font_black_italic, font_black_italic_size);
    F.xbold = add_font(font_extrabold_italic, font_extrabold_italic_size);
    F.semi = add_font(font_semibold_italic, font_semibold_italic_size);
    F.medium = add_font(font_medium, font_medium_size);
    io.FontDefault = F.medium;
    style();
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    make_carbon();
    make_logo();

    int frames = 0, idle = 0;
    bool shown = false;
    auto handle = [&](const SDL_Event& e) {
        if (capture_event(e)) return;
        ImGui_ImplSDL3_ProcessEvent(&e);
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: if (e.window.windowID == SDL_GetWindowID(g_window)) g_quit = true; break;
            case SDL_EVENT_KEY_DOWN: case SDL_EVENT_MOUSE_BUTTON_DOWN: g_prompts = KEYBOARD; break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(e.gaxis.value) < 16000) break;
                const SDL_JoystickID id = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.which : e.gaxis.which;
                const SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
                g_prompts = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? PLAYSTATION : XBOX;
                break;
            }
            default:
                if (e.type == g_exit_event) {
                    g_game_running = false;
                    SDL_ShowWindow(g_window);
                    SDL_RaiseWindow(g_window);
                    wheel::resume();
                    if (const std::string m = opts::exit_message((unsigned)e.user.code); !m.empty())
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F1 CE", m.c_str(), g_window);
                }
        }
    };
    while (!g_quit) {
        SDL_Event e;
        bool any = false;
        // idle: wait for input (a few frames after the last one: ImGui settles hover and nav), unless the wheel's
        // live views are on screen
        const bool live = g_page == opts::WHEEL && !g_game_running;
        if ((g_game_running || (idle > 4 && !live)) && SDL_WaitEventTimeout(&e, g_game_running ? -1 : 500)) {
            handle(e);
            any = true;
        }
        while (SDL_PollEvent(&e)) {
            handle(e);
            any = true;
        }
        if (g_quit) break;
        idle = any ? 0 : idle + 1;
        if (g_game_running) continue;
        {
            std::lock_guard<std::mutex> l(g_folder_m);
            if (g_folder_new) {
                opts::game_path() = g_folder_pick;
                g_folder_new = false;
            }
        }
        if (g_cap.on && g_cap.kind == Capture::PAD && !g_cap.armed && !pad_active()) g_cap.armed = true;
        if (g_cap.on && SDL_GetTicks() - g_cap.t0 > 8000) finish_capture(0);
        if (g_pad_quiet && !g_cap.on && !pad_active()) g_pad_quiet = false;
        const bool pad_nav = !(g_cap.on && g_cap.kind == Capture::PAD) && !g_pad_quiet;
        if (pad_nav) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);   // (Tab switches section)
        frame();
        ImGui::PopItemFlag();
        ImGui::Render();
        SDL_SetRenderDrawColor(g_renderer, 255, 255, 255, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        SDL_RenderPresent(g_renderer);
        if (!shown) {   // (after the first frame: no white flash)
            if (g_ini.get_int("game", "window_maximized", 0)) SDL_MaximizeWindow(g_window);
            SDL_ShowWindow(g_window);
            shown = true;
        }
        if (++frames == 2) {   // gamepads and the wheel after the window shows (DirectInput takes its time)
            SDL_InitSubSystem(SDL_INIT_GAMEPAD);
            wheel::start(g_ini);
        }
    }
    save_all();
    wheel::release();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
}  // namespace ui

int main(int, char**) { return ui::run(); }
