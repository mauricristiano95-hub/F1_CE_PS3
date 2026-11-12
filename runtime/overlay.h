// System dialogs drawn over the game, in place of the PS3's own UI: the save data list (choose, create, overwrite and
// delete saves, as the console and RPCS3 show it). A dialog is laid out with Dear ImGui and rasterised on the CPU into
// a window-sized image, which the renderer blends over each presented frame.
#pragma once
#include "ppu_runtime.h"

#include <string>
#include <vector>

namespace overlay {
// ---- renderer side (the RSX thread, once per presented frame)
struct Image {
    u32 w = 0, h = 0;
    const u32* px = nullptr;   // premultiplied RGBA8 (bytes R, G, B, A), top row first
    u64 serial = 0;            // changes whenever the pixels do
};
// Runs the open dialog for this frame (input, layout, drawing). False: nothing to draw.
bool frame(u32 win_w, u32 win_h, Image* out);
// The game reads a released pad while this is true: a dialog has the input (and until its buttons are let go).
bool input_captured();

// ---- save data dialogs: called on the game's save threads, they block until the player answers
struct SaveEntry {
    std::string dir, title, subtitle, detail;
    s64 mtime = 0;               // seconds since the epoch (0: new data)
    u64 size = 0;                // bytes
    std::vector<u8> icon_png;    // ICON0.PNG
    bool is_new = false;         // the "new data" item
};
enum class SaveMode { Save, Load, Delete };
constexpr int UNAVAILABLE = -2;   // no dialog can be shown (nothing presents frames, or F1CE_SAVE_UI=0)
// The chosen entry, -1 when cancelled, or UNAVAILABLE. Save mode asks before overwriting an existing entry and Delete
// mode before deleting one; "No" goes back to the list.
int save_list(SaveMode mode, const std::vector<SaveEntry>& entries, int focus);
// A message with an OK button. False: no dialog can be shown (it returns at once).
bool message(const std::string& text);
}  // namespace overlay
