// What the launcher's sections share (ui.cpp): fonts, scale, colours and the row widgets, so that the Steering Wheel
// section (wheel_page.cpp) looks like the others.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../third_party/imgui-1.93.0/imgui.h"

namespace ui {
struct Fonts { ImFont *black, *xbold, *semi, *medium; };   // Exo 2: Black / ExtraBold / SemiBold italic, Medium
extern Fonts F;
extern float S;   // pixels per design unit (1280x760 design)
extern const ImU32 INK, GREY, RED, HELP, WHITE;

void carbon(ImDrawList* dl, ImVec2 a, ImVec2 b, float gloss);
void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& s, float wrap = 0);   // size: design units
float text_w(ImFont* f, float size, const std::string& s);

// one setting row: label on the left, value between ◀ ▶ (arrows) on the right. index: among the section's focusable
// rows (the focus is restored by it). Returns what the player did.
struct RowOut { bool activated = false; int delta = 0; bool focused = false; };
RowOut row_widget(const char* id, const std::string& label, const std::string& value, int index, bool arrows,
                  bool experimental, const char* help, bool* focused_out);
void header_widget(const std::string& title);
// a list of choices under the last row (✕ / click picks one, ○ / Esc closes), a text field over its value box
void open_list(std::vector<std::string> items, int sel, std::function<void(int)> pick);
void open_edit(const std::string& text, std::function<void(const std::string&)> done);
// waits for the next key / pad input (a pad input only after everything held is let go) and gives its code to done:
// an SDL scancode, or an inputcfg gamepad input; 0 when cancelled (Esc, a click, 8 s). tag: the row waiting
enum class Capture { KEY, PAD };
void start_capture(Capture kind, int tag, std::function<void(int)> done);
bool capturing();
int capture_tag();
}  // namespace ui
