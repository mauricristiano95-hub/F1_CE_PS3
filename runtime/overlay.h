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
// Toggles the on-screen FPS counter (also enabled at launch with F1CE_SHOW_FPS=1; F3 in-game).
void toggle_fps();
// Toggles the on-screen Fastest Lap / AI vs Player timing pill (F1CE_SHOW_LAPTIMES=1; F4 in-game).
void toggle_laptimes();

// ---- Steward / Penalty notification banner (hle_penalties.cpp): top-center toast or persistent Drive-Through notice
enum class PenaltySeverity { Info, Warning, FinalWarning, DriveThrough, Serving, Served, BlackFlag };
struct PenaltyBanner {
    bool active = false;
    PenaltySeverity severity = PenaltySeverity::Info;
    std::string title;
    std::string subtitle;
    u64 expires_at_us = 0;   // 0 = persistent until cleared/replaced; else auto-hide at time_usec() >= expires_at_us
};
void set_penalty_banner(const PenaltyBanner& b);   // (any thread)

// ---- On-track MFD overlay (hle_mfd.cpp): next pit-stop tyre selector (+ optional keyboard TC/ABS toast)
struct MfdState {
    bool visible = false;
    int tyre_code = 0;       // 0=AUTO, 1=HARD, 2=SOFT, 3=INTER, 4=WET
    std::string tyre_label;  // AUTO / HARD / SOFT / INTER / WET
    std::string note;        // brief action subtitle while expanded
    u64 popup_until_us = 0;  // expanded tyre card until this timestamp; then collapses to pit tyre badge if != AUTO
    // Optional keyboard-only TC/ABS quick toast (F6 / F7)
    std::string aid_tag;     // "TC" or "ABS"
    std::string aid_val;     // e.g. "OFF", "LOW (25%)", "ON"
    bool aid_green = false;
    u64 aid_until_us = 0;
};
void set_mfd_state(const MfdState& s);   // (any thread)

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

// ---- Advanced Replay Mode bar (hle_replay.cpp): drawn while no dialog is up; it never takes the pad. The mouse
// shows it (moving over the window), seeks (a click or drag on the timeline) and plays / pauses (the symbol).
struct ReplayBar {
    bool active = false;   // a replay plays with the director on: the mouse may show the bar
    bool shown = false;    // shown for the pad (a director button was pressed lately)
    bool paused = false;
    float pos = 0, length = 0;   // seconds
    std::string speed, driver, camera;
};
void set_replay_bar(const ReplayBar& bar);   // (any thread)
struct ReplayBarInput {
    float seek = -1;            // a click on the timeline: the fraction of its length (0..1), -1 none
    bool toggle_pause = false;  // a click on the play / pause symbol
};
ReplayBarInput take_replay_input();   // what the mouse asked since the last call (and clears it)
}  // namespace overlay
