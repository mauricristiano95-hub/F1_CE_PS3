// Launcher of the recompiled game: picks the options that are environment variables (README, "Debugging aids"), keeps
// them in f1ce_launcher.ini next to the executable and starts f1ce.exe. The game runs from the folder above build-win,
// the current folder of the old launch script: log and saves (ps3_data/) are relative to it.
// Three tabs: General (Video Settings and Game Settings), Quick Race Settings (settings that replace the game menu's,
// F1CE_QR_*) and Developer Tools (debugging variables, each with its README description as a tooltip).
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
struct Choice { const wchar_t* label; const wchar_t* value; };   // value nullptr: the variable stays unset

// sections: group boxes, stacked in the columns of their tab
struct Section { const wchar_t* title; int page, col; };
enum { VIDEO, GAME, QR_RACE, QR_RULES, DEV_LOG, DEV_TIMING, DEV_SPU, DEV_VALUES, DEV_RENDER, DEV_VALUES2 };
const Section g_sections[] = {
    {L"Video Settings", 0, 0}, {L"Game Settings", 0, 1},
    {L"Race", 1, 0}, {L"Rules", 1, 1},
    {L"Logging and profiling", 2, 0}, {L"Timing and threads", 2, 0}, {L"SPU and job manager", 2, 1},
    {L"Values (empty: unset)", 2, 1}, {L"Renderer", 2, 2}, {L"Captures and tests", 2, 2},
};
constexpr int PAGES = 3;
const int g_cols[PAGES] = {2, 2, 3};

struct Combo {
    const wchar_t* label;
    const wchar_t* env;
    std::vector<Choice> choices;
    int def;
    int section;
    HWND h = nullptr;
};
struct Check {
    const wchar_t* label;
    const wchar_t* env;   // "--name": a command-line switch instead of a variable
    int section;
    // shown under the check box (General; text before a '|' is a warning, drawn bold red) or as its tooltip
    // (Developer Tools)
    const wchar_t* note = nullptr;
    const wchar_t* value = L"1";      // the variable's value when checked
    HWND h = nullptr;
};
struct Field {   // free text; empty leaves the variable unset
    const wchar_t* env;
    int section;
    const wchar_t* tip;
    HWND h = nullptr;
};

Combo g_combos[] = {
    {L"Renderer", L"F1CE_RENDERER", {{L"OpenGL", nullptr}, {L"Vulkan", L"vulkan"}}, 0, VIDEO},
    {L"Internal resolution", L"F1CE_SCALE",
     {{L"720x480 (4:3, PS3 480p)", L"480p:4x3"}, {L"720x480 (16:9, PS3 480p)", L"480p"},
      {L"720x576 (4:3, PS3 576p)", L"576p:4x3"}, {L"720x576 (16:9, PS3 576p)", L"576p"}, {L"904x565 (16:10)", L"904x565"},
      {L"1280x720 (16:9, original)", nullptr}, {L"1280x800 (16:10)", L"1280x800"}, {L"1440x900 (16:10)", L"1440x900"},
      {L"1920x1080 (16:9)", L"1.5"}, {L"2560x1080 (21:9)", L"2560x1080"}, {L"2560x1440 (16:9)", L"2"},
      {L"3440x1440 (21:9)", L"3440x1440"}, {L"3840x2160 (16:9)", L"3"}, {L"5120x2880 (16:9)", L"4"}}, 5, VIDEO},
    {L"Window size", L"F1CE_WINDOW",
     {{L"1280x720", nullptr}, {L"1600x900", L"1600x900"}, {L"1920x1080", L"1920x1080"}, {L"2560x1440", L"2560x1440"}}, 0, VIDEO},
    {L"Vertical sync", L"F1CE_VSYNC", {{L"On", nullptr}, {L"Off", L"0"}, {L"Adaptive", L"-1"}}, 0, VIDEO},
    {L"Anti-aliasing (MSAA)", L"F1CE_MSAA", {{L"2x", L"2"}, {L"4x (original)", nullptr}, {L"8x", L"8"}}, 1, VIDEO},
    {L"Shadow map resolution", L"F1CE_SHADOW_RES",
     {{L"512x1024 (low)", L"0.5"}, {L"1024x2048 (original)", L"1"}, {L"Internal resolution", nullptr},
      {L"2048x4096 (high)", L"2"}}, 2, VIDEO},
    // fewer reflection updates save CPU time (runtime/hle_cubemap.cpp)
    {L"Reflection updates", L"F1CE_CUBE_HZ",
     {{L"30 per second (original)", nullptr}, {L"Every frame", L"0"}, {L"20 per second", L"20"}, {L"15 per second", L"15"},
      {L"10 per second", L"10"}}, 0, VIDEO},
    // with "Up to 60 fps": a steady 30 on machines that cannot hold 60 (Steam Deck), with that mode's pacing
    // (runtime/hle_gcm.cpp pace_ticks)
    {L"60 fps mode cap", L"F1CE_FPS_CAP", {{L"None", nullptr}, {L"30 fps", L"30"}, {L"45 fps", L"45"}}, 0, GAME},
    // gamepad motors: update 02.00's DualShock 3 vibration ported to this version (runtime/hle_rumble.cpp)
    {L"Controller vibration", L"F1CE_RUMBLE",
     {{L"Off", L"0"}, {L"Light", L"50"}, {L"Normal", nullptr}, {L"Strong", L"150"}}, 2, GAME},
    // the game's language when its menu is skipped (F1CE_SKIP_INTRO); otherwise the one the menu starts on
    // (runtime/hle_boot.cpp)
    {L"Language", L"F1CE_LANGUAGE",
     {{L"English", nullptr}, {L"Français", L"french"}, {L"Deutsch", L"german"}, {L"Italiano", L"italian"},
      {L"Español", L"spanish"}, {L"Português", L"portuguese"}, {L"Suomi", L"finnish"}}, 0, GAME},
    // Quick Race Settings (runtime/hle_quickrace.cpp): "Game menu" leaves the variable unset
    {L"Circuit", L"F1CE_QR_TRACK",
     {{L"Game menu", nullptr}, {L"Bahrain (Sakhir)", L"bahrain"}, {L"Malaysia (Sepang)", L"malaysia"},
      {L"Australia (Melbourne)", L"melbourne"}, {L"San Marino (Imola)", L"sanmarino"}, {L"Europe (Nürburgring)", L"nurburgring"},
      {L"Spain (Barcelona)", L"spain"}, {L"Monaco", L"monaco"}, {L"Great Britain (Silverstone)", L"silverstone"},
      {L"Canada (Montréal)", L"canada"}, {L"USA (Indianapolis)", L"indy"}, {L"France (Magny-Cours)", L"france"},
      {L"Germany (Hockenheim)", L"hockenheim"}, {L"Hungary (Budapest)", L"hungary"}, {L"Turkey (Istanbul)", L"istanbul"},
      {L"Italy (Monza)", L"monza"}, {L"China (Shanghai)", L"shanghai"}, {L"Japan (Suzuka)", L"suzuka"},
      {L"Brazil (Interlagos)", L"brazil"}}, 0, QR_RACE},
    {L"Driver", L"F1CE_QR_DRIVER",
     {{L"Game menu", nullptr}, {L"F. Alonso (Renault)", L"falonso"}, {L"G. Fisichella (Renault)", L"gfisichella"},
      {L"K. Räikkönen (McLaren)", L"kraikkonen"}, {L"J. Montoya (McLaren)", L"jmontoya"},
      {L"M. Schumacher (Ferrari)", L"mschumacher"}, {L"F. Massa (Ferrari)", L"fmassa"},
      {L"R. Schumacher (Toyota)", L"rschumacher"}, {L"J. Trulli (Toyota)", L"jtrulli"}, {L"M. Webber (Williams)", L"mwebber"},
      {L"N. Rosberg (Williams)", L"nrosberg"}, {L"R. Barrichello (Honda)", L"rbarrichello"}, {L"J. Button (Honda)", L"jbutton"},
      {L"D. Coulthard (Red Bull)", L"dcoulthard"}, {L"C. Klien (Red Bull)", L"cklien"},
      {L"N. Heidfeld (BMW Sauber)", L"nheidfeld"}, {L"J. Villeneuve (BMW Sauber)", L"jvilleneuve"},
      {L"T. Monteiro (MF1)", L"tmonteiro"}, {L"C. Albers (MF1)", L"calbers"}, {L"V. Liuzzi (Toro Rosso)", L"vliuzzi"},
      {L"S. Speed (Toro Rosso)", L"sspeed"}, {L"T. Sato (Super Aguri)", L"tsato"}, {L"Y. Ide (Super Aguri)", L"yide"}}, 0, QR_RACE},
    {L"Race distance", L"F1CE_QR_DISTANCE",
     {{L"Game menu", nullptr}, {L"10%", L"10"}, {L"20%", L"20"}, {L"30%", L"30"}, {L"50%", L"50"}, {L"75%", L"75"},
      {L"100%", L"100"}}, 0, QR_RACE},
    {L"Weather", L"F1CE_QR_WEATHER",
     {{L"Game menu", nullptr}, {L"Variable", L"variable"}, {L"Sunny", L"sunny"}, {L"Cloudy", L"cloudy"},
      {L"Light rain", L"lightrain"}, {L"Heavy rain", L"heavyrain"}}, 0, QR_RACE},
    {L"Opponent AI", L"F1CE_QR_AI", {{L"Game menu", nullptr}, {L"Easy", L"easy"}, {L"Medium", L"medium"}, {L"Hard", L"hard"}}, 0, QR_RACE},
    {L"Fuel use && tyre wear", L"F1CE_QR_FUEL",
     {{L"Game menu", nullptr}, {L"Off", L"off"}, {L"Scaled", L"scaled"}, {L"Realistic", L"realistic"}}, 0, QR_RULES},
    {L"Crash damage", L"F1CE_QR_DAMAGE", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, QR_RULES},
    {L"Component failure", L"F1CE_QR_FAILURES", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, QR_RULES},
    {L"Penalties", L"F1CE_QR_PENALTIES", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, QR_RULES},
    // Developer Tools
    {L"F1CE_PIN", L"F1CE_PIN", {{L"Default", nullptr}, {L"0: no pinning", L"0"}, {L"1: main PPU too", L"1"}}, 0, DEV_TIMING},
};

Check g_checks[] = {
    {L"FXAA on the final image", L"F1CE_FXAA", VIDEO},
    {L"Start in full screen (F11 toggles it)", L"F1CE_FULLSCREEN", VIDEO},
    {L"Stretch to the window instead of keeping the aspect ratio", L"F1CE_STRETCH", VIDEO},
    {L"Up to 60 fps (game speed unchanged)", L"F1CE_60FPS", GAME,
     L"Experimental:|flashing shadows, odd pit stop animations and audio, broken replay and ghost car behaviour. "
     L"Turn it off if anything goes wrong."},
    {L"Skip the language menu and the intro (start at Press Start)", L"F1CE_SKIP_INTRO", GAME},
    {L"No log file (keep it off when reporting a bug)", L"F1CE_NO_LOG", GAME},
    // Developer Tools: labels are the variables, the notes their tooltips (README, "Debugging aids")
    {L"--trace", L"--trace", DEV_LOG, L"Logs every HLE call and syscall (very slow)."},
    {L"F1CE_PROFILE", L"F1CE_PROFILE", DEV_LOG, L"Every few seconds: what each guest thread spends its time on."},
    {L"F1CE_THREAD_CPU", L"F1CE_THREAD_CPU", DEV_LOG, L"With the cpu: line every 5 s, the cores each thread used."},
    {L"F1CE_USLEEP_LOG", L"F1CE_USLEEP_LOG", DEV_LOG, L"sys_timer_usleep requests per second by size, and the callers of short ones."},
    {L"F1CE_SPU_DMA_STATS", L"F1CE_SPU_DMA_STATS", DEV_LOG, L"Every 10 s per SPU worker: DMA and atomic commands by workload, pc and command."},
    {L"F1CE_SPU_DMA_DIAG", L"F1CE_SPU_DMA_DIAG", DEV_LOG, L"Per-DMA diagnostics of the SPURS workloads (costs a shared atomic per DMA)."},
    {L"F1CE_TASK_DMA_LOG", L"F1CE_TASK_DMA_LOG", DEV_LOG, L"Logs each distinct DMA of the SPURS tasks once."},
    {L"F1CE_PM_SKIP_LOG", L"F1CE_PM_SKIP_LOG", DEV_LOG, L"Logs the job that follows 96+ skip entries of a job queue."},
    {L"F1CE_RUMBLE_LOG", L"F1CE_RUMBLE_LOG", DEV_LOG, L"Logs the player-car data that drives the gamepad vibration."},
    {L"F1CE_DUMP_SHADERS", L"F1CE_DUMP_SHADERS", DEV_LOG, L"Writes the generated shaders (fp_<hash>.glsl, vp_<hash>.glsl)."},
    {L"F1CE_60FPS_NOHOLD", L"F1CE_60FPS_NOHOLD", DEV_TIMING, L"With 60 fps: a stopped car in a race is not held at 30 fps."},
    {L"F1CE_TICKS_FIXED", L"F1CE_TICKS_FIXED", DEV_TIMING, L"With 60 fps: exactly one tick per frame whatever the time."},
    {L"F1CE_SHAKE_FILTER=0", L"F1CE_SHAKE_FILTER", DEV_TIMING, L"With 60 fps: turns off the grid shake filter.", L"0"},
    {L"F1CE_SPIN_WAIT", L"F1CE_SPIN_WAIT", DEV_TIMING, L"Spinning waits as before 2026-11-06 (A/B comparisons)."},
    {L"F1CE_TASK_THREADS", L"F1CE_TASK_THREADS", DEV_TIMING, L"A new thread per SPURS task as before 2026-11-06."},
    {L"F1CE_PRIO=0", L"F1CE_PRIO", DEV_TIMING, L"All threads at normal priority as before 2026-11-06.", L"0"},
    {L"F1CE_SPURS_POLL", L"F1CE_SPURS_POLL", DEV_TIMING, L"Polls every SPURS workload continuously instead of only signalled ones."},
    {L"F1CE_PM_NATIVE=0", L"F1CE_PM_NATIVE", DEV_SPU, L"Job manager functions as SPU code instead of their native versions.", L"0"},
    {L"F1CE_PM_CHECK", L"F1CE_PM_CHECK", DEV_SPU, L"Every 64th native job manager call also runs interpreted on copies; logs differences."},
    {L"F1CE_SPU_NOAOT", L"F1CE_SPU_NOAOT", DEV_SPU, L"Interprets all SPU code."},
    {L"F1CE_SPU_NOCLUSTER", L"F1CE_SPU_NOCLUSTER", DEV_SPU, L"Single-block SPU translations only (no clusters)."},
    {L"F1CE_SPU_VERIFY", L"F1CE_SPU_VERIFY", DEV_SPU, L"Compares every SPU block with the LS before it runs (slower)."},
    {L"F1CE_SPU_NOIDIOM", L"F1CE_SPU_NOIDIOM", DEV_SPU, L"Runs the table-building SPU loops as translated."},
    {L"F1CE_SPU_NOFASTCULL", L"F1CE_SPU_NOFASTCULL", DEV_SPU, L"Geometry job transform and cull as translated, not native."},
    {L"F1CE_SPU_CULL_CHECK", L"F1CE_SPU_CULL_CHECK", DEV_SPU, L"Every 64th transform/cull also runs interpreted on copies; logs differences."},
    {L"F1CE_SPU_NOCULL", L"F1CE_SPU_NOCULL", DEV_SPU, L"Geometry jobs keep all triangles (fewer SPU, more renderer work)."},
    {L"F1CE_NO_LINEGEN", L"F1CE_NO_LINEGEN", DEV_SPU, L"SPU PUTLLC compares line data only (no write generation)."},
    {L"F1CE_SPU_BENCH", L"F1CE_SPU_BENCH", DEV_SPU, L"SPU interpreter benchmark and shufb self-test."},
    {L"F1CE_SYNC_COMPILE", L"F1CE_SYNC_COMPILE", DEV_RENDER, L"Links shader programs on the render thread."},
    {L"F1CE_NO_PRECOMPILE", L"F1CE_NO_PRECOMPILE", DEV_RENDER, L"Skips the start-up shader phase."},
    {L"F1CE_GL_FINISH", L"F1CE_GL_FINISH", DEV_RENDER, L"Waits for the GPU every frame (tells GPU-bound from CPU-bound)."},
    {L"F1CE_FIFO_VERIFY", L"F1CE_FIFO_VERIFY", DEV_RENDER, L"Re-reads each RSX command header shortly after it ran."},
    {L"F1CE_VK_VALIDATION", L"F1CE_VK_VALIDATION", DEV_RENDER, L"Vulkan validation layers (needs the Vulkan SDK)."},
    {L"F1CE_CUBE_SKIP", L"F1CE_CUBE_SKIP", DEV_RENDER, L"Never renders the reflection cube map (measurement only)."},
    {L"F1CE_TEX_CPU", L"F1CE_TEX_CPU", DEV_RENDER, L"Texture decoding on the CPU."},
    {L"F1CE_VTX_CPU", L"F1CE_VTX_CPU", DEV_RENDER, L"Vertex attributes converted on the CPU."},
    {L"F1CE_NO_IDX_ARENA", L"F1CE_NO_IDX_ARENA", DEV_RENDER, L"An element buffer per index list."},
    {L"F1CE_TEX_NOMEMO", L"F1CE_TEX_NOMEMO", DEV_RENDER, L"No per-unit texture memo."},
    {L"F1CE_TEX_NOSKIP", L"F1CE_TEX_NOSKIP", DEV_RENDER, L"Renderer cache off (to bisect rendering bugs)."},
    {L"F1CE_TEX_NOSTATIC", L"F1CE_TEX_NOSTATIC", DEV_RENDER, L"Renderer cache off (to bisect rendering bugs)."},
    {L"F1CE_VTX_NOCACHE", L"F1CE_VTX_NOCACHE", DEV_RENDER, L"Renderer cache off (to bisect rendering bugs)."},
    {L"F1CE_VTX_NOSTATIC", L"F1CE_VTX_NOSTATIC", DEV_RENDER, L"Renderer cache off (to bisect rendering bugs)."},
    {L"F1CE_VTX_NOSUBDATA", L"F1CE_VTX_NOSUBDATA", DEV_RENDER, L"Renderer cache off (to bisect rendering bugs)."},
    {L"F1CE_RAIN_SMOOTH=0", L"F1CE_RAIN_SMOOTH", DEV_RENDER, L"Screen rain drops without the 3x3 tent filter.", L"0"},
};

Field g_fields[] = {
    {L"F1CE_SPU_DUMP_BLOCKS", DEV_VALUES, L"File: records SPU blocks without a translation (input of tools/spu_aot.py)."},
    {L"F1CE_HOST_PROFILE", DEV_VALUES, L"Seconds: then samples the SPU worker threads for 10 s (hottest host functions)."},
    {L"F1CE_SPU_PROFILE", DEV_VALUES, L"Seconds after the SPU workers start: samples workload and SPU pc every ms for 10 s."},
    {L"F1CE_SPURS_SPUS", DEV_VALUES, L"SPU worker threads (default 5; 6 works, 7 and 8 hang at boot)."},
    {L"F1CE_CPUS", DEV_VALUES, L"Runs on the first n logical processors only (8 = Steam Deck class)."},
    {L"F1CE_CUBE_FACES", DEV_VALUES, L"Reflection cube map faces rendered per frame, in turn (1-6, default 6)."},
    {L"F1CE_MAX_FRAMES_AHEAD", DEV_VALUES, L"Frames the PPU may queue ahead of the renderer (default 1)."},
    {L"F1CE_SKIP_FN", DEV_VALUES, L"Measurement only: these render functions (hex, comma separated) return at once."},
    {L"F1CE_DUMP_FRAMES", DEV_VALUES2, L"Saves every N-th frame as frame_*.argb (tools/frames_to_png.py)."},
    {L"F1CE_DUMP_WINDOW", DEV_VALUES2, L"Saves the window after presentation every n-th frame."},
    {L"F1CE_TRACE_FRAME", DEV_VALUES2, L"Logs every draw of frame n (program, target, state, textures)."},
    {L"F1CE_FLIP_LOG", DEV_VALUES2, L"<first>,<count>: per flip, displayed buffer and draws per render target."},
    {L"F1CE_MEM_DUMP", DEV_VALUES2, L"Input times (s,s,..): saves the data segment and user memory as mem_<s>.bin."},
    {L"F1CE_QUIT_AFTER_3D", DEV_VALUES2, L"Exits that many seconds after the first 3D scene (saves the last frame)."},
    {L"F1CE_INPUT", DEV_VALUES2, L"Scripted pad presses, e.g. 9:start,13:cross (seconds since start)."},
    {L"F1CE_INPUT_RECORD", DEV_VALUES2, L"File: records the player's input in F1CE_INPUT syntax."},
};
// any other variables, NAME=value separated by spaces
const wchar_t* const EXTRA_KEY = L"extra";
HWND g_extra;

enum { ID_START = 100, ID_BROWSE, ID_QUIT, ID_TAB, WM_GAME_EXIT = WM_APP + 1 };
HWND g_path, g_main, g_tab, g_tips;
std::vector<HWND> g_page[PAGES];   // the controls of each tab
HFONT g_font, g_bold;
std::wstring g_exe_dir, g_run_dir, g_ini;

std::wstring parent(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}
std::wstring full_path(const std::wstring& p) {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, nullptr);
    return n ? std::wstring(b, n) : p;
}
bool is_file(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring window_text(HWND h) {
    wchar_t b[MAX_PATH * 2];
    GetWindowTextW(h, b, MAX_PATH * 2);
    return b;
}
bool checked(const Check& c) { return SendMessageW(c.h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void load_settings() {
    wchar_t b[MAX_PATH * 2];
    std::wstring def = full_path(g_run_dir + L"\\..\\F1_CE_PS3_og\\PS3_GAME");
    GetPrivateProfileStringW(L"game", L"path", def.c_str(), b, MAX_PATH * 2, g_ini.c_str());
    SetWindowTextW(g_path, b);
    for (Combo& c : g_combos) {
        GetPrivateProfileStringW(L"options", c.env, L"", b, MAX_PATH * 2, g_ini.c_str());
        int sel = c.def;
        for (size_t i = 0; i < c.choices.size(); i++)
            if (c.choices[i].value && !wcscmp(c.choices[i].value, b)) sel = (int)i;
        SendMessageW(c.h, CB_SETCURSEL, sel, 0);
    }
    for (Check& c : g_checks)
        SendMessageW(c.h, BM_SETCHECK, GetPrivateProfileIntW(L"options", c.env, 0, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
    for (Field& f : g_fields) {
        GetPrivateProfileStringW(L"developer", f.env, L"", b, MAX_PATH * 2, g_ini.c_str());
        SetWindowTextW(f.h, b);
    }
    GetPrivateProfileStringW(L"developer", EXTRA_KEY, L"", b, MAX_PATH * 2, g_ini.c_str());
    SetWindowTextW(g_extra, b);
}

void save_settings() {
    WritePrivateProfileStringW(L"game", L"path", window_text(g_path).c_str(), g_ini.c_str());
    for (Combo& c : g_combos) {
        const wchar_t* v = c.choices[SendMessageW(c.h, CB_GETCURSEL, 0, 0)].value;   // (nullptr deletes the key: default)
        WritePrivateProfileStringW(L"options", c.env, v, g_ini.c_str());
    }
    for (Check& c : g_checks) WritePrivateProfileStringW(L"options", c.env, checked(c) ? L"1" : L"0", g_ini.c_str());
    for (Field& f : g_fields) {
        const std::wstring v = window_text(f.h);
        WritePrivateProfileStringW(L"developer", f.env, v.empty() ? nullptr : v.c_str(), g_ini.c_str());
    }
    const std::wstring extra = window_text(g_extra);
    WritePrivateProfileStringW(L"developer", EXTRA_KEY, extra.empty() ? nullptr : extra.c_str(), g_ini.c_str());
}

DWORD WINAPI wait_game(void* process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    PostMessageW(g_main, WM_GAME_EXIT, code, 0);
    return 0;
}

void start_game() {
    std::wstring game = window_text(g_path), exe = g_exe_dir + L"\\f1ce.exe";
    if (!is_file(game + L"\\USRDIR\\EBOOT.BIN") && !is_file(game + L"\\PARAM.SFO")) {
        MessageBoxW(g_main, L"This folder does not look like the game's PS3_GAME folder.", L"F1 CE", MB_ICONWARNING);
        return;
    }
    save_settings();
    std::wstring cmd = L"\"" + exe + L"\" \"" + game + L"\"";
    // the variable is cleared when unset here: some options act as soon as it exists, whatever its value
    for (Combo& c : g_combos)
        SetEnvironmentVariableW(c.env, c.choices[SendMessageW(c.h, CB_GETCURSEL, 0, 0)].value);
    for (Check& c : g_checks) {
        if (c.env[0] == L'-') { if (checked(c)) cmd += L" " + std::wstring(c.env); continue; }
        SetEnvironmentVariableW(c.env, checked(c) ? c.value : nullptr);
    }
    for (Field& f : g_fields) {
        const std::wstring v = window_text(f.h);
        SetEnvironmentVariableW(f.env, v.empty() ? nullptr : v.c_str());
    }
    {   // extra: NAME=value tokens
        const std::wstring extra = window_text(g_extra) + L" ";
        size_t s = 0;
        while ((s = extra.find_first_not_of(L" \t", s)) != std::wstring::npos) {
            const size_t e = extra.find_first_of(L" \t", s);
            const std::wstring tok = extra.substr(s, e - s);
            const size_t eq = tok.find(L'=');
            if (eq != std::wstring::npos && eq > 0) SetEnvironmentVariableW(tok.substr(0, eq).c_str(), tok.substr(eq + 1).c_str());
            s = e;
        }
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_run_dir.c_str(), &si, &pi)) {
        std::wstring msg = L"Could not start " + exe + L" (error " + std::to_wstring(GetLastError()) + L").";
        MessageBoxW(g_main, msg.c_str(), L"F1 CE", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    ShowWindow(g_main, SW_HIDE);
    CloseHandle(CreateThread(nullptr, 0, wait_game, pi.hProcess, 0, nullptr));
}

void browse() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_main;
    bi.lpszTitle = L"Select the PS3_GAME folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        wchar_t b[MAX_PATH];
        if (SHGetPathFromIDListW(id, b)) SetWindowTextW(g_path, b);
        CoTaskMemFree(id);
    }
}

constexpr int TAB_X = 8, TAB_TOP = 8, TAB_HEAD = 26;

// page: the tab the control belongs to (-1: always shown). Page controls are children of the tab control, so they
// draw on its themed body; x and y are in window coordinates either way.
HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, int page = -1) {
    const int dpi = GetDpiForSystem();
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    if (page >= 0) { x -= TAB_X; y -= TAB_TOP; }
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | (page > 0 ? 0 : WS_VISIBLE) | style, px(x), px(y), px(w), px(h),
                             page >= 0 ? g_tab : g_main, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_page[page].push_back(c);
    return c;
}

void add_tip(HWND c, const wchar_t* text) {
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(c);
    ti.uId = (UINT_PTR)c;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void show_page(int page) {
    for (int p = 0; p < PAGES; p++)
        for (HWND c : g_page[p]) ShowWindow(c, p == page ? SW_SHOW : SW_HIDE);
    // the transparent labels leave the hidden page's pixels behind unless the whole body is painted again
    RedrawWindow(g_tab, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

constexpr int WIN_W = 900, MARGIN = 14, LABEL_W = 150, ROW = 30, GAP = 12;

// the tab control passes its children's notifications and colour requests on to the main window
LRESULT CALLBACK tab_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND || msg == WM_CTLCOLORSTATIC || msg == WM_DRAWITEM) return SendMessageW(g_main, msg, wp, lp);
    return DefSubclassProc(h, msg, wp, lp);
}

// one group box with its options; returns the y below it
int build_section(int s, int x, int y, int w) {
    const int page = g_sections[s].page, dev = page == 2;
    HWND group = add(L"BUTTON", g_sections[s].title, BS_GROUPBOX, x, y, w, 100, 0, page);
    const int ix = x + 10, iw = w - 20, top = y;
    y += 22;
    const int label_w = dev ? std::min(LABEL_W + 20, iw / 2 + 30) : LABEL_W;
    for (Combo& c : g_combos) {
        if (c.section != s) continue;
        add(L"STATIC", c.label, 0, ix, y + 3, label_w, 18, 0, page);
        c.h = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, ix + label_w, y, iw - label_w, 240, 0, page);
        for (const Choice& ch : c.choices) SendMessageW(c.h, CB_ADDSTRING, 0, (LPARAM)ch.label);
        y += ROW;
    }
    for (Field& f : g_fields) {
        if (f.section != s) continue;
        HWND l = add(L"STATIC", f.env, SS_NOTIFY, ix, y + 4, label_w, 18, 0, page);
        f.h = add(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, ix + label_w, y, iw - label_w, 24, 0, page);
        add_tip(l, f.tip);
        add_tip(f.h, f.tip);
        y += 28;
    }
    for (Check& c : g_checks) {
        if (c.section != s) continue;
        c.h = add(L"BUTTON", c.label, WS_TABSTOP | BS_AUTOCHECKBOX, ix, y, iw, 22, 0, page);
        y += dev ? 22 : 24;
        if (c.note && dev) {
            add_tip(c.h, c.note);
        } else if (c.note) {
            HWND n = add(L"STATIC", c.note, SS_OWNERDRAW, ix + 18, y - 2, iw - 18, 48, 0, page);
            SetWindowLongPtrW(n, GWLP_USERDATA, (LONG_PTR)c.note);
            y += 50;
        }
    }
    y += 8;
    const int dpi = GetDpiForSystem();
    SetWindowPos(group, nullptr, 0, 0, MulDiv(w, dpi, 96), MulDiv(y - top, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    return y + GAP;
}

// returns the height of the window's client area
int build_controls() {
    const int top = TAB_TOP + TAB_HEAD + 10, inner = MARGIN + 6, inner_w = WIN_W - 2 * inner;
    // (no WS_CLIPCHILDREN: the group boxes are transparent, the tab paints its body under them)
    g_tab = add(WC_TABCONTROLW, L"", WS_TABSTOP, TAB_X, TAB_TOP, WIN_W - 2 * TAB_X, 100, ID_TAB);
    SetWindowLongPtrW(g_tab, GWL_EXSTYLE, GetWindowLongPtrW(g_tab, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);   // (Tab key)
    SetWindowSubclass(g_tab, tab_proc, 1, 0);
    g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, g_main, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, 420);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    const wchar_t* names[PAGES] = {L"General", L"Quick Race Settings", L"Developer Tools"};
    for (int p = 0; p < PAGES; p++) {
        ti.pszText = (LPWSTR)names[p];
        SendMessageW(g_tab, TCM_INSERTITEMW, p, (LPARAM)&ti);
    }

    int bottom = top;
    for (int p = 0; p < PAGES; p++) {
        int y0 = top;
        if (p == 0) {   // game folder, above the columns
            add(L"STATIC", L"Game folder (PS3_GAME)", 0, inner, y0, 300, 18, 0, 0);
            g_path = add(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, inner, y0 + 20, inner_w - 86, 24, 0, 0);
            add(L"BUTTON", L"Browse...", WS_TABSTOP, inner + inner_w - 80, y0 + 20, 80, 24, ID_BROWSE, 0);
            y0 += 56;
        }
        const int cols = g_cols[p], col_w = (inner_w - (cols - 1) * GAP) / cols;
        int y_end = y0;
        for (int col = 0; col < cols; col++) {
            int y = y0;
            for (int s = 0; s < (int)(sizeof(g_sections) / sizeof(g_sections[0])); s++)
                if (g_sections[s].page == p && g_sections[s].col == col) y = build_section(s, inner + col * (col_w + GAP), y, col_w);
            y_end = std::max(y_end, y);
        }
        int y = y_end;
        if (p == 1) {
            add(L"STATIC", L"Used by the next Quick Race in place of the game menu's choices (the menu still shows its own).",
                0, inner, y, inner_w, 18, 0, 1);
            y += 24;
        } else if (p == 2) {
            add(L"STATIC", L"Other variables (NAME=value, separated by spaces)", 0, inner, y + 4, 300, 18, 0, 2);
            g_extra = add(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, inner + 300, y, inner_w - 300, 24, 0, 2);
            y += 32;
            add(L"STATIC", L"For debugging: these options slow the game down or change how it runs. Hover an option for "
                L"what it does (README, \"Debugging aids\"). Leave them off for normal play.", 0, inner, y, inner_w, 34, 0, 2);
            y += 38;
        }
        bottom = std::max(bottom, y);
    }
    const int dpi = GetDpiForSystem();
    int y = bottom + 4;
    SetWindowPos(g_tab, nullptr, 0, 0, MulDiv(WIN_W - 2 * TAB_X, dpi, 96), MulDiv(y - TAB_TOP, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    y += 12;
    add(L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, WIN_W - 2 * MARGIN - 180, y, 90, 28, ID_START);
    add(L"BUTTON", L"Quit", WS_TABSTOP, WIN_W - MARGIN - 90, y, 90, 28, ID_QUIT);
    return y + 28 + MARGIN;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == ID_START) start_game();
            else if (LOWORD(wp) == ID_BROWSE) browse();
            else if (LOWORD(wp) == ID_QUIT) DestroyWindow(h);
            return 0;
        case WM_CTLCOLORSTATIC: {   // labels and check boxes on the tab's body: its background under the text
            // (these classes draw with their parent's clip region: without a rectangle the whole tab body would be
            // repainted over the controls drawn before; a group box would cover the options inside it)
            const HWND c = (HWND)lp;
            wchar_t cls[16] = {};
            GetClassNameW(c, cls, 16);
            if (_wcsicmp(cls, L"Button") || (GetWindowLongPtrW(c, GWL_STYLE) & BS_TYPEMASK) != BS_GROUPBOX) {
                RECT rc;
                GetClientRect(c, &rc);
                DrawThemeParentBackground(c, (HDC)wp, &rc);
            }
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        case WM_DRAWITEM: {   // a check box's note: word-wrapped, its warning head bold red
            const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
            const wchar_t* text = (const wchar_t*)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
            if (!text) return FALSE;
            RECT rc = d->rcItem;
            DrawThemeParentBackground(d->hwndItem, d->hDC, &rc);
            SetBkMode(d->hDC, TRANSPARENT);
            const wchar_t* bar = wcschr(text, L'|');
            struct Word { std::wstring s; bool head; };
            std::vector<Word> words;
            auto split = [&](const std::wstring& t, bool head) {
                size_t i = 0;
                while ((i = t.find_first_not_of(L' ', i)) != std::wstring::npos) {
                    const size_t e = t.find(L' ', i);
                    words.push_back({t.substr(i, e - i), head});
                    i = e;
                }
            };
            if (bar) split(std::wstring(text, bar), true);
            split(bar ? bar + 1 : text, false);
            TEXTMETRICW tm;
            SelectObject(d->hDC, g_font);
            GetTextMetricsW(d->hDC, &tm);
            SIZE space;
            GetTextExtentPoint32W(d->hDC, L" ", 1, &space);
            int x = d->rcItem.left, y = d->rcItem.top;
            for (const Word& w : words) {
                SelectObject(d->hDC, w.head ? g_bold : g_font);
                SIZE sz;
                GetTextExtentPoint32W(d->hDC, w.s.c_str(), (int)w.s.size(), &sz);
                if (x > d->rcItem.left && x + sz.cx > d->rcItem.right) { x = d->rcItem.left; y += tm.tmHeight; }
                SetTextColor(d->hDC, w.head ? RGB(200, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
                TextOutW(d->hDC, x, y, w.s.c_str(), (int)w.s.size());
                x += sz.cx + space.cx;
            }
            return TRUE;
        }
        case WM_NOTIFY:
            if (((NMHDR*)lp)->hwndFrom == g_tab && ((NMHDR*)lp)->code == TCN_SELCHANGE)
                show_page((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
            return 0;
        case WM_GAME_EXIT:
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            if (wp) {
                std::wstring msg2 = L"The game stopped with code " + std::to_wstring((unsigned)wp) + L".\nSee " + g_run_dir + L"\\f1ce_recomp.log";
                MessageBoxW(h, msg2.c_str(), L"F1 CE", MB_ICONERROR);
            }
            return 0;
        case WM_DESTROY:
            save_settings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    g_exe_dir = parent(b);
    g_run_dir = parent(g_exe_dir);
    g_ini = g_exe_dir + L"\\f1ce_launcher.ini";

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"F1CELauncher";
    RegisterClassW(&wc);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int dpi = GetDpiForSystem();
    g_main = CreateWindowW(wc.lpszClassName, L"Formula One: Championship Edition", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           100, 100, nullptr, nullptr, inst, nullptr);
    RECT r{0, 0, MulDiv(WIN_W, dpi, 96), MulDiv(build_controls(), dpi, 96)};   // (the size follows the controls)
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(g_main, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    load_settings();
    ShowWindow(g_main, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
