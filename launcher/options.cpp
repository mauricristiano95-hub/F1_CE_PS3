#include "options.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "ini.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace fs = std::filesystem;

namespace opts {
namespace {
// UTF-8 paths both ways (fs::path from a std::string is the ANSI code page on Windows)
fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

#ifdef _WIN32
std::wstring W(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(n ? n - 1 : 0);
    return w;
}
std::string U(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    s.resize(n ? n - 1 : 0);
    return s;
}
#endif

std::string g_exe_dir, g_game_path;
bool g_dev_mode = false;

Option choice(Page p, const char* label, const char* key, std::vector<Choice> ch, int def, const char* help) {
    Option o{p, CHOICE, label, key};
    o.choices = std::move(ch);
    o.def = o.sel = def;
    o.help = help;
    return o;
}
Option toggle(Page p, const char* label, const char* key, const char* help, const char* on = "1") {
    Option o{p, TOGGLE, label, key};
    o.help = help;
    o.on = on;
    return o;
}
Option text(Page p, const char* key, const char* help) {
    Option o{p, TEXT, key, key};
    o.help = help;
    return o;
}
Option header(Page p, const char* title) { return Option{p, HEADER, title, ""}; }

std::vector<Option> make_options() {
    std::vector<Option> v;
    // ---------------------------------------------------------------- General
    v.push_back(choice(GAMEPAD, "Controller vibration", "F1CE_RUMBLE",
                       {{"Off", "0"}, {"Light", "50"}, {"Normal", nullptr}, {"Strong", "150"}}, 2,
                       "Gamepad motors: update 02.00's DualShock 3 vibration, ported to this version of the game."));
    v.push_back(choice(GENERAL, "Onboard TV camera", "F1CE_TVCAM",
                       {{"On", nullptr}, {"On, tune with the numpad", "tune"}, {"Off (original camera)", "0"}}, 0,
                       "The second TV pod view of the camera cycle as a broadcast T-cam. Off: the game's own TV pod "
                       "offset camera. Tune: the numpad moves it, values on the console, saved in tvcam_tune.ini."));
    v.push_back(choice(GAMEPAD, "Chase cam look-around", "F1CE_CAM_ORBIT", {{"On", nullptr}, {"Off", "0"}}, 0,
                       "The right stick turns the chase cameras around the car."));
    v.push_back(choice(GENERAL, "Language", "F1CE_LANGUAGE",
                       {{"English", nullptr}, {"Français", "french"}, {"Deutsch", "german"}, {"Italiano", "italian"},
                        {"Español", "spanish"}, {"Português", "portuguese"}, {"Suomi", "finnish"}},
                       0, "The game's language when its language menu is skipped; otherwise the one the menu starts on."));
    v.push_back(choice(GENERAL, "Season pack", "F1CE_MOD_PACK", {{"None (original game)", nullptr}}, 0,
                       "The folders of MODS next to the launcher: a pack replaces the game's files with its own."));
    v.push_back(toggle(GENERAL, "Skip intro", "F1CE_SKIP_INTRO",
                       "Skips the language menu and the intro movies: the game starts at Press Start."));
    v.push_back(toggle(GENERAL, "No log file", "F1CE_NO_LOG",
                       "No f1ce_recomp.log. Keep it off when reporting a bug: the log is what tells what went wrong."));

    // ---------------------------------------------------------------- Video
    v.push_back(choice(VIDEO, "Graphics preset", "preset",
                       {{"Original (PS3)", "original"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"},
                        {"Custom", "custom"}},
                       0,
                       "Sets anti-aliasing, shadows, reflections, textures, car detail, motion blur and heat haze at "
                       "once. Changing one of them makes it Custom (kept for the next time Custom is picked)."));
    v.push_back(choice(VIDEO, "Renderer", "F1CE_RENDERER", {{"OpenGL", nullptr}, {"Vulkan", "vulkan"}}, 0,
                       "Vulkan or OpenGL. Vulkan can pick any GPU; OpenGL runs on Windows' high-performance or "
                       "power-saving GPU."));
    v.push_back(choice(VIDEO, "GPU", "F1CE_GPU", {{"Automatic (high performance)", nullptr}}, 0,
                       "The graphics card the game runs on."));
    v.push_back(choice(VIDEO, "Internal resolution", "F1CE_SCALE",
                       {{"720x480 (4:3, PS3 480p)", "480p:4x3"}, {"720x480 (16:9, PS3 480p)", "480p"},
                        {"720x576 (4:3, PS3 576p)", "576p:4x3"}, {"720x576 (16:9, PS3 576p)", "576p"},
                        {"904x565 (16:10)", "904x565"}, {"1280x720 (16:9, original)", nullptr},
                        {"1280x800 (16:10)", "1280x800"}, {"1440x900 (16:10)", "1440x900"},
                        {"1920x1080 (16:9)", "1.5"}, {"2560x1080 (21:9)", "2560x1080"}, {"2560x1440 (16:9)", "2"},
                        {"3440x1440 (21:9)", "3440x1440"}, {"3840x2160 (16:9)", "3"}, {"5120x2880 (16:9)", "4"}},
                       5, "The resolution the game renders at. 1280x720 is the original."));
    v.push_back(choice(VIDEO, "Window size", "F1CE_WINDOW",
                       {{"1280x720", nullptr}, {"1600x900", "1600x900"}, {"1920x1080", "1920x1080"},
                        {"2560x1080 (21:9)", "2560x1080"}, {"2560x1440", "2560x1440"},
                        {"3440x1440 (21:9)", "3440x1440"}, {"3840x2160", "3840x2160"}},
                       0, "The window's size when not in full screen."));
    v.push_back(choice(VIDEO, "Vertical sync", "F1CE_VSYNC", {{"On", nullptr}, {"Off", "0"}, {"Adaptive", "-1"}}, 0,
                       "On, Off or Adaptive (no wait when a frame is late)."));
    {
        Option o = toggle(VIDEO, "Up to 60 fps", "F1CE_60FPS",
                          "Game speed unchanged. Known issues: flashing shadows, odd pit stop animations, broken "
                          "replay and ghost car behaviour. Turn it off if anything goes wrong.");
        o.experimental = true;
        v.push_back(o);
    }
    v.push_back(choice(VIDEO, "60 fps mode cap", "F1CE_FPS_CAP", {{"None", nullptr}, {"30 fps", "30"}, {"45 fps", "45"}},
                       0, "With Up to 60 fps: a steady 30 or 45 fps on machines that cannot hold 60 (Steam Deck), "
                          "with that mode's frame pacing."));
    v.push_back(choice(VIDEO, "Anti-aliasing (MSAA)", "F1CE_MSAA", {{"2x", "2"}, {"4x (original)", nullptr}, {"8x", "8"}},
                       1, "Smooths the edges of the 3D scene. 4x is the original."));
    v.push_back(choice(VIDEO, "Shadow maps", "F1CE_SHADOW_RES",
                       {{"512x1024 (low)", "0.5"}, {"1024x2048 (original)", "1"}, {"Internal resolution", nullptr},
                        {"2048x4096 (high)", "2"}},
                       2, "Shadow map resolution: sharper car and track shadows, costs GPU time. 1024x2048 is the "
                          "original; Internal resolution scales it with the resolution above."));
    v.push_back(choice(VIDEO, "Reflection updates", "F1CE_CUBE_HZ",
                       {{"30 per second (original)", nullptr}, {"Every frame", "0"}, {"20 per second", "20"},
                        {"15 per second", "15"}, {"10 per second", "10"}},
                       0, "How often the cars' reflections are drawn again. Fewer updates save CPU time."));
    v.push_back(choice(VIDEO, "Reflection resolution", "F1CE_CUBE_RES",
                       {{"128x128 (low)", "0.5"}, {"256x256 (original)", "1"}, {"Internal resolution", nullptr},
                        {"512x512 (high)", "2"}},
                       2, "The reflection map's faces. 256x256 is the original."));
    v.push_back(choice(VIDEO, "Textures", "F1CE_TEX_RES",
                       {{"High (original)", nullptr}, {"Medium (half size)", "1"}, {"Low (quarter size)", "2"}}, 0,
                       "Smaller textures save video memory on small GPUs."));
    v.push_back(choice(VIDEO, "Car detail distance", "F1CE_CAR_LOD",
                       {{"Near (x0.5, lighter)", "0.5"}, {"Original (15/25/50 m)", nullptr}, {"Far (x2)", "2"},
                        {"Very far (x4)", "4"}},
                       1, "How far away the cars switch to their simpler models (15, 25 and 50 m in the original)."));
    v.push_back(choice(VIDEO, "Motion blur", "F1CE_MOTION_BLUR", {{"On (original)", nullptr}, {"Off", "0"}}, 0,
                       "The game's motion blur."));
    v.push_back(choice(VIDEO, "Heat haze", "F1CE_HEAT_HAZE", {{"On (original)", nullptr}, {"Off", "0"}}, 0,
                       "The shimmer behind the cars' exhausts."));
    v.push_back(toggle(VIDEO, "FXAA", "F1CE_FXAA", "FXAA on the final image (softens what MSAA leaves)."));
    v.push_back(toggle(VIDEO, "Full screen", "F1CE_FULLSCREEN", "Start in full screen. F11 toggles it in the game."));
    v.push_back(toggle(VIDEO, "Stretch to window", "F1CE_STRETCH",
                       "Stretch the picture to the window instead of keeping its aspect ratio."));
    v.push_back(toggle(VIDEO, "Static reflections", "F1CE_CUBE_STATIC",
                       "Reflections drawn once when a race scene starts: saves CPU time."));

    // ---------------------------------------------------------------- Quick Race ("Game menu": unset)
    const char* qr = "Used by the next Quick Race in place of the game menu's choice (the menu still shows its own).";
    v.push_back(choice(QUICK_RACE, "Circuit", "F1CE_QR_TRACK",
                       {{"Game menu", nullptr}, {"Bahrain (Sakhir)", "bahrain"}, {"Malaysia (Sepang)", "malaysia"},
                        {"Australia (Melbourne)", "melbourne"}, {"San Marino (Imola)", "sanmarino"},
                        {"Europe (Nürburgring)", "nurburgring"}, {"Spain (Barcelona)", "spain"}, {"Monaco", "monaco"},
                        {"Great Britain (Silverstone)", "silverstone"}, {"Canada (Montréal)", "canada"},
                        {"USA (Indianapolis)", "indy"}, {"France (Magny-Cours)", "france"},
                        {"Germany (Hockenheim)", "hockenheim"}, {"Hungary (Budapest)", "hungary"},
                        {"Turkey (Istanbul)", "istanbul"}, {"Italy (Monza)", "monza"}, {"China (Shanghai)", "shanghai"},
                        {"Japan (Suzuka)", "suzuka"}, {"Brazil (Interlagos)", "brazil"}},
                       0, qr));
    v.push_back(choice(QUICK_RACE, "Driver", "F1CE_QR_DRIVER",
                       {{"Game menu", nullptr}, {"F. Alonso (Renault)", "falonso"},
                        {"G. Fisichella (Renault)", "gfisichella"}, {"K. Räikkönen (McLaren)", "kraikkonen"},
                        {"J. Montoya (McLaren)", "jmontoya"}, {"M. Schumacher (Ferrari)", "mschumacher"},
                        {"F. Massa (Ferrari)", "fmassa"}, {"R. Schumacher (Toyota)", "rschumacher"},
                        {"J. Trulli (Toyota)", "jtrulli"}, {"M. Webber (Williams)", "mwebber"},
                        {"N. Rosberg (Williams)", "nrosberg"}, {"R. Barrichello (Honda)", "rbarrichello"},
                        {"J. Button (Honda)", "jbutton"}, {"D. Coulthard (Red Bull)", "dcoulthard"},
                        {"C. Klien (Red Bull)", "cklien"}, {"N. Heidfeld (BMW Sauber)", "nheidfeld"},
                        {"J. Villeneuve (BMW Sauber)", "jvilleneuve"}, {"T. Monteiro (MF1)", "tmonteiro"},
                        {"C. Albers (MF1)", "calbers"}, {"V. Liuzzi (Toro Rosso)", "vliuzzi"},
                        {"S. Speed (Toro Rosso)", "sspeed"}, {"T. Sato (Super Aguri)", "tsato"},
                        {"Y. Ide (Super Aguri)", "yide"}},
                       0, qr));
    v.push_back(choice(QUICK_RACE, "Race distance", "F1CE_QR_DISTANCE",
                       {{"Game menu", nullptr}, {"10%", "10"}, {"20%", "20"}, {"30%", "30"}, {"50%", "50"},
                        {"75%", "75"}, {"100%", "100"}},
                       0, qr));
    v.push_back(choice(QUICK_RACE, "Weather", "F1CE_QR_WEATHER",
                       {{"Game menu", nullptr}, {"Variable", "variable"}, {"Sunny", "sunny"}, {"Cloudy", "cloudy"},
                        {"Light rain", "lightrain"}, {"Heavy rain", "heavyrain"}},
                       0, qr));
    v.push_back(choice(QUICK_RACE, "Opponent AI", "F1CE_QR_AI",
                       {{"Game menu", nullptr}, {"Easy", "easy"}, {"Medium", "medium"}, {"Hard", "hard"}}, 0, qr));
    v.push_back(choice(QUICK_RACE, "Fuel use & tyre wear", "F1CE_QR_FUEL",
                       {{"Game menu", nullptr}, {"Off", "off"}, {"Scaled", "scaled"}, {"Realistic", "realistic"}}, 0, qr));
    v.push_back(choice(QUICK_RACE, "Crash damage", "F1CE_QR_DAMAGE", {{"Game menu", nullptr}, {"Off", "0"}, {"On", "1"}},
                       0, qr));
    v.push_back(choice(QUICK_RACE, "Component failure", "F1CE_QR_FAILURES",
                       {{"Game menu", nullptr}, {"Off", "0"}, {"On", "1"}}, 0, qr));
    v.push_back(choice(QUICK_RACE, "Penalties", "F1CE_QR_PENALTIES", {{"Game menu", nullptr}, {"Off", "0"}, {"On", "1"}},
                       0, qr));

    // ---------------------------------------------------------------- Developer Tools (README, "Debugging aids")
    v.push_back(header(DEVELOPER, "Logging and profiling"));
    v.push_back(toggle(DEVELOPER, "--trace", "--trace", "Logs every HLE call and syscall (very slow)."));
    v.push_back(toggle(DEVELOPER, "F1CE_PROFILE", "F1CE_PROFILE", "Every few seconds: what each guest thread spends its time on."));
    v.push_back(toggle(DEVELOPER, "F1CE_THREAD_CPU", "F1CE_THREAD_CPU", "With the cpu: line every 5 s, the cores each thread used."));
    v.push_back(toggle(DEVELOPER, "F1CE_USLEEP_LOG", "F1CE_USLEEP_LOG", "sys_timer_usleep requests per second by size, and the callers of short ones."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_DMA_STATS", "F1CE_SPU_DMA_STATS", "Every 10 s per SPU worker: DMA and atomic commands by workload, pc and command."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_DMA_DIAG", "F1CE_SPU_DMA_DIAG", "Per-DMA diagnostics of the SPURS workloads (costs a shared atomic per DMA)."));
    v.push_back(toggle(DEVELOPER, "F1CE_TASK_DMA_LOG", "F1CE_TASK_DMA_LOG", "Logs each distinct DMA of the SPURS tasks once."));
    v.push_back(toggle(DEVELOPER, "F1CE_PM_SKIP_LOG", "F1CE_PM_SKIP_LOG", "Logs the job that follows 96+ skip entries of a job queue."));
    v.push_back(toggle(DEVELOPER, "F1CE_RUMBLE_LOG", "F1CE_RUMBLE_LOG", "Logs the player-car data that drives the gamepad vibration."));
    v.push_back(toggle(DEVELOPER, "F1CE_WHEEL_LOG", "F1CE_WHEEL_LOG", "Logs the steering wheel's reports, force commands and the forces sent to it."));
    v.push_back(toggle(DEVELOPER, "F1CE_DUMP_SHADERS", "F1CE_DUMP_SHADERS", "Writes the generated shaders (fp_<hash>.glsl, vp_<hash>.glsl)."));
    v.push_back(header(DEVELOPER, "Timing and threads"));
    v.push_back(choice(DEVELOPER, "F1CE_PIN", "F1CE_PIN", {{"Default", nullptr}, {"0: no pinning", "0"}, {"1: main PPU too", "1"}}, 0,
                       "Thread pinning to cores."));
    v.push_back(toggle(DEVELOPER, "F1CE_60FPS_NOHOLD", "F1CE_60FPS_NOHOLD", "With 60 fps: a stopped car in a race is not held at 30 fps."));
    v.push_back(toggle(DEVELOPER, "F1CE_TICKS_FIXED", "F1CE_TICKS_FIXED", "With 60 fps: exactly one tick per frame whatever the time."));
    v.push_back(toggle(DEVELOPER, "F1CE_SHAKE_FILTER=0", "F1CE_SHAKE_FILTER", "With 60 fps: turns off the grid shake filter.", "0"));
    v.push_back(toggle(DEVELOPER, "F1CE_SPIN_WAIT", "F1CE_SPIN_WAIT", "Spinning waits as before 2026-11-06 (A/B comparisons)."));
    v.push_back(toggle(DEVELOPER, "F1CE_TASK_THREADS", "F1CE_TASK_THREADS", "A new thread per SPURS task as before 2026-11-06."));
    v.push_back(toggle(DEVELOPER, "F1CE_PRIO=0", "F1CE_PRIO", "All threads at normal priority as before 2026-11-06.", "0"));
    v.push_back(toggle(DEVELOPER, "F1CE_SPURS_POLL", "F1CE_SPURS_POLL", "Polls every SPURS workload continuously instead of only signalled ones."));
    v.push_back(header(DEVELOPER, "SPU and job manager"));
    v.push_back(toggle(DEVELOPER, "F1CE_PM_NATIVE=0", "F1CE_PM_NATIVE", "Job manager functions as SPU code instead of their native versions.", "0"));
    v.push_back(toggle(DEVELOPER, "F1CE_PM_CHECK", "F1CE_PM_CHECK", "Every 64th native job manager call also runs interpreted on copies; logs differences."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_NOAOT", "F1CE_SPU_NOAOT", "Interprets all SPU code."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_NOCLUSTER", "F1CE_SPU_NOCLUSTER", "Single-block SPU translations only (no clusters)."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_VERIFY", "F1CE_SPU_VERIFY", "Compares every SPU block with the LS before it runs (slower)."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_NOIDIOM", "F1CE_SPU_NOIDIOM", "Runs the table-building SPU loops as translated."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_NOFASTCULL", "F1CE_SPU_NOFASTCULL", "Geometry job transform and cull as translated, not native."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_CULL_CHECK", "F1CE_SPU_CULL_CHECK", "Every 64th transform/cull also runs interpreted on copies; logs differences."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_NOCULL", "F1CE_SPU_NOCULL", "Geometry jobs keep all triangles (fewer SPU, more renderer work)."));
    v.push_back(toggle(DEVELOPER, "F1CE_NO_LINEGEN", "F1CE_NO_LINEGEN", "SPU PUTLLC compares line data only (no write generation)."));
    v.push_back(toggle(DEVELOPER, "F1CE_SPU_BENCH", "F1CE_SPU_BENCH", "SPU interpreter benchmark and shufb self-test."));
    v.push_back(text(DEVELOPER, "F1CE_SPU_DUMP_BLOCKS", "File: records SPU blocks without a translation (input of tools/spu_aot.py)."));
    v.push_back(text(DEVELOPER, "F1CE_HOST_PROFILE", "Seconds: then samples the SPU worker threads for 10 s (hottest host functions)."));
    v.push_back(text(DEVELOPER, "F1CE_SPU_PROFILE", "Seconds after the SPU workers start: samples workload and SPU pc every ms for 10 s."));
    v.push_back(text(DEVELOPER, "F1CE_SPURS_SPUS", "SPU worker threads (default 5; 6 works, 7 and 8 hang at boot)."));
    v.push_back(text(DEVELOPER, "F1CE_CPUS", "Runs on the first n logical processors only (8 = Steam Deck class)."));
    v.push_back(header(DEVELOPER, "Renderer"));
    v.push_back(toggle(DEVELOPER, "F1CE_SYNC_COMPILE", "F1CE_SYNC_COMPILE", "Links shader programs on the render thread."));
    v.push_back(toggle(DEVELOPER, "F1CE_NO_PRECOMPILE", "F1CE_NO_PRECOMPILE", "Skips the start-up shader phase."));
    v.push_back(toggle(DEVELOPER, "F1CE_GL_FINISH", "F1CE_GL_FINISH", "Waits for the GPU every frame (tells GPU-bound from CPU-bound)."));
    v.push_back(toggle(DEVELOPER, "F1CE_FIFO_VERIFY", "F1CE_FIFO_VERIFY", "Re-reads each RSX command header shortly after it ran."));
    v.push_back(toggle(DEVELOPER, "F1CE_VK_VALIDATION", "F1CE_VK_VALIDATION", "Vulkan validation layers (needs the Vulkan SDK)."));
    v.push_back(toggle(DEVELOPER, "F1CE_CUBE_SKIP", "F1CE_CUBE_SKIP", "Never renders the reflection cube map (measurement only)."));
    v.push_back(toggle(DEVELOPER, "F1CE_TEX_CPU", "F1CE_TEX_CPU", "Texture decoding on the CPU."));
    v.push_back(toggle(DEVELOPER, "F1CE_VTX_CPU", "F1CE_VTX_CPU", "Vertex attributes converted on the CPU."));
    v.push_back(toggle(DEVELOPER, "F1CE_NO_IDX_ARENA", "F1CE_NO_IDX_ARENA", "An element buffer per index list."));
    v.push_back(toggle(DEVELOPER, "F1CE_TEX_NOMEMO", "F1CE_TEX_NOMEMO", "No per-unit texture memo."));
    v.push_back(toggle(DEVELOPER, "F1CE_TEX_NOSKIP", "F1CE_TEX_NOSKIP", "Renderer cache off (to bisect rendering bugs)."));
    v.push_back(toggle(DEVELOPER, "F1CE_TEX_NOSTATIC", "F1CE_TEX_NOSTATIC", "Renderer cache off (to bisect rendering bugs)."));
    v.push_back(toggle(DEVELOPER, "F1CE_VTX_NOCACHE", "F1CE_VTX_NOCACHE", "Renderer cache off (to bisect rendering bugs)."));
    v.push_back(toggle(DEVELOPER, "F1CE_VTX_NOSTATIC", "F1CE_VTX_NOSTATIC", "Renderer cache off (to bisect rendering bugs)."));
    v.push_back(toggle(DEVELOPER, "F1CE_VTX_NOSUBDATA", "F1CE_VTX_NOSUBDATA", "Renderer cache off (to bisect rendering bugs)."));
    v.push_back(toggle(DEVELOPER, "F1CE_RAIN_SMOOTH=0", "F1CE_RAIN_SMOOTH", "Screen rain drops without the 3x3 tent filter.", "0"));
    v.push_back(text(DEVELOPER, "F1CE_CUBE_FACES", "Reflection cube map faces rendered per frame, in turn (1-6, default 6)."));
    v.push_back(text(DEVELOPER, "F1CE_MAX_FRAMES_AHEAD", "Frames the PPU may queue ahead of the renderer (default 1)."));
    v.push_back(text(DEVELOPER, "F1CE_SKIP_FN", "Measurement only: these render functions (hex, comma separated) return at once."));
    v.push_back(header(DEVELOPER, "Captures and tests"));
    v.push_back(choice(DEVELOPER, "F1CE_AI_DRIVE", "F1CE_AI_DRIVE",
                       {{"Off", nullptr}, {"F8 toggles AI driving", "key"}, {"AI from the start (F8 toggles)", "0"}}, 0,
                       "The AI drives the player's car, as after a black flag: F8 toggles it in a race."));
    v.push_back(text(DEVELOPER, "F1CE_DUMP_FRAMES", "Saves every N-th frame as frame_*.argb (tools/frames_to_png.py)."));
    v.push_back(text(DEVELOPER, "F1CE_DUMP_WINDOW", "Saves the window after presentation every n-th frame."));
    v.push_back(text(DEVELOPER, "F1CE_TRACE_FRAME", "Logs every draw of frame n (program, target, state, textures)."));
    v.push_back(text(DEVELOPER, "F1CE_FLIP_LOG", "<first>,<count>: per flip, displayed buffer and draws per render target."));
    v.push_back(text(DEVELOPER, "F1CE_MEM_DUMP", "Input times (s,s,..): saves the data segment and user memory as mem_<s>.bin."));
    v.push_back(text(DEVELOPER, "F1CE_QUIT_AFTER_3D", "Exits that many seconds after the first 3D scene (saves the last frame)."));
    v.push_back(text(DEVELOPER, "F1CE_INPUT", "Scripted pad presses, e.g. 9:start,13:cross (seconds since start)."));
    v.push_back(text(DEVELOPER, "F1CE_INPUT_RECORD", "File: records the player's input in F1CE_INPUT syntax."));
    v.push_back(text(DEVELOPER, "F1CE_AUDIO3D_HZ", "Updates/s: each 3D sound source (volume, pan, Doppler) at most n times a second (30 = PS3)."));
    v.push_back(text(DEVELOPER, "F1CE_ENGINE_SND_HZ", "Updates/s: each car's engine sound (grain engine) at most n times a second."));
    {
        Option o = text(DEVELOPER, "extra", "Any other variables: NAME=value, separated by spaces.");
        o.label = "Other variables";
        v.push_back(o);
    }
    return v;
}

std::vector<Option> g_options;

// graphics presets: values of the PRESET_ENVS options (nullptr: the default choice), in the order of the preset
// choices, Custom last. Measured on the 1080p grid (2026-11-10): Low/Medium/Original within noise, High ~-9 fps at 60
const char* const PRESET_ENVS[] = {"F1CE_MSAA",    "F1CE_SHADOW_RES", "F1CE_CUBE_HZ",     "F1CE_CUBE_RES",
                                   "F1CE_TEX_RES", "F1CE_CAR_LOD",    "F1CE_MOTION_BLUR", "F1CE_HEAT_HAZE"};
constexpr int PRESET_N = sizeof(PRESET_ENVS) / sizeof(PRESET_ENVS[0]);
const char* const PRESETS[][PRESET_N] = {
    {nullptr, "1", nullptr, "1", nullptr, nullptr, nullptr, nullptr},   // Original
    {"2", "0.5", "15", "0.5", "2", "0.5", "0", "0"},                    // Low
    {"2", "1", "20", "1", "1", nullptr, nullptr, nullptr},              // Medium
    {"8", "2", "0", "2", nullptr, "2", nullptr, nullptr},               // High
};
constexpr int PRESET_CUSTOM = sizeof(PRESETS) / sizeof(PRESETS[0]);
const char* const CUSTOM_SECTION = "custom preset";

const char* value(const Option& o) { return o.kind == CHOICE ? o.choices[o.sel].value : nullptr; }
bool same(const char* a, const char* b) { return a && b ? !strcmp(a, b) : a == b; }
void select_value(Option& o, const char* v) {   // unknown values leave the selection alone
    for (size_t i = 0; i < o.choices.size(); i++)
        if (same(o.choices[i].value, v)) {
            o.sel = (int)i;
            return;
        }
}
bool is_preset_option(const Option& o) {
    for (const char* e : PRESET_ENVS)
        if (!strcmp(o.key, e)) return true;
    return false;
}
int matching_preset() {
    for (int p = 0; p < PRESET_CUSTOM; p++) {
        bool ok = true;
        for (int i = 0; i < PRESET_N && ok; i++) ok = same(value(*find(PRESET_ENVS[i])), PRESETS[p][i]);
        if (ok) return p;
    }
    return PRESET_CUSTOM;
}
void save_custom(Ini& ini) {   // "" for the unset choice (a missing key: never saved)
    for (const char* e : PRESET_ENVS) {
        const char* v = value(*find(e));
        ini.set(CUSTOM_SECTION, e, v ? v : "");
    }
}
void apply_preset(int p, const Ini& ini) {
    if (p < PRESET_CUSTOM) {
        for (int i = 0; i < PRESET_N; i++) select_value(*find(PRESET_ENVS[i]), PRESETS[p][i]);
        return;
    }
    for (const char* e : PRESET_ENVS)
        if (const std::string* v = ini.get(CUSTOM_SECTION, e)) select_value(*find(e), v->empty() ? nullptr : v->c_str());
}

// ---------------------------------------------------------------- season packs, GPUs
void add_season_packs() {
    static std::vector<std::string> names;   // (stable: the choices are copies anyway)
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(P(g_exe_dir) / "MODS", ec))
        if (e.is_directory(ec) && S(e.path().filename())[0] != '.') names.push_back(S(e.path().filename()));
    std::sort(names.begin(), names.end());
    Option& o = *find("F1CE_MOD_PACK");
    for (const std::string& n : names) o.choices.push_back({n, nullptr});
    // (the value points into names: kept alive)
    for (size_t i = 1; i < o.choices.size(); i++) o.choices[i].value = names[i - 1].c_str();
}

// one choice per GPU (DXGI adapter, software ones left out); the value is its PCI ids "vvvv:dddd" (hex), which the
// Vulkan renderer looks for (runtime/rsx_vk.cpp). OpenGL takes the GPU from Windows' graphics preference of f1ce.exe
// (set_gpu_preference), which names only the power-saving or the high-performance GPU: any other is Vulkan only.
struct Gpu { std::string label, value; int pref; };   // pref: Windows' GpuPreference, 1 power saving, 2 high performance
std::vector<Gpu> g_gpus;
void add_gpus() {
#ifdef _WIN32
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&f))) return;   // (before Windows 10 1803)
    auto desc = [](IDXGIAdapter1* a) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        return d;
    };
    auto first = [&](DXGI_GPU_PREFERENCE p) {
        IDXGIAdapter1* a = nullptr;
        return SUCCEEDED(f->EnumAdapterByGpuPreference(0, p, __uuidof(IDXGIAdapter1), (void**)&a)) ? desc(a).AdapterLuid : LUID{};
    };
    const LUID fast = first(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE), low = first(DXGI_GPU_PREFERENCE_MINIMUM_POWER);
    auto eq = [](LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; };
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        const DXGI_ADAPTER_DESC1 d = desc(a);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        char v[16];
        snprintf(v, sizeof(v), "%04x:%04x", d.VendorId, d.DeviceId);
        if (std::any_of(g_gpus.begin(), g_gpus.end(), [&](const Gpu& g) { return g.value == v; })) continue;
        g_gpus.push_back({U(d.Description), v, eq(d.AdapterLuid, fast) ? 2 : eq(d.AdapterLuid, low) ? 1 : 0});
    }
    f->Release();
#endif
    if (g_gpus.size() > 1)
        for (Gpu& g : g_gpus) g.label += g.pref == 2 ? " (high performance)" : g.pref == 1 ? " (power saving)" : " (Vulkan only)";
    Option& o = *find("F1CE_GPU");
    for (const Gpu& g : g_gpus) o.choices.push_back({g.label, g.value.c_str()});
}

#ifdef _WIN32
// Windows' per-application graphics preference (Settings > System > Display > Graphics): "GpuPreference=n;" in the
// value named after f1ce.exe under HKCU\Software\Microsoft\DirectX\UserGpuPreferences, the GPU OpenGL runs on.
// Automatic asks for the high-performance GPU but keeps a preference the player set there themselves; the one the
// launcher wrote last is remembered in the ini to tell them apart.
void set_gpu_preference(const std::wstring& exe, Ini& ini) {
    const char* v = value(*find("F1CE_GPU"));
    int want = 2;
    for (const Gpu& g : g_gpus)
        if (v && g.value == v) want = g.pref;
    if (!want) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &k, nullptr))
        return;
    wchar_t b[512] = {};
    DWORD size = sizeof(b) - sizeof(wchar_t), type = 0;
    std::wstring cur;
    if (!RegQueryValueExW(k, exe.c_str(), nullptr, &type, (BYTE*)b, &size) && type == REG_SZ) cur = b;
    const size_t at = cur.find(L"GpuPreference=");
    const int have = at == std::wstring::npos ? 0 : _wtoi(cur.c_str() + at + 14);
    if (!v && have && have != ini.get_int("game", "gpu_preference", 0)) {
        RegCloseKey(k);
        return;
    }
    if (have != want) {   // (the value's other entries are kept)
        const std::wstring entry = L"GpuPreference=" + std::to_wstring(want) + L";";
        if (at == std::wstring::npos) {
            cur += entry;
        } else {
            const size_t end = cur.find(L';', at);
            cur.replace(at, end == std::wstring::npos ? std::wstring::npos : end + 1 - at, entry);
        }
        RegSetValueExW(k, exe.c_str(), 0, REG_SZ, (const BYTE*)cur.c_str(), DWORD((cur.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
    ini.set("game", "gpu_preference", std::to_string(want));
}
#endif

// f1ce is built for x86-64-v3 (ARCH_FLAGS in CMakeLists.txt): on an older CPU it dies with an illegal instruction
// before it can open its log, so name what is missing here instead
std::string missing_cpu_features() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    std::string m;
    auto need = [&](bool ok, const char* name) { if (!ok) m += (m.empty() ? "" : ", ") + std::string(name); };
    if (!__get_cpuid(1, &a, &b, &c, &d)) return "CPUID";
    const bool osxsave = c & bit_OSXSAVE;
    unsigned lo = 0, hi = 0;
    if (osxsave) __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    need(c & bit_SSE42, "SSE4.2");
    need(c & bit_POPCNT, "POPCNT");
    need((c & bit_AVX) && osxsave && (lo & 6) == 6, "AVX");
    need(c & bit_FMA, "FMA3");
    need(c & bit_F16C, "F16C");
    need(c & bit_MOVBE, "MOVBE");
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) b = 0;
    need(b & bit_AVX2, "AVX2");
    need(b & bit_BMI, "BMI1");
    need(b & bit_BMI2, "BMI2");
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) c = 0;
    need(c & bit_LZCNT, "LZCNT");
    return m;
#else
    return {};
#endif
}

void set_env(const char* name, const char* v) {
#ifdef _WIN32
    SetEnvironmentVariableW(W(name).c_str(), v ? W(v).c_str() : nullptr);
#else
    if (v) setenv(name, v, 1);
    else unsetenv(name);
#endif
}
}  // namespace

std::string Option::value_label() const {
    switch (kind) {
        case CHOICE: return choices[sel].label;
        case TOGGLE: return sel ? "On" : "Off";
        case TEXT: return text.empty() ? "(not set)" : text;
        default: return {};
    }
}

std::vector<Option>& all() { return g_options; }
Option* find(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

void init(const std::string& exe_dir) {
    g_exe_dir = exe_dir;
    while (!g_exe_dir.empty() && (g_exe_dir.back() == '/' || g_exe_dir.back() == '\\')) g_exe_dir.pop_back();
    g_options = make_options();
    {   // older versions ran the game in the parent folder: copy the saves from there once (the old folder is left alone)
        std::error_code ec;
        const fs::path data = P(g_exe_dir) / "ps3_data", old = P(g_exe_dir).parent_path() / "ps3_data";
        if (!fs::exists(data, ec) && fs::is_directory(old / "dev_hdd0" / "home", ec))
            fs::copy(old, data, fs::copy_options::recursive, ec);
    }
    add_season_packs();
    add_gpus();
}
const std::string& exe_dir() { return g_exe_dir; }
bool dev_mode() { return g_dev_mode; }
std::string ini_path() { return S(P(g_exe_dir) / "f1ce_launcher.ini"); }
std::string& game_path() { return g_game_path; }

void load(const Ini& ini) {
    std::error_code ec;
    const fs::path def = fs::absolute(P(g_exe_dir).parent_path() / ".." / "F1_CE_PS3_og" / "PS3_GAME", ec).lexically_normal();
    g_game_path = ini.get("game", "path", S(def));
    g_dev_mode = ini.get_int("game", "F1CE_DEV_MODE", 0) != 0 || (getenv("F1CE_DEV_MODE") && atoi(getenv("F1CE_DEV_MODE")));
    for (Option& o : g_options) {
        if (o.kind == CHOICE) {
            o.sel = o.def;
            if (const std::string* v = ini.get("options", o.key))
                for (size_t i = 0; i < o.choices.size(); i++)
                    if (o.choices[i].value && *v == o.choices[i].value) o.sel = (int)i;
        } else if (o.kind == TOGGLE) {
            o.sel = ini.get_int("options", o.key, 0) != 0;
        } else if (o.kind == TEXT) {
            o.text = ini.get("developer", o.key, "");
        }
    }
    // a named preset whose options were changed in the ini (or an ini from before presets) shows what matches
    Option& preset = *find("preset");
    if (preset.sel != PRESET_CUSTOM && matching_preset() != preset.sel) preset.sel = matching_preset();
}

void save(Ini& ini) {
    ini.set("game", "path", g_game_path);
    for (const Option& o : g_options) {
        if (o.kind == CHOICE) {
            if (const char* v = value(o)) ini.set("options", o.key, v);
            else ini.erase("options", o.key);   // (the default)
        } else if (o.kind == TOGGLE) {
            ini.set("options", o.key, o.sel ? "1" : "0");
        } else if (o.kind == TEXT) {
            if (o.text.empty()) ini.erase("developer", o.key);
            else ini.set("developer", o.key, o.text);
        }
    }
    if (find("preset")->sel == PRESET_CUSTOM) save_custom(ini);
}

void changed(Option& o, Ini& ini) {
    Option& preset = *find("preset");
    if (&o == &preset) {
        apply_preset(preset.sel, ini);
    } else if (is_preset_option(o)) {
        preset.sel = PRESET_CUSTOM;
        save_custom(ini);
    }
}

std::string game_path_problem() {
    std::error_code ec;
    const fs::path g = P(g_game_path);
    if (g_game_path.empty()) return "Choose the game's PS3_GAME folder.";
    if (!fs::is_regular_file(g / "USRDIR" / "EBOOT.BIN", ec) && !fs::is_regular_file(g / "PARAM.SFO", ec))
        return "This folder does not look like the game's PS3_GAME folder.";
    if (!fs::is_regular_file(g / "USRDIR" / "EBOOT.elf", ec))
        return "USRDIR/EBOOT.elf is missing: the game needs the decrypted executable next to EBOOT.BIN.";
    return {};
}

std::string gpu_summary() {
    const Option& r = *find("F1CE_RENDERER");
    const Option& g = *find("F1CE_GPU");
    std::string gpu;
    if (g.sel > 0) gpu = g_gpus[g.sel - 1].label.substr(0, g_gpus[g.sel - 1].label.find(" ("));
    else
        for (const Gpu& x : g_gpus)
            if (gpu.empty() || x.pref == 2) gpu = x.label.substr(0, x.label.find(" ("));
    return r.choices[r.sel].label + (gpu.empty() ? "" : " · " + gpu);
}

std::string start_game(Ini& ini, std::function<void(unsigned)> exited) {
    if (const std::string m = missing_cpu_features(); !m.empty())
        return "This CPU cannot run the game: it lacks " + m +
               ".\nF1 CE needs a processor with AVX2 (Intel Core 4th generation \"Haswell\" / AMD Ryzen or newer).";
    if (std::string p = game_path_problem(); !p.empty()) return p;
    save(ini);
#ifdef _WIN32
    const std::wstring exe = W(g_exe_dir) + L"\\f1ce.exe";
    set_gpu_preference(exe, ini);
#endif
    ini.save(ini_path());
    std::vector<std::string> args;
    // the variable is cleared when unset here: some options act as soon as it exists, whatever its value
    for (const Option& o : g_options) {
        if (o.page == DEVELOPER && !g_dev_mode) {   // (hidden: as if unset)
            if (o.key[0] != '-' && strcmp(o.key, "extra")) set_env(o.key, nullptr);
            continue;
        }
        if (o.kind == CHOICE && strcmp(o.key, "preset")) set_env(o.key, value(o));
        if (o.kind == TOGGLE) {
            if (o.key[0] == '-') { if (o.sel) args.push_back(o.key); }
            else set_env(o.key, o.sel ? o.on : nullptr);
        }
        if (o.kind == TEXT && strcmp(o.key, "extra")) set_env(o.key, o.text.empty() ? nullptr : o.text.c_str());
    }
    set_env("F1CE_MODS_DIR", S(P(g_exe_dir) / "MODS").c_str());
    {   // extra: NAME=value tokens
        const std::string extra = (g_dev_mode ? find("extra")->text : std::string()) + " ";
        size_t s = 0;
        while ((s = extra.find_first_not_of(" \t", s)) != std::string::npos) {
            const size_t e = extra.find_first_of(" \t", s);
            const std::string tok = extra.substr(s, e - s);
            const size_t eq = tok.find('=');
            if (eq != std::string::npos && eq > 0) set_env(tok.substr(0, eq).c_str(), tok.substr(eq + 1).c_str());
            s = e;
        }
    }
#ifdef _WIN32
    std::wstring cmd = L"\"" + exe + L"\" \"" + W(g_game_path) + L"\"";
    for (const std::string& a : args) cmd += L" " + W(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, W(g_exe_dir).c_str(),
                        &si, &pi))
        return "Could not start " + U(exe) + " (error " + std::to_string(GetLastError()) + ").";
    CloseHandle(pi.hThread);
    std::thread([h = pi.hProcess, exited] {
        WaitForSingleObject(h, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        CloseHandle(h);
        exited(code);
    }).detach();
#else
    const std::string exe = g_exe_dir + "/f1ce";
    std::vector<std::string> argv_s = {exe, g_game_path};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);
    if (chdir(g_exe_dir.c_str()) != 0) return "Could not enter " + g_exe_dir + ".";
    pid_t pid;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return "Could not start " + exe + ".";
    std::thread([pid, exited] {
        int st = 0;
        waitpid(pid, &st, 0);
        exited(WIFEXITED(st) ? (unsigned)WEXITSTATUS(st) : 0x80000000u | (unsigned)WTERMSIG(st));
    }).detach();
#endif
    return {};
}

std::string exit_message(unsigned code) {
    if (!code) return {};
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08X", code);
    std::string m = "The game stopped with code " + std::to_string(code) + " (" + hex + ").\n";
    if (code == 0xC000001D) m += "Illegal instruction: this CPU lacks an instruction set the game needs (AVX2).\n";
    return m + "See " + S(P(g_exe_dir) / "f1ce_recomp.log");
}
}  // namespace opts
