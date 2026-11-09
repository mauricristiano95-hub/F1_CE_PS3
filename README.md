# F1 Championship Edition (PS3) – static recompilation

Translates the game's PPU executable (`EBOOT.elf`, PowerPC64/Cell) into C++ and links it with a
runtime that reimplements the PS3 kernel (LV2), the system libraries (HLE), the SPUs and the RSX
GPU, producing native x86-64 executables for Windows and Linux. Game data is never embedded: the
executable reads your own `PS3_GAME` folder at runtime.

## Layout

| Path | Content |
|------|---------|
| `tools/ps3elf.py` | ELF loader: segments, OPD, imports (NIDs resolved via `nid_names.txt`) |
| `tools/ppc.py` | PowerPC/AltiVec decoder |
| `tools/functions.py` | function discovery (OPD + call targets + import stubs) |
| `tools/recomp.py` | PPU → C++ translator, jump-table resolver, output in `generated/` |
| `tools/spu.py` | SPU decoder/disassembler (used to analyse the SPU code) |
| `tools/frames_to_png.py` | converts frame dumps (`frame_*.argb`) to PNG |
| `ghidra/scripts/ExportDecomp.java` | headless Ghidra export of pseudo-C for every function (`out/decomp/`) |
| `runtime/core.cpp`, `loader.cpp` | guest memory, threads, dispatch, crash diagnostics, ELF loading |
| `runtime/lv2.cpp`, `fs.cpp` | LV2 syscalls (threads, sync, events, timers, memory, tty, file system) |
| `runtime/hle_*.cpp` | HLE for the 9 imported libraries (sysPrxForUser, sys_io, sys_fs, cellSysutil, cellAudio, cellGcmSys, cellSpurs, cellUsbd, cellSysmodule) |
| `runtime/spu.cpp` | SPU execution: block cache + dispatcher, MFC DMA (incl. lists with stall-and-notify), atomics, events, interrupts |
| `runtime/spu_ops.h` | SPU instruction semantics (one template handler per instruction), shared by the interpreter and the translated code |
| `tools/spu_aot.py` | ahead-of-time SPU translation: recorded blocks → C++ in `generated/spu/` (single blocks + linked clusters) |
| `runtime/hle_spurs.cpp` | host SPURS kernel running the game's SPU policy module |
| `runtime/spu_pm.cpp` | the job manager policy module's hot functions rewritten in C++ (buffer lists, DMA info, buffer release, job end, job queue, interrupt entry) |
| `runtime/hle_frontend.cpp` | front-end additions as Lua source run in the game's own Lua state ("QUIT GAME" button), native Lua error unwinding |
| `runtime/hle_gcm.cpp` | RSX FIFO front end (jumps/calls, semaphores, labels, reports, flips) |
| `runtime/rsx_gl.cpp` | OpenGL 3.3 renderer: RSX vertex/fragment programs → GLSL, textures, blending |
| `runtime/host_sdl.cpp` | SDL3 window, keyboard/gamepad input, audio output |
| `runtime/host_null.cpp`, `rsx_null.cpp` | headless backend (`-DF1CE_HOST=NULL`) |
| `cmake/` | toolchains: Windows (llvm-mingw), Linux cross (zig, with `pkg-config.cmd` for the sysroot) |
| `linux/` | Linux packaging: `AppRun` (launcher), `make_appimage.py`, `build_from_windows.sh`, `build.sh` + `Dockerfile` (native build) |
| `tools/linux_sysroot.py`, `tools/pkgconfig.py` | Linux headers/sonames for SDL's backends in the zig cross build, and a pkg-config for them |
| `third_party/` | SDL3 sources (not in git; download `SDL3-3.4.18` there) |

## Build

Requirements: Python 3, CMake, Ninja, llvm-mingw (Windows), zig (Linux cross build), SDL3 3.4.18
sources in `third_party/SDL3-3.4.18`, the decrypted `EBOOT.elf` copied to `input/EBOOT.elf`.

```sh
python tools/recomp.py                     # regenerate generated/*.cpp
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake
cmake --build build-win -j 6
python tools/linux_sysroot.py              # once: Linux headers for SDL's X11/audio/input backends
cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-zig.cmake -DF1CE_VULKAN=ON
cmake --build build-linux -j 6
```

The Linux steps and the AppImage are also one command, `sh linux/build_from_windows.sh` (see
[Linux and Steam Deck](#linux-and-steam-deck)).

SPU code runs through translated C++ where available (`generated/spu/`, kept in the repository). To add blocks
the game executes that are not translated yet, run with `F1CE_SPU_DUMP_BLOCKS=<file>` and then
`python -I tools/spu_aot.py <file>` (blocks already translated are kept), and rebuild.

Optimised release build (PGO + ThinLTO, Windows; +4% fps on the grid, 2026-11-09):

```sh
cmake -S . -B build-pgo-gen -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake -DF1CE_VULKAN=ON -DF1CE_PGO=GEN
cmake --build build-pgo-gen -j 12 --target f1ce
sh tools/pgo_train.sh                      # 6 scripted races (~8 min) -> pgo/f1ce.profdata
cmake -S . -B build-pgo -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake -DF1CE_VULKAN=ON -DF1CE_PGO=USE -DF1CE_LTO=ON
cmake --build build-pgo -j 12 --target f1ce
```

The training build writes its profile in `process_exit` (the game leaves with `_Exit`, which skips the profile
runtime's own writer). `F1CE_PGO_SKIP` lists generated files left uninstrumented: their giant functions took hours to
compile with instrumentation. A stale profile still works (changed functions just lose it), but retrain after large
changes to recomp.py or the runtime. `build-pgo` has no shader cache of its own: copy `build-win/shader_cache` next
to it or set `F1CE_CACHE_DIR`.

`F1CE_HOST` selects the platform backend (`SDL` default, `NULL` = headless).
`-DF1CE_TRACE_PC=ON` (off by default since 2026-10-31) records the guest PC of memory accesses for crash reports and
enables `F1CE_WATCH_LINE` on PPU stores; without it crash reports give the function and LR only.

## Run

```sh
f1ce <path-to>/PS3_GAME [--trace]
```

Or use `build-win/f1ce_launcher.exe`: it sets the display and speed options below, remembers them in
`build-win/f1ce_launcher.ini` and starts the game from the folder above `build-win/`, without a console. Tabs: General (Video Settings, Game Settings), Quick Race Settings (`F1CE_QR_*` below) and Developer Tools (the debugging variables below, grouped, each with its description as a tooltip; value fields; "Other variables" takes any `NAME=value` list).

A 1280x720 window opens. Log in `f1ce_recomp.log`, saves in `ps3_data/` (both in the current folder).
The console shows warnings and errors only; the log file has everything (`F1CE_LOG_CONSOLE=1` prints every line,
which slows the game noticeably in Git Bash / the Windows console).

Keyboard: arrows = d-pad, **Z** = ✕, **X** = ◯, **A** = □, **S** = △, **Q/W** = L1/R1, **1/2** = L2/R2,
**Enter** = Start, **Backspace** = Select. Gamepads are supported through SDL.

## Linux and Steam Deck

Built, packaged and checked on Windows, **not run on Linux yet** (no Linux machine here).

**Building** (on Windows, Git Bash): `sh linux/build_from_windows.sh` makes `out/F1CE-x86_64.AppImage` and
`out/F1CE-x86_64.tar.gz` (the same AppImage with its executable bit, which most copies from Windows lose). Steps:
`tools/linux_sysroot.py` the first time (Debian 10 headers, `.pc` files and shared objects of X11, xcb, PulseAudio, ALSA,
libudev and D-Bus, checked against the archive's package index, into `third_party/linux-sysroot`), the zig cross build
in `build-linux/`, then `linux/make_appimage.py`. The executable needs only glibc 2.28 or newer (libc++ is linked in);
SDL loads X11, PulseAudio/ALSA, libudev, D-Bus, OpenGL and Vulkan at run time. This build has no native Wayland
(SDL makes its protocol code with `wayland-scanner`, a Linux program): it runs through XWayland on Wayland desktops and
in the Deck's game mode. On Linux, `linux/build.sh` (`--deps` prints the packages to install) or the container in
`linux/Dockerfile` builds with the system compiler and adds Wayland and PipeWire; that AppImage needs the build
machine's glibc or newer (Debian 12 image: 2.36).

`make_appimage.py` writes the SquashFS image itself (zlib, no fragments; checked by reading it back with an independent
reader) behind the AppImage type 2 runtime (downloaded once into `third_party/appimage-runtime`), strips the executable
(238 -> 40 MB; `build-linux/f1ce` keeps its debug information) and adds `build-win/shader_cache/programs.bin` if it
exists, so the first start translates every program the game has used up front (progress bar) instead of during play.
The AppImage holds code generated from the game's executable: it is for your own use, not for distribution.

**Running:** make the AppImage executable (`chmod +x`, or unpack the `.tar.gz`) and start it. The first time it looks
for `PS3_GAME` next to the AppImage (also one folder down), in `~/Games/F1CE/` and on SD cards (`/run/media/*/*/F1CE/`),
else asks for the folder (zenity or kdialog). It needs `USRDIR/EBOOT.elf`, the decrypted executable, as on Windows.

| | |
|---|---|
| `--settings` | settings dialog (zenity: renderer, resolution, full screen, vsync, MSAA, FXAA, 60 fps mode and cap, vibration, intro skip, language), then start; also the desktop entry's "Settings" action |
| `--game-dir <dir>`, `--reset`, `--where`, `--trace` | set the game folder / forget all settings / print the paths below / passed to the game |
| `~/.config/f1ce-recomp/settings.conf` | `F1CE_GAME_DIR` and any `F1CE_*` variable of this README, one `NAME=value` per line; variables set in the environment win (Steam launch options `F1CE_SCALE=2 %command%`) |
| `~/.local/share/f1ce-recomp/` | working folder: `ps3_data/` (saves), `f1ce_recomp.log` |
| `~/.cache/f1ce-recomp/shader_cache/` | shader cache (`F1CE_CACHE_DIR`; on Windows it stays next to the executable) |

**Steam Deck:** copy the AppImage (or unpack the `.tar.gz`) and `PS3_GAME` into one folder, e.g. `~/Games/F1CE/`, in
desktop mode; run it once there to pick the game folder and look at the settings; then Steam -> "Add a Non-Steam Game"
-> the AppImage, and start it in game mode. Steam Input gives the game an Xbox-style pad (✕ = A, ◯ = B, □ = X,
△ = Y); the default "Gamepad" layout fits. On a Deck (DMI board vendor Valve, product Jupiter/Galileo) unset options
default to Vulkan, 1280x800 and full screen; under gamescope the launcher also selects SDL's X11 driver. It keeps the
original 30 fps mode: 60 fps mode with a 30 fps cap paced better in the 6-CPU estimate (2026-11-06) but can freeze
(cube map skip, see the 2026-11-07 log). Choices made in the dialog are stored explicitly (`opengl`, `1`, `0`), so they
are kept on a Deck too.

### Debugging aids

| Variable / option | Effect |
|-------------------|--------|
| `--trace` | logs every HLE call and syscall |
| `F1CE_DUMP_FRAMES=N` | saves every N-th frame as `frame_*.argb` (convert with `tools/frames_to_png.py`) |
| `F1CE_INPUT="12:cross,16.5:down"` | scripted pad presses (seconds since start, 200 ms each) for automated tests |
| `F1CE_DUMP_SURFACES=740000,10000` | saves those render targets every 300 frames |
| `F1CE_DUMP_ON_PROG=<prog>` | saves the input/target surfaces around that program's first draw every 300 frames |
| `F1CE_TRACE_FRAME=<n>` | logs every draw of frame n (program, target, state, textures) |
| `F1CE_DUMP_SHADERS=1` | writes the generated GLSL (`fp_<hash>.glsl`, `vp_<hash>.glsl`) |
| `F1CE_FP_STOP=<i>:<reg>` + `F1CE_FP_STOP_HASH=<fp hash>` | outputs a register (`R3`, `H0`, `v2`, `c`, `C0`) right after instruction i of one fragment program; `F1CE_FP_STOP_SCALE`, `F1CE_FP_STOP_NAN` |
| `F1CE_FP_DEBUG=t` | shows unit 0's texture instead of the shaded colour |
| `tools/dbg_runs.sh` | runs several debug sessions in parallel up to the profile screen |
| `tools/race_test.sh <name> [VAR=value ...]` | Quick Race test in `dbg/<name>`, booted with `F1CE_SKIP_INTRO=1` (English); quits `QUIT_AFTER` (default 10) s after the race scene appears |
| `F1CE_QUIT_AFTER_3D=<s>` | exits that many seconds after the first frame with 800+ draws (saves the last frame) |
| `F1CE_QR_TRACK`, `F1CE_QR_DRIVER`, `F1CE_QR_DISTANCE`, `F1CE_QR_WEATHER`, `F1CE_QR_AI`, `F1CE_QR_FUEL`, `F1CE_QR_DAMAGE`, `F1CE_QR_FAILURES`, `F1CE_QR_PENALTIES` | Quick Race settings regardless of the menu (`runtime/hle_quickrace.cpp`; the launcher's "Quick Race Settings" tab): track folder name (`hungary`) or calendar index 0-17, driver id/surname/code (`heidfeld`, `HEI`; the team follows), distance 10/20/30/50/75/100, weather variable/sunny/cloudy/lightrain/heavyrain, AI easy/medium/hard, fuel and tyre wear off/scaled/realistic, damage/failures/penalties 0/1. Applied until the race's 3D scene, so `tools/race_test.sh` (cross only) reaches any combination; the menu keeps showing its own choices, and a setting changed in the menu (a value change within 2 s of a d-pad/stick move) is left to the menu from then on |
| `F1CE_LUA_PRINT=1`, `F1CE_LUA_LOG=1`, `F1CE_LUA_PATCH=<file>` | front-end Lua (`runtime/hle_frontend.cpp`): log the scripts' `print` output (a stub in the retail game), log every script load, run a Lua file instead of the built-in patch after each script the loader runs |
| `F1CE_SKIP_INTRO=1`, `F1CE_LANGUAGE=<name>` | starts at the "Press Start" screen: no language menu, no Studio Liverpool / SCE logos, no intro movie (`runtime/hle_boot.cpp`; launcher: General tab). The language is `english` (default), `french`, `german`, `italian`, `spanish`, `portuguese` or `finnish`; without the skip it is only the one the language menu starts on |
| `F1CE_QR_POKE=<offset>:<hex>` | experiment: writes a word into the Quick Race settings object until the race (+0x08 and +0x18 showed no effect in single runs, +0x10 = 4 crashes; an early "+0x08 changes the grid size" came from parallel runs taking another menu path) |
| `F1CE_TICK_DUMP=<s>,<ea hex>,<size hex>[,<ea>,<size>...]` | with `F1CE_60FPS`: <s> seconds into the first 3D scene, saves that guest memory on 32 (`F1CE_TICK_DUMP_FRAMES`) consecutive frames (`tick_<k>.bin`, further ranges `tick_<k>_<ea>.bin`), to find values that swing from tick to tick; keep it small (large dumps slow frames to 2 ticks) |
| `F1CE_PHYS_PROBE=<s>[,<ea>]`, `F1CE_PHYS_LOG=<s>,<ea>[,all]` | car physics probes (`runtime/hle_physics.cpp`, `PROBES` in `tools/recomp.py`): the wheel-spin update and slip solve of the car whose +0x910 is <ea>, or calls of the tyre function 0x11c150 |
| `F1CE_MEM_DUMP=<s>,<s>,..` | saves the data segment and user memory (`mem_<s>.bin`, 16 MB at 10000000 then 192 MB at 30000000) at those input times, to find game variables by comparing snapshots |
| `F1CE_PROFILE=1` | every few seconds: what each guest thread spends its time on (functions, HLE calls) |
| `F1CE_HOST_PROFILE=<s>` | after s seconds, samples the SPU worker threads for 10 s: hottest host functions and addresses, and the callers of time spent in DLLs; `F1CE_HOST_PROFILE_RSX=1` samples the renderer thread instead |
| `F1CE_SPU_PROFILE=<s>` | s seconds after the SPU workers start, samples each worker's workload and SPU pc every ms for 10 s (sees cluster code and DMA time, unlike `pc_samples`); with `F1CE_PROFILE=1` the `main-wait` line shows what the workers run while main waits for them; `F1CE_SPU_PROFILE_DUMP=<wid>` also saves the LS of a worker running that workload (`ls_w<wid>_<n>.bin`) |
| `F1CE_SPU_DMA_STATS=1` | every 10 s per SPU worker: DMA and atomic commands by workload, pc and command (count per second, mean size) |
| `F1CE_SPU_DUMP_BLOCKS=<file>` | records SPU blocks that have no translation (input of `tools/spu_aot.py`) |
| `F1CE_SPU_NOAOT=1`, `F1CE_SPU_NOCLUSTER=1` | interpret all SPU code / use single-block translations only |
| `F1CE_SPU_NOAOT_TASK=<elf hex>` | runs the SPURS tasks of that ELF (`SPURS task ... start elf=` in the log) in the interpreter, to compare it with the translation |
| log `SPU: n M interpreted instructions/s` | SPU code without a translation (workers and tasks) is running: record it with `F1CE_SPU_DUMP_BLOCKS` and run `tools/spu_aot.py` |
| `F1CE_SPU_BENCH=1` | SPU interpreter benchmark and shufb self-test |
| `F1CE_SPU_VERIFY=1` | compares every SPU block with the LS before it runs and logs mismatches (no dispatch cache, slower) |
| `F1CE_SPU_NOIDIOM=1` | runs the table-building SPU loops as translated (`IDIOMS` in `tools/spu_aot.py`; for comparison) |
| `F1CE_SPU_NOCULL=1` | the geometry jobs keep all triangles: SPU workers ~80% -> ~56% busy, but the renderer gets ~1000 more draws and 8x the indices, so fps drop (see the progress log) |
| `F1CE_FPS_CAP=<n>` | with `F1CE_60FPS`: frames of at least 1/n s (launcher: "Frame rate cap"). 30 = two ticks per frame as the original, with this mode's pacing; holds 30 on machines where the original 30 fps mode dips (Steam Deck class) |
| `F1CE_SHAKE_FILTER=0` | with `F1CE_60FPS`: turns off the shake filter (then the old 30 fps hold for a standing car applies) |
| `F1CE_SHAKE_RANGE=<lo hex>,<hi hex>` | debug: the shake filter only touches that part of the car object (to bisect side effects) |
| `F1CE_SPIN_WAIT=1`, `F1CE_TASK_THREADS=1`, `F1CE_PRIO=0` | the waits, SPURS task threads and thread priorities as before 2026-11-06 (spinning waits, a new thread per task, all at normal priority): A/B comparisons |
| `F1CE_LLAR_SPIN=1` | SPU threads polling an unchanged line with GETLLAR keep spinning instead of yielding (before 2026-11-09): A/B |
| `F1CE_CPUS=<n>` | runs on the first n logical processors only (Windows), to estimate smaller machines: 8 = 4 cores / 8 threads like a Steam Deck (whose cores are slower) |
| `F1CE_THREAD_CPU=1` | with the `cpu:` line every 5 s: the cores each thread used (threads are named) |
| `F1CE_TICKS_FIXED=1` | with `F1CE_60FPS`: exactly one tick per frame whatever the time (with frame dumps, to see per-tick motion) |
| `F1CE_USLEEP_LOG=1` | `sys_timer_usleep` requests per second by size, and the callers of short ones |
| `F1CE_AUDIO3D_HZ=<n>` | experiment: each 3D sound source (volume, pan, Doppler; fn 0x5C8260) is updated at most n times per second, on an n Hz grid (30 = the PS3's rate; the game sends one update per frame, so 60 with `F1CE_60FPS`). Launcher field |
| `F1CE_ENGINE_SND_HZ=<n>` | experiment: each car's engine sound (fn 0x4C6280, grain engine inputs and update, normally once per 256-sample mix block = 187.5/s) runs at most n times per second. Launcher field |
| `F1CE_AUDIO_CMD_LOG=1`, `F1CE_AUDIO_CMD_TRACE=<s>[,<len>]` | sound engine commands run per second and engine sound updates per car (every 10 s); trace of the vector commands (67/68 emitter position/velocity, 75-78 listener matrix) and command 15 (a per-frame triangle sweep, object +0x18) to `audio_cmds.csv`, of every 3D source update to `audio_src.csv` and of each car's grain engine input (+0x60, +0x7C..+0x8C) to `engine_snd.csv`, from s seconds after start for len seconds (default 8) |
| `F1CE_AUDIO_WAV=<file>` | records the audio output (what goes to SDL) as a 48 kHz float stereo WAV, to compare runs |
| `F1CE_AUDIO_LAG=<n>` | cellAudio copies a block n periods (5.33 ms each) after the read index passed it, so a late mix by the game still plays (default 2, at most nblock - 4; 0: copied when due, as before 2026-11-08). `audio out:` log lines count late wakes, late mixes played and lost, queue level, underruns and drops (every 10 s when any occurred) and the device clock (every minute) |
| `F1CE_PIN=0/1` | thread placement with 12+ logical processors (Windows). Default: the renderer prefers a core of its own (ideal processor, not a hard pin: pinned, it was sometimes held off for 10-30 ms and frames missed their vblank). 0: all threads left to the system; 1: the main PPU thread gets a core too |
| `F1CE_CACHE_DIR=<dir>` | shader cache folder instead of `shader_cache/` next to the executable (set by the Linux launcher: the AppImage is read-only) |
| `F1CE_NO_LOG=1` | no `f1ce_recomp.log` (launcher: "No log file"); info lines are dropped before formatting, warnings and errors still go to the console |
| `F1CE_PM_NATIVE=0` | runs the job manager policy module's functions as SPU code instead of their native versions (`runtime/spu_pm.cpp`) |
| `F1CE_PM_CHECK=1/2` | every 64th call (2: every call) of each native job manager function also runs on two copies of the SPU, interpreter against native (DMA puts only logged), and logs differences in local store, registers, MFC state and DMA commands (`pm check` lines) |
| `F1CE_JOBLIST_LOG=1` | traces how the frame job lists (w1/w2, queues 0x10C15100/0x10C29100) and the w0 queue are built (`runtime/hle_joblist.cpp`, `PROBES` in `tools/recomp.py`): PPU reserves, SPU entry writes with the writer's job, w0 grabs/ends, the frame end's waits and compaction; logs jobs written behind a queue's head, and when the watchdog sees the same job list wait twice it prints the cycles under way (`joblist:` lines). `F1CE_JOBLIST_DUMP_AT=<s>` also prints the first three cycles after that many seconds |
| `F1CE_SPU_NOFASTSEARCH=1` | runs the rain job's triangle search (04824, workload 3) as translated, not natively (`spu_fast_tri_search`) |
| `F1CE_SPU_NOFASTCULL=1` | runs the geometry job's vertex transform and triangle cull as translated, not the native versions (`spu_fast_*` in `runtime/spu_ops.h`) |
| `F1CE_SPU_CULL_CHECK=1/2` | every 64th transform, cull and rain triangle search also runs on two copies of the SPU, interpreter against native, and logs differences; 2 puts NaN, infinities and huge values into the copies' input |
| `SPU_AOT_HEAT=1 python -I tools/spu_aot.py` | builds counters per SPU block; the game writes `spu_heat.txt` every 10 s (regenerate without it afterwards) |
| `F1CE_MAX_FRAMES_AHEAD=<n>` | frames the PPU may queue ahead of the renderer (default 1) |
| `F1CE_TASK_DMA_LOG=1` | logs each distinct DMA of the SPURS tasks once (with the data of transfers up to 128 bytes); `=<elf hex>` only for the tasks of that ELF |
| `F1CE_SYNC_COMPILE=1` | link shader programs on the render thread instead of the background compile thread |
| `F1CE_DUMP_FROM=<n>`, `F1CE_DUMP_COUNT=<k>` | limit `F1CE_DUMP_FRAMES` to frames n .. n+k-1; `tools/flicker_check.py <dir>` finds flicker in such dumps |
| `F1CE_FLIP_LOG=<first>,<count>` | per flip: displayed buffer and draws/clears per render target |
| `F1CE_TRACE_COUNT=<k>` | extends `F1CE_TRACE_FRAME` to k consecutive frames |
| `F1CE_WPOS_CLIP_W_FP=<fp hash>,..` | WPOS.w = clip w for those programs only (`-<hash>,..`: all but those) |
| `F1CE_FULLSCREEN=1`, F11 / Alt+Enter | start in full screen / toggle it at any time |
| `F1CE_RUMBLE=<0..200>` | gamepad vibration strength in percent (default 100, 0 off; launcher: "Controller vibration"). Disc version 01.00 has no pad vibration; this is the DualShock 3 vibration of update 02.00 (0x3591e8 there) ported to 01.00's controller update 0x3574c0: impacts, kerbs, slipping wheels, run-off, low grip and engine stall/start, see `runtime/hle_rumble.cpp`. `F1CE_RUMBLE_LOG=1` logs its inputs 10 times a second |
| `F1CE_WINDOW=<w>x<h>`, `F1CE_STRETCH=1` | initial window size / fill the window instead of keeping the internal image's aspect ratio (black bars) |
| `F1CE_COMPILE_THREADS=<n>`, `F1CE_NATIVE_GLSL=1` | shader compile threads (default 1) / the scale-1 shader sources of before |
| `F1CE_NO_PRECOMPILE=1` | skips the start-up shader phase (the program database is still recorded) |
| `F1CE_NO_LINEGEN=1` | SPU PUTLLC compares the line data only (no write generation: the old behaviour) |
| `F1CE_SCALE=1..4`, `<w>x<h>`, `480p`, `576p` | internal resolution (x 1280x720; 1.5 = 1920x1080, 3 = 3840x2160): fractions allowed, supersampled to the window. `<w>x<h>` (640x360 to 5120x2880), at another aspect ratio: the 3D scene takes that ratio and a wider field of view, its interface stays 16:9 in the middle, menus are 16:9 between black bars (`runtime/hle_aspect.cpp`). `480p`, `576p` (`:4x3` for 4:3): the PS3's own video modes, with the game's layouts for them |
| `F1CE_MSAA=2/4/8` | samples of the targets the game antialiases (default 4, as on the RSX) |
| `F1CE_FXAA=1` | FXAA on the final image |
| `F1CE_VSYNC=1/0/-1` | wait for the display's vblank (default) / off / adaptive: late frames tear instead of waiting |
| `F1CE_60FPS=1` | up to 60 fps with the game speed unchanged (ticks per frame follow the frame time); a stopped car in a race stays at 30 fps (`F1CE_60FPS_NOHOLD=1` turns that off) |
| `F1CE_RAIN_SMOOTH=0` | the rain drops on the screen as the game's water map gives them (default: read through a 3x3 tent filter, see the 2026-11-03 rain entry) |
| `F1CE_CUBE_FACES=1..6` | reflection cube map faces rendered per frame, in turn (default 6, as on the RSX; saves renderer time only, not the SPU jobs) |
| `F1CE_CUBE_HZ=<n>` | reflection cube map updates per second at most (default 30 with `F1CE_60FPS`, as the original game at 30 fps, else every frame; 0: every frame; `runtime/hle_cubemap.cpp`); `F1CE_CUBE_SKIP=1` never renders it (measurement only) |
| `F1CE_SHADOW_RES=<k>` | shadow maps at k times their original 1024x2048 (0.5 = lighter on the GPU, 2 = sharper); default: they follow `F1CE_SCALE` like every render target |
| `F1CE_SKIP_FN=<hex>,..` | measurement only: these render functions return at once (2f141c static shadows, 2f1bec blurred shadows, 43f620 shadow map, 43ffa0 shadow to alpha, 30388c cars); all of them log their calls per second (`skipfn:`, `runtime/hle_skipfn.cpp`) |
| `F1CE_SPURS_SPUS=<n>` | SPU worker threads (default 5; 6 works, 7 and 8 hang at boot; 3 halves the grid fps) |
| `F1CE_SPU_DMA_DIAG=1` | the per-DMA diagnostics of the SPURS workloads (off: they cost a shared atomic on every DMA) |
| `F1CE_DUMP_WINDOW=<n>` | saves the window after presentation every n-th frame (`frame_w_*`) |
| `F1CE_TEX_CPU=1`, `F1CE_NO_IDX_ARENA=1`, `F1CE_TEX_NOMEMO=1` | texture decoding on the CPU / an element buffer per index list / no per-unit texture memo (comparisons) |
| `F1CE_VTX_CPU=1` | converts vertex attributes on the CPU into float4 buffers (the path before vertex pulling) |
| `F1CE_TEXEL_NEAREST`, `F1CE_SHADOW_CULL_FLIP` | old experiments, for comparison (crowd table filter, inverted shadow-map winding) |
| `F1CE_WATCH_LINE=<ea>` + `F1CE_LS_WRITERS=<ls>` + `F1CE_WATCH_SRC=<prog offset>` | follows a fragment-program constant from the SPU put back to its main-memory source and watches that line |
| `F1CE_TEX_NOSKIP`, `F1CE_TEX_NOSTATIC`, `F1CE_VTX_NOCACHE`, `F1CE_VTX_NOSTATIC`, `F1CE_VTX_NOSUBDATA`, `F1CE_IDX_NOCACHE` | switch off parts of the renderer caches (to bisect rendering bugs) |
| watchdog (always on) | every 20 s logs where each guest thread is, its current HLE call and backtrace |

## Status

- [x] Decompilation: 19,198 functions (Ghidra), 0 failures
- [x] PPU translation: 19,070 functions + 139 import stubs, all 195 used opcodes supported
- [x] LV2 kernel + HLE for all 9 imported libraries
- [x] SPU execution: interpreter + host SPURS kernel; the game's job manager and audio jobs run
- [x] RSX: FIFO front end and OpenGL renderer; the 2D frontend renders at ~58 fps
- [x] Vulkan renderer (`F1CE_RENDERER=vulkan`), on par with OpenGL; played by hand with no crashes reported
- [x] SDL3 window, keyboard/gamepad input, audio output (Windows)
- [x] Linux x86-64 cross build with SDL3 (X11, PulseAudio/ALSA, gamepads) and both renderers, AppImage with a launcher
      for desktops and the Steam Deck (2026-11-07; built and packaged on Windows, not run on Linux yet)
- [x] 3D scene of the profile screen: lighting, shadow maps, fog, post-processing chain
- [x] Profile creation (save data), Single Player menu, Quick Race at Silverstone: the race renders
- [x] SPURS tasksets: tasks run on SPU interpreters; event flags and queues shared with the SPU side
- [x] Race scene: sky, crowd, lit cars
- [x] Race performance: 30 fps cap held everywhere; with `F1CE_60FPS=1` ~54 fps on the full grid and 60 while
      driving (Vulkan, 1080p; was ~2 fps)
- [x] Scene brightness: matches RPCS3 (shadow compare direction fixed)
- [x] Resolution scaling up to 4K and other aspect ratios, MSAA/FXAA, 60 fps mode, launcher
- [x] All 18 tracks load and drive (SPU code translated for each), Quick Race settings incl. weather
- [x] Race-start / grid freeze (SPU task STUCK mailbox, fixed 2026-10-30, confirmed by the user at Monaco)
- [x] Mirrors too dark (fixed 2026-10-31), blue player car on Vulkan (fixed 2026-11-03)
- [x] Heavy rain at ~25 fps (untranslated SPU job, fixed 2026-11-03: ~38 fps on the grid)
- [x] OpenGL crash (guest memory fault) after a cold shader compile (fixed 2026-11-04)
- [ ] Save data beyond profiles (`cellSaveData*` callbacks for progress): deferred
- [ ] First run on Linux and on a Steam Deck
- [x] Screen rain drops (smoothed by `F1CE_RAIN_SMOOTH`) and the suspension arms: the user judged both faithful to the
      original on 2026-11-04 (RPCS3 is brighter overall)
- [x] Car shake at the grid with `F1CE_60FPS` (the game's own wheel-spin swing, also on the original): drawn filtered,
      physics unchanged (2026-11-06, `runtime/hle_physics.cpp`)

## Roadmap

Steps 1-7 are done; 8 is in progress, 9-11 are next.

1. **Correctness and a steady 30 fps.** *Done: the 30 fps cap holds on the full grid and while driving on every track
   (SPU code translation, renderer caches; the grid was ~23-26 fps on 2026-10-25).*
2. **Decode guest data on the GPU.** *Done 2026-10-28: vertex pulling, one index arena, texture decoding in a compute shader.* Vertex conversion (big-endian, packed formats), index list rebuilding and
   texture deswizzling run on the CPU (~15-20 ms per frame on the render thread, plus change-detection hashing):
   upload raw data and decode it in shaders / compute shaders. Also removes most of the loading stutter.
3. **Resolution scaling (up to 4K) and anti-aliasing options.** *Done 2026-10-29: `F1CE_SCALE`, `F1CE_MSAA`, `F1CE_FXAA`.* Render targets at an internal scale factor (surfaces
   the CPU reads back stay native; copies, cube map and shadow maps scaled; post-processing offsets checked);
   MSAA 8x, FXAA/SMAA on the final image, supersampling via the scale factor. GPU-side cost only.
4. **60 fps.** *Done 2026-10-31: `F1CE_60FPS=1`, adaptive ticks per frame.* How close it gets to a steady 60 is
   step 8.
5. **Vulkan backend** next to OpenGL: lower per-call driver cost, explicit pipeline caches (no stutter on new
   shaders), multithreaded command recording, better Linux support. DirectX 11 is not planned (no gain over OpenGL,
   Windows only). *Done (2026-10-31): `runtime/rsx_vk.cpp` runs the boot, menus and a Silverstone race at scale 2 at the
   30 fps cap, matching OpenGL by eye; played by hand with `F1CE_60FPS=1`, FXAA and Monaco with no crashes or glitches reported;
   never run with validation layers. It is built with
   `cmake -DF1CE_VULKAN=ON` (needs `third_party/glslang`, tag vulkan-sdk-1.4.350.1) and selected with
   `F1CE_RENDERER=vulkan`; OpenGL stays the default. Shared with OpenGL: `rsx_common.{h,cpp}` (registers, microcode
   translators, hashes, block heap, program database) and `rsx_draw_shared.inl` (index lists, raw vertex regions).*

6. **Launcher.** *Done 2026-10-31: `build-win/f1ce_launcher.exe` (`launcher/`).* A Windows `.exe` to set the options that are environment variables today (`F1CE_SCALE`,
   `F1CE_RENDERER`, `F1CE_60FPS`, ...) and start the game, replacing the shell script `launch_decomp_wwin.sh`.
7. **Shader progress text.** *Done 2026-10-31: "Loading shaders... 42%" above the bar, on both renderers.*
8. **Performance for 60 fps.** Make the game as fast as possible with `F1CE_60FPS=1`, aiming at a steady 60.
   *In progress. Full Silverstone grid, Vulkan at 1080p: 33 (2026-10-31) -> 37-38 (11-01) -> ~46 (11-03) -> ~48 (cube
   map at 30 Hz, 11-04) -> ~54 (translated SPU clusters keep registers in host registers, 11-04) -> ~57 (native job
   manager functions, 11-05) -> ~58 (its interrupt entry too). Driving holds 60 on open track. On this PC the grid now
   waits for particular jobs (the main thread's job waits), not for SPU time in total: skipping the shadow passes frees
   ~20% of SPU time and gains nothing. Less SPU work still matters on weaker CPUs (step 9). Shadow passes keep running
   every frame (any other rate breaks them visually).*
9. **Linux backend and Steam Deck.** *Built 2026-11-07: SDL window/input/audio and both renderers in the Linux build,
   an AppImage with a launcher (see [Linux and Steam Deck](#linux-and-steam-deck)); not run on Linux yet.* Next: a test on
   a Linux PC and on a Deck. An 8-thread estimate on this PC (2026-10-31) gave 26-30 fps on the grid, so the
   Deck needs less SPU work, not only a faster runtime.
10. **Remaining visual differences.** Rubber marks not checked since 2026-10-28 (see below). *Screen rain drops and the
    suspension arms were judged faithful by the user on 2026-11-04.*
11. **Save data beyond profiles.** Deferred by request.

All of these become options (environment variables or a config file) with the original behaviour as default.

### Known rendering issues (reported 2026-10-25, updated 2026-11-04)

- [x] Suspension arms in front of the cockpit darker than in RPCS3: faithful to the game, RPCS3 is brighter overall
      (user, 2026-11-04).
- [ ] Rubber marks near the corners looked much darker than on PS3: not checked again since the 2026-10-28 fixes
      (needs a driven lap).
- [x] Screen rain drops in heavy rain: faithful with `F1CE_RAIN_SMOOTH` (user, 2026-11-04; see the 2026-11-03 rain entry).
- [x] Mirrors and cockpit panels too dark (2026-10-31).
- [x] Crowd clothes wrong (FP flow control IFE, 2026-10-28), white heat haze (2026-10-28).
- [x] Blue player car on Vulkan when starting last (2026-11-03).

## Progress log

**2026-10-20** – Toolchain set up (llvm-mingw, zig, CMake, Ninja, JDK 21, Ghidra). Ghidra decompilation of
the whole executable. PPU → C++ translator with jump-table resolution. Runtime with LV2 syscalls and HLE.
First Windows build: the game runs `main`, initialises memory/SPURS/GCM, loads `offline.wad` and audio
banks, then waits for SPU work.

**2026-10-21**
- SPU interpreter (about 200 instructions, MFC DMA, atomics, events, decrementer, mailboxes).
- Host SPURS kernel, with the kernel/policy-module interface derived from the game's own SPU code:
  SPURS instance address, current workload id, exit and poll services, ready counts in the first line
  of the SPURS instance.
- Fixes found through the SPU code: MFC list stall-and-notify, SPU interrupts (E/D bits of indirect
  branches, vector at LS 0), missing `orx` instruction. The audio workload now completes its jobs and
  the main thread runs the game loop.
- SDL3 window/input/audio, then the OpenGL renderer: surfaces as FBOs, vertex arrays and indexed
  draws with primitive restart, vertex/fragment program translation, textures (ARGB, DXT, swizzled),
  blending, depth, alpha test, viewport with bottom-left window origin.
- Fixed `CellPadInfo` layout (arrays sized for 127 pads): the game now detects the controller.
- Scripted input and frame dumps for automated tests. The frontend renders: language selection,
  "Press Start", "Manage Profiles"; the game loads engine sounds, menu music and the intro video.
- Fixed a double vertical flip when presenting to the window.
- Blocked at profile creation: `cellSaveDataListSave` is not implemented yet.

**2026-10-22** – 3D rendering of the profile-creation screen (garage with the driver):
- Fragment programs: condition codes (set/test, conditional writes, conditional KIL), depth export
  (`gl_FragDepth` from R1.z), and the FOGC input computed from FOG_MODE/FOG_PARAMS as on RSX. The
  fog factor was 0, so every lit pixel collapsed to the fog colour (black screen).
- Shadow maps: depth-compare order fixed (reference FUNC stored depth); a zeta buffer sampled with a
  colour format returns its depth bits as A8R8G8B8.
- 2D engines: NV308A inline transfers (the game patches fragment-program constants in video memory
  this way; POINT is method 0x304) and NV0039 memory copies, including copies of render targets.
- New debug tools: per-instruction register output for one shader, surface dumps around a draw,
  parallel test runs.
- Later the same day: profile creation works with the existing save-data HLE; Single Player → Quick
  Race loads Silverstone. Fixed on the way:
  - jump tables whose bound check is not the nearest `cmplwi` (the index register is now tracked):
    a 9-way switch was cut to 2 cases and the game called an unknown address while loading a track;
  - `_cellSpursQueueInitialize` / `_cellSpursEventFlagInitialize` take (spurs, taskset, object, ...);
  - SPURS tasks now run (task ELF loaded at LS 0x3000, SPURS EA at LS 0x1C0, exit through the table
    at [LS 0x2FB8] + 0xC4), and queues/event flags live in guest memory with the layout the game's
    task-side library uses (derived from its SPU code), so PPU pushes reach the tasks and their event
    bits reach the PPU.
  - The race renders (cockpit, start lights, HUD, opponents) but runs at a few frames per second.
- `tools/spu_elf.py <vaddr>` extracts and disassembles an SPU ELF embedded in the executable.
- Race rendering work: texture mip chains, unnormalized texture coordinates (format bit 0x40), vertex
  texture fetch (VP opcode TXL, float textures on vertex units 0x0900+), vertex-program condition codes,
  and the command-buffer callback now also waits for return addresses of RSX CALLs (the PPU overwrote
  commands still to be executed when the RSX lagged behind). Still open: wrong geometry/streaks from
  the programs that use vertex textures, dark image, car/helmet textures, sky, crowd.
- **2026-10-23** – Race scene: vertex-attribute instancing (frequency field of the attribute format +
  FREQUENCY_DIVIDER_OPERATION 0x1FC0: `index % freq` or `index / freq`) makes the crowd billboards appear and
  removes the streaks over the sky; SPU single-precision semantics (no inf/NaN: results saturate,
  denormals flushed); at most one frame of PPU lead over the renderer (`F1CE_MAX_FRAMES_AHEAD`).
  Car bodies still show only reflections: their fragment programs are rebuilt every frame by an SPU job
  (template + patch list, DMA into video memory) and the ambient/sun constants arrive as zero; the source
  of those values is not found yet. Debug aids: `F1CE_PROG_ID`, `F1CE_SKIP_PROG`, `F1CE_SKIP_VTEX`,
  `F1CE_WATCH_FP`, `F1CE_SPU_LS_WATCH`, `F1CE_WATCH_HISTORY`, `F1CE_WATCH_DUMP_LS`, `F1CE_WATCH_MEM_SIZE`.
- Car lighting investigation (2026-10-23, continued): the car fragment programs are assembled by an SPU job
  that patches each constant component by component from per-instance material parameters (84-byte entries,
  copied by PPU function 0x446354 from the material definition); the ambient/sun entries of the cars are zero
  in main memory and nothing updates them during the race. Fixed on the way: MFC list elements smaller than
  16 bytes land at `list LSA | (EA & 15)` and the list LSA advances by max(size, 16) rounded to a quadword.
  New aids: `F1CE_LS_WRITERS`, `F1CE_SPU_PC_LOG` (+ `_R3` filter), `F1CE_AUTO_WATCH`.
- **Cars lit (2026-10-23):** the per-car sun factor comes from a 32x1 A8R8G8B8 render target that the game
  draws in *main memory* and reads back on the PPU (function 0x2FA404, table at the target's address); the
  renderer skipped colour buffers outside local memory. Main-memory colour targets are now rendered and
  written back to guest memory after every draw and clear. Chain found with the new tools: car fragment
  program -> SPU patch job -> per-frame PPU lighting block (0x3094E0: sun colour x per-car factor) -> lookup
  table filled by the GPU. Shader parameter names are hashed as `~crc32(name)`.
- `tools/race_test.sh <name>`: Quick Race test that exits 10 s after the race scene appears
  (`F1CE_QUIT_AFTER_3D`), about 60-70 s per run.
- **Speed (2026-10-23):** the race went from ~2 to ~27 fps on a 6-core Ryzen 5 5600G.
  - SPU: decoded blocks are cached per 128-byte LS line (content-aware invalidation, up to 8 variants per address
    for job code swapped in by DMA), and blocks recorded in play are translated ahead of time into C++
    (`tools/spu_aot.py`). Linked blocks form clusters (one function with direct jumps, validated once by a hash
    of its words). Clusters with loops or long blocks keep the SPU registers in host registers (`SpuLocal`).
    SPURS task contexts are reused per ELF; the reservation lock is striped per 128-byte line; DMA counters
    no longer share a cache line with the dispatcher.
  - Renderer: textures, vertex data and index lists are cached with content hashes (static data is re-checked
    every few frames); GL state, render target, program, uniforms and vertex-buffer bindings are only sent when
    they change; vertex-program constants are uploaded by changed range; render targets that alternate sizes
    at one address are kept instead of reallocated; unused buffers are evicted. Cache entries carry their full
    identity, because a colliding key on a static entry drew another mesh's data (stretched triangles).
  - Now the PPU, SPUs and renderer are all close to their limits: the main thread still waits on SPU jobs
    for about 40% of the frame, and the renderer spends ~15 ms per frame in vertex setup.
- **Flicker fixed (2026-10-23):** splash screens and menus flickered. Both came from the "static data" shortcut of
  the renderer caches (data unchanged for 60 frames was re-checked only every 8-16 frames): the splash quads live
  in a small vertex ring buffer rewritten after a long pause, and the menu background is a video whose YUV planes
  (linear textures) the decoder rewrites. The shortcut now applies only to vertex ranges over 64 KB and to
  swizzled/compressed textures. Vertex and index data seen for the first time is streamed instead of getting
  its own GL buffer (dynamic data sits at a new ring-buffer address every frame); a buffer is made when the same
  data comes back in a later frame. Race: ~27-29 fps.
- **Comparison with RPCS3 screenshots (2026-10-23):**
  - Race scene: the shadow-map test was inverted. RSX texture depth compare is "stored FUNC reference"; we used
    GL's "reference FUNC stored", so with the game's GEQUAL the player car sat in its own shadow and lost the sun
    term: washed-out car, faint decals, dark image. Now the car, asphalt and shadows match the reference.
  - Quick Race menu: the driver portrait (DXT5) is loaded into memory where the menu video used to be rendered,
    and the renderer kept sampling the old render target. Compressed textures now always come from memory.
  - Colour targets in B8 format (one byte per pixel, used by the glare pass) are kept as RGBA; a B8 texture read
    of such a target returns its blue channel.
  - New debug aids: `F1CE_TRACE_DUMP=<from>,<to>` (target after each draw in that range), `F1CE_DUMP_TEX_PROG=<prog>`
    (textures of that program's draws), `F1CE_TRACE_FC=<prog>` (its fragment constants), `F1CE_FP_STOP=<i>:f`
    (fog factor), target size and pitch in the draw trace.
- **Second RPCS3 comparison (2026-10-24):**
  - Title screen: the logo image is loaded where the menu video used to be rendered (uncompressed this time).
    Each local-memory render target now keeps a sampled fingerprint of its guest memory from when it was last
    rendered; if the CPU has rewritten that memory since, the texture comes from memory.
  - Texture sampling state was ignored: TEXTURE_FILTER bits 0-12 hold a signed LOD bias (the game uses -1.5),
    bits 16-19/24-27 the min/mag filters, CONTROL0 the anisotropy and min/max LOD. Car liveries and tyres are now
    as sharp as in RPCS3. Textures addressed in texels (format bit 0x40, e.g. the crowd's G8B8 data table) keep
    linear filtering: nearest sampling at integer texel coordinates made the crowd disappear (probably the reason
    RPCS3 shows no spectators).
  - Occlusion queries: ZPASS_PIXEL_CNT reports (SET_ZPASS_PIXEL_COUNT_ENABLE 0x1D7C, CLEAR_REPORT_VALUE 0x17C8,
    GET_REPORT type 1) are GL samples-passed queries, written into the report at the next flip. They returned 0;
    with real counts the game selects more detailed car/driver models.
  - Still different from RPCS3: cockpit side panels and mirrors look light grey (strong sky reflection from the
    256x256 environment map) where RPCS3 shows them dark and glossy. The tone-mapping exposure is a fixed 0.359.
    It is not the cause on its own: an exposure of 1.0 matches the stands but overexposes the barrier and helmet.
  - Tools: `tools/img_compare.py` (mean colour of regions against a reference screenshot),
    `tools/img_side_by_side.py` (stacked crops), `F1CE_FC_LOG=<fp offset>`, `F1CE_SCAN_FLOAT=<value>`,
    `F1CE_WB_LOCAL=1` (experiment: write small local render targets back to guest memory), FP hash in the
    program log.
- **Race crash, loading, motion blur (2026-10-24):**
  - Crash about 20 s into a race: a jump table (0x119D9C, 6 entries) was cut to 3 entries because the bound came from
    an unrelated `cmplwi` (its index is set by `li` in each path); case 3 fell into an indirect call to 0x11A624 and
    the game stopped. A bound now counts only when the compare is on the index register; otherwise the table is
    read up to the first entry that leaves the function. A 4-minute race now runs to the finish.
  - The watchdog diagnostics could fault on a racy pointer (fixed); scripted pad presses now report full pressure
    (the throttle is the pressure of cross), and `tools/race_test.sh` takes `DRIVE=1` (throttle held for the race),
    `TIMEOUT=<s>` and the `t:button:duration` input syntax.
  - White screen after loading: on the first frames of a scene the renderer linked ~110-220 GL programs (~50-110 ms
    each in the AMD driver) on the render thread, so nothing was presented for up to 25 s; a long stall like this also
    let the RSX run into a command list that was not ready (FIFO fault). Programs are now linked on a background
    thread with a shared GL context (draws that need them are skipped for those frames) and stored in
    `build-win/shader_cache/` (`glProgramBinary`), so later runs load them in ~1 ms each.
  - Motion blur everywhere: the motion-blur pass reads a depth-only render target (0x02F20000) that also has an
    unwritten colour surface at its offset; the renderer took the colour one. A colour and a depth surface at the
    same offset are now told apart by which one was written last. TEXTURE_CONTROL1 channel remaps are applied for
    A8R8G8B8/B8 data and render targets (one texture uses an ARGB->BGRA swap), but not for depth read as colour
    (the decode already gives the order the game expects).
  - Still open: weak reflections and tyre sun highlights compared with PS3 screenshots (`REFERENCE/`), some ghosting
    of distant scenery in the motion blur, ~25 fps in the race instead of a fixed 30.
- **Frame rate, reflections, lighting (2026-10-24):**
  - Vertex data is cached per buffer region (attribute base address + format) instead of per draw range, and index
    lists are built in one pass and drawn with a base vertex (`glDrawElementsBaseVertex`) instead of being rebased.
    Index buffers that the game rewrites every frame (most of the ~450k indices per frame) cannot be cached; static
    ones use index regions per 16K-index chunk, and regions whose content keeps changing fall back to per-draw lists.
    A `getenv` left in the per-draw uniform path cost ~5-8 ms per frame (Windows `getenv` is slow); debug switches
    are read once into statics. The race start (full grid, ~3000 draws per frame) runs at ~25 fps, later in the race
    27-30 (the game's cap is 30); vertex setup (~15 ms) is the largest remaining render-thread cost.
  - Environment reflections: the game renders a 256x256 cube map every frame (six faces one face size apart,
    double-buffered at 0x04060000 / 0x041E0000) and samples it with a cube texture (TEXTURE_FORMAT bit 2), which
    the renderer did not support. The faces are now blitted into a GL cube map and the units declared `samplerCube`
    (a program variant per cube mask). Cockpit side panels and mirror housings are dark and glossy as on PS3.
  - Lighting: two TEXTURE_ADDRESS fields were ignored. Bits 12-15 = 1 (unsigned remap BIASED, x * 2 - 1) are set
    on the normal maps, so normals were wrong on every normal-mapped surface (the tyres were almost black, 21 vs 72
    in RPCS3). Bits 20-23 decode R/G/B/A from sRGB, and SHADER_PACKER (0x1FD8) bit 0 encodes the output to sRGB:
    the game lights in linear space. With both, the onboard view matches RPCS3 within ~10% per region (tyres, white
    bodywork, helmet, sky, asphalt) and the image has the original's contrast. `F1CE_NO_TEXGAMMA`,
    `F1CE_NO_TEXBIAS` and `F1CE_NO_CUBE` switch them off for comparison.
  - The watchdog crashed the game when its diagnostic dump followed a register into an unmapped guest range (seen
    once during the menu video); it now checks the committed ranges (`mem::readable`).
  - Command lists reached by CALL are not bounded by PUT; twice the RSX front end read a stale word of a list the
    game was still writing (a jump to an unmapped offset, fatal). A jump/call to an unmapped offset now waits up to
    500 ms for the word to be rewritten before failing.
  - Debug: `F1CE_TRACE_DUMP_FP=<fp ctl>,...` dumps the target after the draws of chosen fragment programs (draw
    numbers vary between runs), `F1CE_GL_FINISH=1` logs the GPU wait per frame.
- **Video colours, motion blur, render-thread cost (2026-10-24, later):**
  - sRGB output: colour surfaces are now `GL_SRGB8_ALPHA8` (sampled raw, `GL_SKIP_DECODE_EXT`) and drawn with
    `GL_FRAMEBUFFER_SRGB`, so the GPU encodes and blends in linear space. SHADER_PACKER bit 0 stays set for the
    whole game, yet the intro/menu video (YUV planes without gamma bits, copied to the display buffer by a pass
    that reads it raw) came out washed out and magenta when encoded. Encoding is applied to draws that sample
    sRGB-decoded textures and do not copy a raw (non-decoded) colour target: the 3D passes write encoded colour
    that the post passes read back with gamma bits, the video chain stays raw. Video, menus and race now all match
    RPCS3/PS3 screenshots.
  - Motion blur: the pass that smears the screen rebuilds depth from the depth buffer read as A8R8G8B8 with the
    game's 0xAA93 channel remap (A/R/G = depth high/mid/low bytes moved to R/G/B). The remap was skipped for depth
    reads, so depth came from the low bytes and distant scenery was smeared as if close; with it the blur stays at
    the screen edges as in the PS3 footage, and the dotted look on nearby cars is gone.
  - Render thread: the 2D-subchannel method histogram (a `std::map` update per inline-transfer word and a log line
    every 2000 words) cost several ms per frame and is now behind `F1CE_SUBCH_STATS`; small vertex regions that
    have not changed for 60 frames are checked once per frame instead of per draw; streamed vertices and index lists
    go through a 64 MB persistently mapped ring (`F1CE_NO_RING`); the attribute offsets' common part moves into the
    base vertex. At the race start the renderer now waits for the game 5-9 ms per frame: the start (full grid,
    ~3000 draws) runs at 23-26 fps limited by the PPU waiting for SPU jobs (SPU workers ~88% busy, AOT coverage
    complete), later 27-30.
  - SPU dispatch: a direct-mapped cache of native block entry points (`F1CE_SPU_NOFAST`) skips the scattered block
    lookup; no measurable change at the race start, kept as it is cheaper per dispatch.
- **Stability, windscreen, reflections (2026-10-24, evening):**
  - FIFO crashes (entering DRIVER in the Quick Race menu, before the race start): the game builds each frame into
    one of two alternating command lists reached from a 4-slot ring (0x10000-0x14000) and rewrites a list without
    waiting for the RSX. When the renderer stalled, the PPU got ahead, rewrote a list the RSX was still running
    and the FIFO decoded garbage. Fixes: synchronisation writes (reference, semaphore releases) become visible only
    after the next command is fetched (the RSX prefetches; the game reuses a list as soon as it is released);
    the PPU's `sys_timer_usleep` waits while it is more than one ring slot ahead of the RSX (measured from the
    SET_REFERENCE at the start of each slot vs control->ref; `F1CE_NO_RSX_THROTTLE`). The crash history (last jumps
    with PUT/reference, semaphore operations) is printed with a FIFO fault. `tools/menu_test.sh` runs a custom
    input script; `F1CE_INPUT_RECORD=<file>` records the player's input in that syntax (`dbg/rec1/input_rec.txt`
    reaches DRIVER); `F1CE_RSX_STALL=<frame>:<ms>[:<n>]` simulates renderer stalls.
  - Windscreen: method 0x1D7C is ANTI_ALIASING_CONTROL (not the zpass enable): the race target (surface format AA
    bits) is 4x antialiased and see-through materials use alpha to coverage (bit 4). Antialiased targets are now
    drawn into 4x multisampled renderbuffers and resolved before they are read (`F1CE_NO_MSAA`), with
    GL_SAMPLE_ALPHA_TO_COVERAGE: the windscreen is transparent (RPCS3, without MSAA, shows it grainy), the driver's
    gloves behind it are visible again, and edges are antialiased as on PS3.
  - Reflections: the cube map faces were written sRGB-encoded (their draws sample gamma textures) while the car
    shaders read the cube without decode and add it to linear lighting, so reflections were far too strong, most
    visibly on cars in shadow. Cube faces are now written linear; the DRIVER garage scene matches RPCS3.
  - Streamed lists (later the same evening): while loading (race grid, pits) and in race frames the game streams a
    frame list with PUT inside it, then returns PUT to a ring slot and soon rewrites the list. When the renderer was
    still seconds behind in that list (the loading list, texture decoding) it ran into the rewritten part (crash
    starting a Quick Race; the PPU throttle cannot help, the game does not call usleep there). A watcher thread now
    copies the rest of a streamed list the moment PUT leaves it (only if the front end is still in it or has not
    entered it yet) and the front end runs the copy (`F1CE_NO_LIST_SNAP`). The PPU throttle waits up to 15 s and
    only applies in the ring layout. `launch_debug_pit.sh` (repo root) records input and dumps SPURS workload 5's
    control block for the pit-entry freeze (main thread waiting in 0x551228 for job manager wkl 5, which runs once,
    finds no work and exits).
  - Logging: every log line was printed to the console under the log lock and the file flushed per line; in a
    terminal (Git Bash) this stalled the renderer and the PPU enough to cause the slow loading, long white screens
    and FIFO crashes seen only by the user (tests redirect the output). The console now gets warnings/errors only and
    the file is flushed on warnings/errors and once a second. Also handled: `sys_spu_thread_send_event` from SPURS
    tasks (OutIntrMbox): acknowledged instead of blocking the SPU forever (freeze in the race: the task never finished
    and the PPU waited on its event flag).
  - Pit-entry freeze: entering the pits the game queues one job on SPURS job manager workload 5 and waits for its
    completion. The job manager took the job and ran it, but the job asserts that SPU interrupts are enabled
    (`rdch SPU_RdMachStat; andi 1; brz -> stopd`) and our MachStat channel returned 0, so it halted silently with
    `stopd` and the completion never came. SPURS workloads and tasks now report interrupts enabled (they run with
    interrupts on under the real kernel), and `stopd` is logged. Found with `F1CE_TRACE_WKL=<wid>` (block trace of a
    workload's runs) and the workload control-block dumps at ReadyCountStore / after rare runs.
- **Roadmap step 1, first pass (2026-10-25):**
  - SPU code invalidation: the job manager's interrupt handler generates 80 register-save instructions on its LS
    stack at every interrupt. Stores into decoded code counted as code changes even when the bytes were equal, and a
    line stayed marked as code forever, so ordinary stack stores invalidated every block of the SPU ~7 million times
    per second (each one revalidating all blocks and emptying the dispatch cache). Stores now invalidate only on a
    real change and clear the line's code mark (set again when code is decoded there): ~240k/s, the job manager's
    share of SPU samples fell from ~50% to ~8%, SPU workers 77-81% busy.
  - Render thread FIFO: the streamed-list reader took a mutex per command word when snapshots existed but none
    covered the address (~10% of the thread); it now keeps a local copy of the snapshot list and reads a command
    with its arguments through one pointer. `rsx::method` has a table-driven fast path for plain state writes.
  - Race start (full grid): ~25 -> 26-28 fps; the render thread is now the limit (FIFO idle 0-4 ms per frame),
    with vertex setup (~12 ms) the largest part: roadmap step 2.
- **Graphics fixes (2026-10-28):**
  - Fragment program flow control: IFE (opcode 0x42, `if (cond) { } else { }`, words 2/3 = else/endif offsets in
    32-bit words) was ignored, so both branches ran. The crowd shader picks each figure's clothes in such a branch:
    the spectators now wear coloured clothes as on PS3 instead of only skin colour.
  - Textures addressed in texels (crowd data table) are linear-filtered again by default (`F1CE_TEXEL_NEAREST`
    keeps the game's nearest filter; with it the crowd disappears).
  - Shadow maps cull front faces as requested (the inverted winding was an experiment): the start gantry casts its
    shadow across the grid again (`F1CE_SHADOW_CULL_FLIP` restores the experiment for comparison).
  - The draw trace also logs the cull state (`cull=enable/face/front`).
- **Roadmap step 2, vertices (2026-10-28): vertex pulling.** The vertex programs read the raw big-endian guest
  records themselves from two buffer textures (`u_va_ring`: the mapped stream ring; `u_va_heap`: a 256 MB vertex
  heap in video memory) and decode the seven RSX formats (S1, F, SF, UB, S32K, CMP, UB256) and instancing
  (frequency, modulo) in GLSL; `gl_VertexID` is the guest vertex index (base vertex 0). Per draw only `u_va[16]`
  (offset, stride, format, frequency) is uploaded, instead of ~2400 `glBindVertexBuffers` per frame (~1.6 us each in
  the AMD driver) and the float4 conversion. The region cache, change detection and streaming of first sightings
  are as before but keep raw bytes (2-4x less memory, ~40 MB at the race start). Heap blocks are never rewritten:
  changed data gets a new block, the old one is reused when the fence of the frame that freed it has passed. Uploads
  go through the ring and `glCopyBufferSubData`: `glBufferSubData` into the heap while draws read it stalled the
  driver (0.1 fps). Race (driving, average of 8 samples, both with `F1CE_GL_FINISH`): vertex setup 11.7 -> 7.7 ms
  per frame, 27.4 -> 29.5 fps; not GPU-bound (~2 ms wait at the flip either way). `F1CE_VTX_CPU=1` keeps the old
  path. Next for this step: one element buffer for all index lists (the index block is now the largest part of
  vertex setup, ~2.5-4 ms: per-draw GL calls ~1.2 ms, the rest hash-map lookups and min/max scans), then textures.
  The host profiler no longer allocates while a sampled thread is suspended (it could deadlock on the heap lock).
- **Fixes after the user's test (2026-10-28):**
  - Heat haze behind the cars was a white cloud: the haze pass refracts the 1280x720 scene copy into a 640x360
    buffer, and a raw read of a target counted as a copy (not sRGB-encoded) only when it had exactly the
    destination's size; now when it is at least as large.
  - Shader stutter: a cached program cost ~10 ms (`glProgramBinary`) at its first draw. Programs used in earlier
    sessions are listed in `shader_cache/used.txt` and preloaded at start-up on a second shared context (~0.4 s
    for 184 programs, in the background); first-use cost in a race 413 -> 3 ms. Programs never seen before still
    compile in the background once (`F1CE_NO_PRELOAD` turns the preload off).
  - Quitting: `sys_process_exit` used `std::exit`, which ran static destructors while the SPU workers and the
    renderer were still running (access violations in the log after every quit); it now flushes the log and
    leaves at once.
  - The Driver garage opened fine in scripted tests; the reported crash there was not reproduced.
  - Crash entering the Driver garage (user log): a session with ~130 programs new to the cache. The compile thread
    linked in a context sharing objects with the renderer's, and the AMD driver held up every renderer GL call
    during each 50-100 ms link: 0.3 fps for tens of seconds, the PPU overran the FIFO (jump to 0, semaphore
    timeout with an address as value, fault). The compile thread now uses a context of its own share group and
    hands over the program binary; the renderer loads at most two binaries per frame. Cold-compile test
    (`F1CE_SHADER_SALT=<text>` makes every program new, with `F1CE_NO_PRELOAD=1`): 0.3 -> 26-30 fps while compiling.
    KHR_parallel_shader_compile is exposed by the driver (RX 6600) but compiles on the calling thread: worse.
  - Still seen: `glError=502` after ~100 first draws per run (present since before 2026-10-28) and FP opcode 0x3F
    (not a documented RSX opcode) in a few garage programs.
  - FIFO fault in a race (user log, no stall: ~29 fps): the front end ran a CALL at 006d9b8c with the target of two
    frames earlier and hit rewritten memory. A copy of a streamed list (taken when PUT leaves it) stayed queued
    because the front end never left it at call depth 0, and kept covering its addresses (the race streams each
    frame list from 0x55d100 to ~0x6f0000). Copies now carry the reference value of the slot PUT returned to and
    are ignored and dropped once the front end is two frames past them ("dropped as stale" in the log). Not
    reproduced in two 70 s driving runs (no stale copy occurred there).
- **Roadmap step 2 completed (2026-10-28): index lists and textures.**
  - Index arena: all index lists in one persistently mapped 96 MB buffer, bound once as the VAO's element buffer:
    a 16 MB ring (four fenced segments) for lists used by one draw and a block heap (the vertex heap's allocator,
    now generic: `BlockHeap`) for cached lists and index regions, written through the mapping. Replaces a buffer per
    list (a `glBindBuffer` per draw, `glBufferData` per rebuild). Race start: index block 3.8 -> 2.4 ms per frame.
  - Texture decoding: non-DXT formats (A8R8G8B8, B8, A4R4G4B4, A1R5G5B5, R5G6B5, G8B8, others as 32-bit) are
    deswizzled and converted by a compute shader from a staging buffer texture straight into the texture levels
    (same results as the CPU loop: menu frames identical, race identical up to animation). Race: 0.6 MB of textures
    decoded per frame cost 2.3 ms on the CPU, 0.08 ms now; large new textures no longer stall the render thread.
  - Per-unit texture memo in draw(): unchanged unit registers within a frame and a surface epoch reuse the last
    result (90% of ~19k unit binds per frame at the race start) and skip the wrap / filter maps; units are selected
    (`glActiveTexture`) only before GL calls on them; the unit loop is timed once instead of per unit.
  - Driving race (average of 14 samples): textures + uniforms 5.9-6.3 -> 5.0 ms, vertex setup 6.7-7.5 -> 6.5 ms per
    frame; at the 30 fps cap. Still on the CPU: content hashes for change detection (~1.5-2 ms), index-region
    hash maps / min-max scans (~2 ms), fragment-constant uploads per draw (~1-2 ms).
  - Scripted menu input: the Single Player menu ignores presses for ~10 s after it opens (Quick Race opens at the
    36 s press with `2:cross,12:start,16:cross,...` every 4 s); the Driver garage was entered three times with
    `...,36:cross,40:down,42:cross,...`.
- **Roadmap step 3 (2026-10-29): resolution scaling and anti-aliasing.**
  - `F1CE_SCALE=N` (1-4): local render targets, depth buffers, multisampled storage and cube maps are allocated at
    N x their guest size; viewport, scissor, resolves, surface copies (NV0039) and the presentation use the physical
    sizes; the vertex programs map window coordinates through the guest size, so geometry is unchanged.
    Main-memory targets (read back by the CPU: the per-car lighting table) stay native. Fragment programs read WPOS
    divided by the target's scale (`u_res_scale`, emitted only when N > 1, so the shader cache of scale 1 stays
    valid); occlusion counts are divided by N^2 (LOD and flare intensity come from them).
  - Presentation: with N > 1 or FXAA, a full-window pass samples a mip chain of the display buffer trilinearly at the
    level matching the window (supersampling; the linear blit skipped pixels when shrinking 4K to 720p) and
    optionally applies FXAA (`F1CE_FXAA=1`). `F1CE_MSAA` sets the samples of the antialiased targets (8x tested).
  - Results: 2560x1440 and 3840x2160 render correctly (shadows, crowd, haze, mirrors, HUD); thin structures (ladder
    rungs, poles, crowd figures) are solid and sharp after downsampling. The full grid runs at ~28-29 fps at 4K and
    the GPU (RX 6600) waits only 2.5 ms per frame at the flip: still CPU-bound; driving at 4K holds 30 fps.
  - PPU throttle while lists stream: `refs_behind()` returned 0 whenever PUT stood inside a streamed frame list (race
    frames), so a renderer stuck for a moment mid-list (a long upload, 4K) let the game rewrite lists the front end
    was still running: the FIFO faults seen in races ("unmapped IO offset"). The reference of the last ring slot PUT
    passed through is now remembered (`note_slot`, also from the list watcher) and used while streaming.
    Reproduced with `F1CE_RSX_DRAW_STALL=3100:300:3` (600 ms per frame mid-list): without the throttle 1 of 2 runs
    faulted, with it 2 of 2 completed (the PPU waited ~1.45 s per stall).
- **Fixes after the user's test at scale 2 (2026-10-29):**
  - Full screen (`F1CE_FULLSCREEN=1`, F11 or Alt+Enter) and the window keeps the image's 16:9 aspect ratio with black
    bars (`F1CE_STRETCH=1` stretches as before); `F1CE_WINDOW=<w>x<h>` sets the initial size.
  - Vertical line in the Quick Race menu at higher scales: the portrait (512x512 DXT1) is drawn 1:1 onto 512 pixels;
    at scale 2 the edge samples lie a quarter texel outside and the linear filter blended in the opposite edge of
    the repeating texture. Draws without depth test (interface, full-screen passes) keep coordinates within [0, 1]
    half a texel from the edges when scaled (`u_ui_clamp`); repeating coordinates outside [0, 1] are unchanged.
  - Shadows under the cars were black: their shadow decals decode alpha only (gamma 8) and subtract from the scene,
    and the sRGB-output rule counted any gamma bit, so their colour was encoded (0.2 -> 0.48). Only decoded colour
    channels count now (`F1CE_ALPHA_GAMMA_ENCODE=1`: the old rule). Grid screen behind the car: 3 -> 29 (RPCS3 37);
    the sides of the car are still darker (22 vs 38).
  - One shader source for all scales: the programs read the scale from uniforms, so changing `F1CE_SCALE` no longer
    compiles every program again (the Driver garage stayed white for seconds at a new scale). The first run after
    this change compiles everything once (~14 s of white screens in the first scenes). Four compile threads were
    tried: the AMD driver serialises the links (each took ~6x longer, ~15 fps while compiling), so one thread stays.
  - Open: an intermittent freeze at the race start / early race (2 of ~10 runs on 2026-10-29): the main thread waits
    in `cellSpursEventFlagWait` while SPURS workloads 0, 1, 2 and 4 stop running (6-8 go on); no warning, no FIFO or
    renderer stall. Not reproduced in six further runs.
- **Shader preparation at start-up and the race-start freeze (2026-10-29):**
  - Program database `shader_cache/programs.bin`: the guest microcode of every program pair the game uses (vertex
    program slots, fragment program bytes, shader control, cube mask) is recorded when it first appears. At start-up,
    before the renderer runs its first command, every entry is translated again with the current translator and
    loaded from the binary cache or compiled, with a progress bar in the window; get_program then finds them ready.
    No compile or binary load happens during play for anything seen before, also after a translator change, a new
    scale or a driver update (then the phase compiles: 284 programs in 79.5 s). Warm start: 281 programs in 0.7 s.
    Programs never seen still compile once in play and join the database. (The scripted test input counts from
    process start, so a long phase makes it miss the menus.)
  - Freeze at the race start / early race (main thread in `cellSpursEventFlagWait`, SPURS job managers no longer
    scheduled, their job lists not empty, ready counts 0): a lost wake-up. SPU PUTLLC compared the line data only;
    the job manager read ready count 1 and an empty queue, the game queued jobs and stored ready count 1 again (an
    unchanged value), and the PM's PUTLLC of ready 0 succeeded. On the Cell any store to the line loses the
    reservation. Each 128-byte line now has a write generation (hashed table, `core.cpp`), bumped by every write
    under the reservation lock (PPU stwcx./stdcx., SPU PUTLLC/PUTLLUC, the HLE SPURS writers: ready counts, event
    flags, queues); PUTLLC fails when it changed since GETLLAR. PUTLLC failure rate unchanged (~3%). The freeze was
    rare (3 of ~15 runs); not seen in the runs since, but not reproducible on demand.

- **FIFO fault late in a Quick Race (2026-10-30):**
  - User log: at ~30 fps with no stall the front end ran `0090502a` at 006901ec, right after a complete command: vertex
    program microcode from an older frame decoded as a CALL into the empty end of the IO area, NOPs, then a jump to an
    unmapped offset. `F1CE_FIFO_VERIFY=1` (new) re-reads each command header 4-12 ms after it ran: 655 million headers,
    none changed, so the front end does not overtake the writers.
  - Cause in the list copies. PUT stopping inside a list at an address ending in 0x060 was taken for a ring slot
    (20+ times per 90 s): such a copy had no slot header, so no reference, so it never aged, and could stay queued to
    cover later frames. A late wake-up of the watcher had the same effect: the copy ended short of the closing jump
    and took a later slot's reference. `F1CE_SNAP_LAG=40` (new, late wake-up every ~2 s) reproduced a bad jump at scale 2.
  - Fix: a copy now needs the real closing jump to PUT's slot (searched up to 2 MB from the old PUT), a poll gap below
    30 ms and a reference. The fault itself was not reproduced: its link to these copies is inferred, as the user's
    log has only the first 20 copy messages. Guard added: a jump or call into empty memory waits up to 500 ms for the
    word or target to be written, and drops the copies if a copy disagrees with memory.
  - Scripted input times now count from the end of the start-up shader phase (`host::input_clock_reset`). A held
    scripted button (`50:cross:900`) also blocks manual presses of it: the game sees no new press.
- **Freeze on the race grid (2026-10-30, fixed):**
  - User log (Monaco, at once on the grid): main in `cellSpursEventFlagWait` at 0x16ED9C, second of the three flags of
    FUN_0016ebac, which starts three TerrainCollider tasks (ELF at 0x10082500, host threads). One task sat at pc 0x12DC4
    with its instruction count unchanged. A first theory (result queues full) was wrong: the peak was 17 items of 50.
  - A task retries `QueueTryPopBegin` 100000 times, then prints "QueueTryPopBegin STUCK" (error BUSY: another task
    preempted between PopBegin and PopEnd, more likely in heavy scenes). The print routine (0x12D60) uses send_event and
    reads a second mailbox word, the completion from the PPU-side handler. Only the first word was ever written.
  - Fix: SPURS tasks get the completion word at once (`event_needs_completion`) and the text goes to the log
    ("SPU task<n> print:"). `F1CE_STUCK_TEST=1` lowers the retry limit to 2: it froze every time before the fix, and
    runs normally after it. A PPU flag wait over 3 s logs "freeze:" lines and saves `freeze_task<tid>.bin`.
  - Confirmed by the user: Monaco loads and a full lap completed without a freeze.
- **Vsync and mirrors (2026-10-31):**
  - Vsync: `SDL_GL_SetSwapInterval` was fixed at 0, so frames tore. `F1CE_VSYNC` now selects 1 (default), 0 or -1
    (adaptive). Race start at scale 2 with it on: 28.5-29.3 fps.
  - Mirrors too dark: the rear view's unlit program and its 0.1 constant are the ones the environment cube map uses,
    so they were never the fault. The glass applies exposure 0.18 and Reinhard and outputs linear colour, which needs
    the sRGB encode (sky 0.15 -> 0.42), but the encode rule only saw textures with gamma bits and the glass reads the
    rear view raw. A raw read of a smaller target with the depth test on now selects the encode. Bloom and glare read
    smaller targets too, with the depth test off, and stay unencoded. The mirrors now match RPCS3 in brightness.
- **60 fps (2026-10-31, `F1CE_60FPS=1`, off by default):**
  - The RPCS3 patches are for 02.00 (BCES00005) and this disc is 01.00, so their addresses do not apply. Their
    "60 FPS" word is the game's ticks per frame: here 0x10044288, value 2, read every frame by the main loop (0x2494a4,
    virtual call +0xA0 in a loop). The limiter waits for two vblanks per frame (0x1004d960, handler 0x489a2c).
  - Fixed 1 tick per frame with a 120 Hz vblank gives 60 fps, but half speed where the game holds only 30 fps. So
    `do_flip` sets the ticks of the next frame from the time this one took (1 or 2, remainder carried over) and the
    vblank runs at 119.88 Hz. This does the job of the RPCS3 "Delta time" patch without code changes: that patch
    could not be ported as given (02.00 addresses, the `BCES00005_DeltaTimer` block was not in the copy we had).
  - Scale 2, 60 s of a Quick Race with the throttle held: menus 60 fps, full grid ~30, driving 38-46, game logic
    57.7-60.1 ticks/s throughout (54.8 once during loading). `perf` lines and "60 fps:" log lines show flips and ticks per second.
    Physics step size is unchanged because ticks are unchanged. Frames with 1 and 2 ticks alternate below 60 fps, so
    motion may look less even than a locked 30.
- **Launcher, progress text, first 60 fps work (2026-10-31):**
  - `launcher/f1ce_launcher.cpp`: plain Win32, no dependencies. Variables are cleared when an option is off, because
    `F1CE_60FPS` counts as on whatever its value. The game runs without a console; a non-zero exit shows a message
    pointing at the log. Closing the window under Vulkan used to exit with code 1: the swapchain was rebuilt on the
    destroyed window, now `create_swapchain` stops once a quit was requested.
  - The bar's label is drawn from a 5x7 font as clears (`progress_label`): no shader is ready at that point.
  - RSX thread at scale 2: ~15 clock reads per draw were 6% of the thread, now `prof_usec` (TSC) on both renderers.
    Vulkan also skips unchanged fragment constants and unchanged set 0 bindings. On their own these did not raise the
    fps: the time moved to the SPU wait, so the RSX was not the only limit.
  - SPU: job binaries alternate in one LS buffer, ~310000 line invalidations per second per SPU. A returning binary
    still needed new lookups and a byte-wise hash of whole clusters. Line versions are now a 64-bit hash of the line's
    bytes, taken lazily (`line_version`; a write only clears `line_code`), and one verified block revalidates its whole
    cluster in the dispatch cache. Three identical races: driving 46-48 fps (was 38-42), grid 34-35 (was 33).
    `F1CE_SPU_VERIFY=1` found no mismatch in a race, but it bypasses the dispatch cache, so the cluster shortcut itself
    is checked only by normal play.
- **60 fps work, part 2 and Steam Deck estimate (2026-10-31, Vulkan, scale 1.5, `F1CE_60FPS=1`, Silverstone):**
  - Build: PPU code at -O2, `F1CE_TRACE_PC` off, `__restrict` on the PPU and SPU contexts (guest loads and stores are char
    accesses that otherwise force registers back to memory). `__restrict` also allows hoisting guest loads out of
    loops, so `tools/recomp.py` puts `PPU_LOOP()` (a compiler barrier) on backward branch targets: spin loops must read again.
    No measurable fps change on its own; no hang in any run.
  - SPU: guest atomics use a spin lock, `write_ls` compares per line with AVX2, shorter `shufb` and `cflts` (checked against
    the old code: 8 million `shufb` cases, 160 million `cflts` values, no mismatch), the per-DMA diagnostic needs
    `F1CE_SPU_DMA_DIAG`. Dispatch cache 4096 entries: no change in slow dispatches, so these come from code invalidations
    (75-90 million dispatches/s, ~20% slow), not collisions.
  - SPU work is real: 3 workers give ~17 fps on the grid, 5 give ~31. 6 workers + 2 faces give 34; 7 and 8 hang at boot.
  - Draws per target on the grid (~3400): main 1639, cube map 879 (six 256x256 faces), two 1024x2048 shadow maps 712.
    `F1CE_CUBE_FACES=2` takes the renderer from ~20 to ~14 ms per frame, draws from ~2970 to ~2680; the image looks the same.
  - Results: grid 28-33 fps -> 32-35 with 2 faces, driving 41-54 in these runs (varies a lot with the track position). A steady 60
    is **not** reached: the SPU workers are at 94% and main spends ~40% in the job wait 0x551228.
  - Steam Deck estimate: this machine limited to 8 logical CPUs, scale 1, 2 faces: grid 26-30, driving 35-38. The Deck's cores
    are slower (Zen 2, 3.5 GHz against Zen 3, 4.4 GHz) and it has never been run there, so expect less. 60 fps there needs a
    lower SPU workload (fewer cars or LOD, shadow and cube culling in the game's jobs), not just a faster runtime.
- **60 fps work, part 3: power use (2026-10-31, Vulkan, scale 1.5, `F1CE_60FPS=1`, Silverstone, Ryzen 5 5600G + RX 6600):**
  - New log line `cpu: <n> cores at <f> fps`, every 5 s: cores used by the whole process.
  - Menus at 60 fps: 6.1 -> ~1.7 cores. Idle SPU workers polled with `sleep_usec(100)`, which spins, so five idle workers took
    five cores. `idle_wait` (kernel.h) sleeps for real after 3 ms without work, in the workers and in the RSX FIFO loop.
  - Short `sleep_usec` waits pause between yields. The job manager's PPU waits 0x551228 and 0x5513BC (they only poll SPU counters)
    are `PPU_SPIN` loops (`SPIN_LOOPS` in `tools/recomp.py`): no fps change, they matter when threads outnumber cores.
  - SPU dispatch: one epoch per 4 KB of LS (`region_writes`, with `region_links` for blocks and clusters across regions) instead of
    one global counter, so swapped job binaries and the handler's stack code leave other code valid: slow dispatches 15.5 -> ~2
    million/s. `F1CE_SPU_VERIFY=1` runs every dispatch through this check: no mismatch in a race. The per-DMA debug ring is
    off unless a watch is set and `write_ls` copies lines inline.
  - Results: grid 30-33 -> 31-36, driving 43-58 in one 80 s run with no errors (not comparable sample by sample). 60 fps is still
    **not** steady: race use is still ~7.2 cores, workers 80-91% busy.
  - Tested and rejected: scale 1 + no MSAA changes nothing on the grid and the RSX idles 5-9 ms per frame there, so the grid is
    not GPU- or renderer-limited. The hot clusters are real float maths, not polling. 6 workers give +2-4% for +0.9 cores (default
    stays 5; the job manager's waits read exactly 6 counters, a likely reason why 7+ hang, not verified).
  - Where the SPU instructions go (`SPU_AOT_HEAT`): six loops are 42.7%. 0d800 triangle cull 15.3%, 05778 audio voice (resample,
    two filters, 4 outputs, one sample per iteration) 7.3%, 0d628 vertex transform 6.8%, 08910 u16 ramp 5.7%, 0e3f0 age table 4.1%,
    04ae4 word fill 3.5%. Those last three are bulk helpers now (`spu_idiom_*`, spu_ops.h): they do all iterations but the last,
    the translated loop does the last one and so sets the final registers. `F1CE_SPU_BENCH=1` checks them against the interpreter.
    Grid 30-32 -> ~35, workers 91% -> 81%, 7.4 -> 7.0 cores.
  - Culling cannot simply go: the list is compacted in place (r3, 8 bytes per triangle, r13 output bytes) and skipping it
    (`F1CE_SPU_NOCULL`) gives 1.44 million indices per frame instead of 0.17, grid 28-32. The transform table stays either way
    (the caller keeps its pointer at 272(r80)). `foaDynamicCubeMap::Render` is 0x2F0D14 and 0x2F3B34 (virtual / from 0x2F4420), but
    skipping them hangs the race: 0x2F0390 waits in 0x551228 for a job fence of the skipped pass. `F1CE_CUBE_FACES` only saves
    renderer time for that reason. Removing those jobs means rebuilding that fence chain.
  - One unrelated process at 100% of a core cost ~15% on the grid (25-28 instead of 30-33), so low-end machines will lose more
    than core speed suggests. `pc_samples` only records blocks outside clusters: do not read them as an SPU profile.
- **60 fps work, part 4 (2026-11-01, Vulkan, scale 1.5, `F1CE_60FPS=1`, Silverstone):**
  - Audio voice loop 05778 is an idiom now (`spu_idiom_audio_voice`): all four lanes as on the SPU, last iteration as translated. 3000 random
    states against the interpreter: no difference. In a race its block went from 7.3% of SPU instructions to almost nothing. No fps change on its own.
  - Cube map: the game leaves the cube enabled on a texture unit while it renders that cube's faces, also for programs that do not read it. The
    renderer copied the face just written after nearly every draw of that pass: 437 blits per frame, each ending the render pass, not the 6 expected.
    A unit the program does not sample is now skipped (both renderers). Vulkan "uploads" 4.4 -> 2.2 ms per frame; reflections unchanged by eye.
    Replacing the blit with an image copy changed nothing: the count was the problem, not the cost of one blit.
  - `hash_range` uses two AVX2 accumulators (hash ~1.5 -> ~0.8 ms). `BlockHeap::free_size` is a set of (size, offset): freeing no longer scans all
    blocks of one size.
  - Results: grid 35-36 -> 37-38 fps, sections that ran at 42-44 now ~50, no errors in an 80 s run. Peaks had already reached 60 with ~1500 draws. 60 is
    **not** steady: the grid has SPU workers at ~87% and ~7.3 cores. Largest SPU loops now: 0d800 cull ~19%, 0d628 transform ~8.5% (both software-pipelined, so
    a bulk version must rebuild many live registers).
- **Car shaking on the grid with `F1CE_60FPS` (2026-11-01):** reported on Vulkan, but it is game logic. A stopped car never settles: tyre values at 0x101106F0
  change sign every tick and the heading swings ~0.08 degrees, the wheels ~3 mm. The game shows one frame per two ticks, always the same phase, so it never
  appears; adaptive ticks show both (fixed 1 tick per frame: the scenery jumps 1.9 px on every frame). `pace_ticks` now keeps 2 ticks and 33 ms per frame
  while the player's car stands in a 3D scene (`car_standing`, wheel at 0x10110670), and leaves that as soon as it moves ~3 cm per frame or the frame has
  under 800 draws. Scenery steps above 0.5 px: 434 -> 11 of 900 frames over a run that includes the race start. Measured with a strip copied per frame without waiting (full dumps slow the game to 13 fps and hide
  this). Checked at Silverstone only; 0x10110670 is a static address, Monaco not measured.
- **60 fps work, part 5 (2026-11-02, Vulkan, scale 1.5, `F1CE_60FPS=1`, Silverstone):**
  - The geometry job's vertex transform (0d628) and triangle cull (0d800) are native (`spu_fast_vertex_transform`, `spu_fast_triangle_cull`): the whole software-pipelined loop is
    replaced from its head, because nothing after it reads the loop's registers (only r13 and the compacted list). The conditions were read from an SSA trace of the loop. `F1CE_SPU_CULL_CHECK` runs the interpreter
    against them on copies of the SPU: ~24000 of each compared in races, real data and with NaN/infinity input, no difference.
  - Measured on the grid with `F1CE_60FPS_NOHOLD=1`, three runs each: 38.2 -> 41 fps. Instructions in the geometry workloads drop ~44%. The driving part of `race_test.sh DRIVE=1` cannot compare builds: the car crashes at different times.
  - The freed SPU time became polling (more dispatches and atomics) and the renderer ran slower. The CPU has 6 cores and 12 logical processors at ~7.3 cores of load, with no placement set: a worker on the renderer's
    core costs it ~15%. The renderer now has a core of its own (`F1CE_PIN`): grid ~42.5, ~11% in all. Also giving the main thread a core gives the same fps but workers at 97%.
  - Tried and rejected: keeping indirect branches inside clusters (a pc switch at the end of the cluster, plus return points and `ila` targets as cluster members). Dispatches fell ~20% and slow ones 3.3 -> 1.3 M/s, but the grid
    lost ~6% in alternating runs in all three variants (local registers forced, not forced, sync blocks kept apart); the cause was not found. The hottest loop left is a morton texture copy (0be98/0beb8, ~9% of SPU instructions, its back edge is `binz`), and the profile is flat after it.
  - Renderer thread on the grid (`F1CE_HOST_PROFILE_RSX`): vk draw 21%, FIFO front end ~30% (`rsx_thread` 14, `rsx::vk::method` 8, `fifo_ptr` 4.4, `rsx::method` 3.4), hash_range 10, amdvlk 9. Each command re-read PUT and GET, ran the flow checks and two `fifo_ptr` calls.
    After a command, the front end now runs plain commands in a row from the same source (at most 64). It stops at PUT, at any flow command or NOP, at the end of the source, when the reference moves and when a list copy appears; so flow commands still take every check.
    Grid 43.4 -> 44.5 fps in three alternating pairs. A 100 s drive on Vulkan and three OpenGL runs with `F1CE_RSX_DRAW_STALL=3100:300:3 F1CE_NO_RSX_THROTTLE=1` (the stall only exists in the OpenGL backend; fps fell to 0.2-8 there) had no faults or list copy warnings.
  - Renderer, measured by section: uniforms 0.65 ms (not the 7.5 KB constant copies, as suspected), `setup_attrs_gpu` 3.3 ms. Interleaved attributes each had their own region, so a buffer was hashed and copied once per attribute. Attributes of one record now share a region (`va_fetch` reads bytes at any offset) and a direct-mapped table sits before the region map: hashed 29.5 -> 13 MB per frame, vertices 7.8 -> 6.6 ms, vertex heap 41 -> 17 MB. Plain register writes skip the calls down to the backend (`rsx::g_plain`, Vulkan only).
    Grid ~45-46 fps; the renderer now idles ~4 ms there, so the grid waits for the SPUs again. Scripted driving cannot judge this: the 60 fps of that run were the car stopped on the grass.
- **60 fps work, part 6 (2026-11-03, Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - Where main waits (`F1CE_PROFILE`, new `main-wait` line): ~45% in 00551228 (job counters) and ~10% in `cellSpursEventFlagWait`, and in those samples
    all five workers are busy (w0 on one, w1/w2 on the other four). The SPUs are the limit, not their scheduling.
  - `F1CE_SPU_PROFILE` (new): w1/w2 spend about a third of their time in the job manager's policy module, but its hot pcs are DMA sites
    (`wrch ch21`): 16 KB GETs of job data (01f24), list GETs (01eec), and the job-grab loop GETLLAR 022ac -> PUTLLC 02388 (about 25% of the PUTLLCs fail).
    The host profile had ~5% of worker time spinning on the reservation locks.
  - GETLLAR takes no lock now: it waits for the stripe to be free, reads the generation, copies the line and checks that the lock is still free and the
    generation did not move (every locked writer stores, then bumps the generation). PUTLLC fails without the lock when its generation already moved. The
    locks are 1024 hashed stripes instead of 64. Grid 41.2 -> 43.5 and 39.8 -> 43.0 fps in alternating pairs; a 90 s drive had no faults or freezes.
  - Tried and reverted: the generation in the lock stripe's cache line (two cache lines per atomic instead of three) was ~2% slower in three pairs.
  - The PM's poll service (`others_waiting`, asked on every job) reads the workloads without `g_sp.m` (the mutex was ~2.7% of worker time): 43.9 -> 44.8 fps
    over three pairs.
  - Stopped-car hold (`car_standing`): it took 30 frames to engage, so the car shook for ~0.6 s each time the grid appeared; now 8 frames (~0.2 s). Its
    transitions are logged (`60 fps: hold on/off at <s>`, `entering/leaving 3D scene`), so a user log shows whether it dropped out.
  - Tried and reverted: a 16-byte dispatch cache entry (`fast[]` 64 KB instead of 128 KB; the hottest address in `run()` is the `fe.pc` compare, 5%) was ~2%
    slower in three pairs, so that sample is the landing point of the indirect call, not a cache miss.
  - 6 SPU workers against 5 with these changes: 45.7 against 46.2 fps, +0.9 cores. The 6-core CPU already carries ~7.3 cores of load (renderer, main, five
    workers), so a sixth worker only shares a core. Reaching 60 on the grid needs less work in total, not more threads.
  - Scripted driving (`DRIVE=1`) ran 55-59 fps with main waiting for the SPUs ~50% of the time, but the car was on the grass again by then.
- **Blue player car on Vulkan, Quick Race settings (2026-11-03):**
  - The player's car was blue on Vulkan when it started last on the grid (Heidfeld, Hungary). The per-car lighting table is 32 points at integer x drawn into
    a 32x1 main-memory target; a point on a pixel edge covers the pixel below it in OpenGL and the one before it in Vulkan, so the table came out one entry
    to the left and car 21 read an unwritten texel (000000ff). Point lists now get a quarter-pixel viewport shift (`rsx_vk.cpp`, `apply_state`).
    Comparing the `rsx: lighting table` lines of both backends shows such shifts.
  - `F1CE_QR_*` (see the table) and the launcher's Advanced tab set the Quick Race track, driver, distance and Difficulty screen. Found with
    `F1CE_MEM_DUMP` snapshots taken after each menu press: the settings object is at [0x10138748]; +0x70 holds the whole Difficulty screen as bits
    (`hle_quickrace.cpp`). The dumps are copied and written by a thread: a slow write inside pad polling lost the scripted presses.
  - Open: heavy rain (tested at Monaco) renders, but the rain drops on the screen show hard posterised edges over the whole image; the rain post effect
    ("Rain And Muck", rain refraction) was never checked before. (See the next entry.)
- **Rain (2026-11-03, Vulkan, scale 1.5, `F1CE_QR_TRACK=brazil F1CE_QR_WEATHER=heavyrain`):**
  - Heavy rain ran at ~25 fps on the grid against ~41 dry. `F1CE_PROFILE` showed one more SPURS workload (w3, contention 1) in every sample of main
    waiting for the SPUs, and `F1CE_SPU_PROFILE` gave it ~19% of all worker time spread over every instruction of one loop: the job (a linear
    point-in-triangle search, LS 0x4824) had never been translated because no recording had been made in the rain. Two recordings (`dbg/spu_blocks_rain1.txt`,
    Brazil heavy rain driving; `dbg/spu_blocks_rain2.txt`, Monaco light rain) added 384 blocks: w3 19% -> 3% of worker time, the main-wait samples
    look like the dry ones again, and the grid went from 747 to 1146 frames in 30 s (~25 -> ~38 fps; A/B over 18 s from the race start: 26.3 -> 33.7).
  - So that this cannot hide again, the SPURS report warns (`SPU: n M interpreted instructions/s`) when workers or tasks interpret more than 2 M
    instructions per second (normal races: no warning; the rain task forced into the interpreter: ~190 M/s).
  - Screen rain drops: the chain is the SPURS task `SpuRainDroplet.spu.elf` (ELF at 0x100C3580) -> a 512x288 B8 water map in main memory, double
    buffered (io 00cc2380 / 00ce6380), each row rebuilt from rows y-2..y+2 of the other buffer plus 0xe00 bytes of state per row -> fragment program
    8213ee750eb96053, the last pass before the interface, which moves the image by 0.04 x the slope of the map (eight taps, 1/512 and 1/288 apart) and
    reads the display buffer it writes. The map holds trails one texel wide (of 10888 lit texels 243 have a lit horizontal neighbour) that slide down a
    row per frame and fade ~4% per frame; their steep edges move the image by tens of pixels in hard patches. A Python model of the program on dumped
    inputs reproduces the game's image exactly, the task gives the same map translated and interpreted (`F1CE_SPU_NOAOT_TASK`), its instructions
    (cflts/cfltu scales, gb, rotqm*, fi/frest, ...) match the ISA and its DMA order is sound, so no emulation fault was found. Without a console or
    RPCS3 reference in the rain, `F1CE_RAIN_SMOOTH` (default on) reads the map through a 3x3 tent filter (four bilinear taps half a texel apart, this
    program only), which turns the patches into smooth streaks; `=0` gives the unfiltered image. Debug aids added: `F1CE_SPU_PROFILE_DUMP`,
    `F1CE_SPU_NOAOT_TASK`, `F1CE_TASK_DMA_LOG=<elf>` with data, and the watch saves the LS of the first SPU that puts to the watched line
    (`watch_ls.bin`). Note: main-memory textures are at EA = io + 0x31200000 (`_cellGcmInitBody` io base), not 0x32000000.
- **All tracks, renderer front end (2026-11-03, Vulkan, scale 1.5):**
  - SPU coverage on every track: a scripted drive at each of the 18 tracks with `F1CE_QR_TRACK` and `F1CE_SPU_DUMP_BLOCKS` (dry) found 132 blocks
    without a translation (Spain 28, Brazil 25, Malaysia 21, Indianapolis 19, the rest 0-6), none hot enough to raise the interpreted-instructions
    warning; added from `dbg/spu_blocks_tracks.txt`. No faults or freezes on any track.
  - Vertex program uploads in bulk: the constants (0x1F00..0x1F7C) and microcode (0x0B80..0x0BFC) went through `exec_method` -> `rsx::method` ->
    `vk::method` once per word. The front end now hands a whole run of such words to the renderer (`rsx::g_upload_words`, Vulkan only; OpenGL keeps
    the per-word path). Renderer busy time on the grid (frame time minus FIFO idle) 19.3 -> 16.4 ms over three alternating pairs, grid fps
    43.3 -> 44.7 (the grid is SPU-limited); driving ~51 fps with 2856 draws against ~50 with 2564 before.
  - Index lists are converted with AVX2 (`convert_indices`, `index_minmax`; checked against the old loop on 200k random lists). It made no
    measurable difference: the scalar loop was ~0.6 ns per index, and the ~2 ms of `index` time per frame is lookups, hashing and streaming of the
    ~400k indices per frame that change every frame (lists the SPU cull job writes).
  - Logs: `perf vtx` splits rebuilt indices into region growth, changed regions and per-draw lists; `perf tex` gives texture hash time and MB per
    frame by kind (linear / up to 64 KB / larger) while driving: ~0.9 ms over ~2.6 / 5 / 7 MB. Vertex + texture hashing is ~1.6 ms per frame and
    memory-bound; skipping it would need write tracking whose ordering against PPU/SPU writes is not solved, so it stays.
- **Renderer front end and per-draw checks (2026-11-04, Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - New log line `perf front`: whole draws, their attribute setup and parameter/pass part, clears, flips, semaphore waits, and the commands,
    words and slow-path entries of the FIFO front end per frame. On the grid: ~155k commands / ~500k words per frame, draws 10.5 ms, front end
    ~3.8 ms. ~37k of the commands were NOPs, and each ended the run of plain commands, so the next command took the slow path (47k entries
    per frame); NOPs now stay in the run (1.3k entries). GET is written at the end of a run and before SEMAPHORE_ACQUIRE / flip (`sync_get`),
    not after every command (it shares a cache line with PUT, which the PPU writes), and the front end's position for the list watcher is a
    plain store instead of a seq_cst one.
  - Vertex program hash per draw kept until the VP words change (`g_vp_gen`): program 1.5 -> 1.3 ms.
  - Change checks of data that has not changed for 60 frames: vertex regions over 16 KB and textures of 16-64 KB compare a word per 256 bytes
    on 7 frames of 8 (full check on the 8th, staggered); textures over 64 KB get a full check every 32 frames and a word per KB every 4 frames
    (before: a full check every 8 frames, ~60 MB of track textures at the grid). Data streamed into the same memory rewrites nearly every line,
    so the sparse words catch it within 4 frames. Vertex hash 1.0 -> 0.7 ms, texture hash 1.0 -> 0.6 ms per frame.
  - Tried and dropped (no measurable change): a direct-mapped lookup in front of `g_idx_regions`, prefetching command words. The vertex data
    still hashed every frame (~6 MB) is ~1500 regions of ~4 KB rewritten recently, checked at every draw.
  - Renderer busy time on the grid 15.7 -> 14.3 ms over three alternating pairs (every pair better); grid fps unchanged (~46, SPU-limited:
    FIFO idle ~6.5 ms per frame). 70 s drive with no errors and the same warnings as before. At the race start (60 fps) the render thread
    waits up to ~4 ms per frame in SEMAPHORE_ACQUIRE for the game: that is waiting, not renderer work.
- **SPU work on the grid, reflection cube map at 30 Hz (2026-11-04, Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - Where the worker time goes (rdtsc per dispatch, a diagnostic build only): ~30% in the job manager's policy module, ~66% in job code. The jobs
    call PM services ~250k times per second per worker (DMA list builders 0x2D48, waits 0x1580/0x1968, the LS buffer allocator 0x1670, job end
    0x2AB0), and the PM's interrupt handler (0x0A90, registers saved by code it writes on its stack, then the prefetch of the next job) runs once
    per job: ~6000 PM instructions per job, ~2700 jobs per frame in w1/w2. Both parts run at ~2.5 ns per SPU instruction; the PM is busy, not
    polling (its grab-loop spin is <1%), and block rebuilds cost ~1.2%.
  - Tried and dropped (dispatches fell 16-18%, fps unchanged in alternating runs): self-loops through indirect branches in single-block
    translations (the morton copy 0be98/0beb8), and inlining leaf calls (`brsl` to a one-block function returning through its link register)
    in cluster code. A fast dispatch costs ~7 ns; the cost is the instructions themselves, about 43 million SPU instructions per grid frame.
  - The reflection cube map (`foaDynamicCubeMap::Render`, 0x2F0D14, once per frame, all six faces, ~500 draws plus their cull and transform
    jobs) is ~20% of a grid frame: never rendering it (`F1CE_CUBE_SKIP=1`) gives 55 fps against 44. The game renders it every frame, so 30
    updates per second at its 30 fps. With `F1CE_60FPS` a frame that starts less than 25 ms after the last update now keeps the previous
    faces, and the main view's wait for the faces' job fences (0x2F0390, six `0x551228` waits) is skipped with it (the earlier hang came from
    that wait on fences of a pass that never ran). At 60 fps that is exactly 30 updates per second, below ~40 fps every frame. Hooks in
    `tools/recomp.py` HOOKS, `runtime/hle_cubemap.cpp`; `F1CE_CUBE_30HZ=0` turns it off. Grid 45.1 -> 48.4 fps over three alternating pairs
    (every pair better); a 70 s drive ran to the finish with no errors, reflections look the same. The faces are drawn inside the scene
    render 0x43B528, so three faces per frame (even frame times) would need that function taken apart.
  - `tools/recomp.py` now rewrites only the generated files whose text changed (a new hook rebuilds two PPU files, not 48).
  - New diagnostic `F1CE_SPU_DMA_STATS=1`. Next leads: a native job manager PM (the largest piece of SPU time left, but the game's own job
    API, interrupt-driven prefetch and allocator would have to be reproduced exactly), and the shadow map passes (~700 draws).
- **OpenGL start-up crash, SPU register allocation (2026-11-04, evening; Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - The OpenGL "guest memory fault after a cold shader compile" (seen once on 2026-11-01) came back in a run that had to compile 169
    programs (51 s). OpenGL builds its program database on the RSX thread at the first command, and the PPU throttle in `timer_usleep`
    gives up after 15 s: the game then rewrote the first frame's command lists under the waiting RSX. The throttle now waits without a
    limit while that phase runs (`g_rsx_starting`). Cold test (`F1CE_SHADER_SALT=x F1CE_NO_PRELOAD=1`, 933 programs, 185 s): the PPU
    waited 181 s and the race started normally.
  - Translated SPU clusters were meant to keep the SPU registers in host registers (`SpuLocal`), but the store path called
    `SpuLocal::code_hit` out of line, which took the context's address: every register access was a stack load or store and the pc
    was stored at every instruction. `code_hit` is now inlined into a call on the thread (`SPUThread::code_hit`, out of line). Grid
    ~50 -> ~54.3 fps over three alternating pairs (every pair better); a 70 s drive had no errors and held 60 fps on the straights.
  - Tried and reverted: running clusters with channel instructions on local registers too (registers copied around each channel
    instruction, 32 more clusters): ~54.5 -> ~49 fps. Those clusters (mostly the job manager's) are entered often for short runs,
    and the copies in and out cost more than they save.
  - Shadow passes priced with the new `F1CE_SKIP_FN` (measurement only): skipping `RenderStaticShadows` (0x2F141C, one shadow map,
    ~160 draws) gives ~55 fps against ~50, skipping `RenderBlurredShadows` (0x2F1BEC, two maps of ~270 draws; the sun shadows of
    cars and the start gantry) ~56. The static map cannot simply be kept for a frame as the cube map is: its target (0x03060000)
    is drawn over by the blurred pass in the same frame.
- **Launcher options for weaker PCs: reflection updates, shadow resolution (2026-11-04, night; Vulkan, scale 1.5,
  `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - `F1CE_CUBE_HZ=<n>` replaces `F1CE_CUBE_30HZ`: reflection updates per second (default unchanged: 30 with `F1CE_60FPS`). The rule is
    the same as before for any rate: a frame that starts less than 3/4 of the interval after the last update keeps the previous faces.
  - Shadow maps: the three maps of a frame are drawn by `RenderShadowMap` (0x43F620): the static scenery into 0x03060000 (cleared), then
    in `RenderBlurredShadows` the same list of ~270 casters into 0x03860000 and over the static map (no clear between). The blurred pass
    also contains the screen-space passes (the receivers write a shadow mask into the main target, it is copied to 0x02190000, blurred and
    composited). Skipping `RenderBlurredShadows` whole on every other frame made the shadows vanish on those frames; skipping only the
    three `RenderShadowMap` calls keeps them: the receivers read the maps of the previous update and, while driving at ~6 ticks per frame
    (frame dumps), the shadows of the scenery stayed in place, so the matrices the receivers use belong to the maps. Moving cars' shadows
    lag by up to one update. As `F1CE_SHADOW_HZ=<n>` this gave on the grid (pairs alternating) default 52.0 / 51.4, 30 updates/s
    55.4 / 58.7, plus `F1CE_CUBE_HZ=15` and `F1CE_SHADOW_RES=0.5` 58.8 / 58.8, but the user saw the shadows under the cars blink
    at 30 updates/s and below: removed (2026-11-05).
  - `F1CE_SHADOW_RES=<k>`: the shadow maps (the only 1024x2048 targets) at a fixed multiple of their original size instead of the
    internal resolution; their depth buffers take the same scale (`DepthSurface::scale_x/y`, recreated when it changes). Checked at 0.5
    on Vulkan and 2 on OpenGL: the gantry shadow is in place, softer or sharper.
  - Launcher, General tab: "Reflection updates" (replaces "Reflection faces per frame", which saved renderer time only) and "Shadow
    map resolution".
- **Native job manager (2026-11-05, Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone grid):**
  - The job manager's policy module (guest 0x100be000, LS 0xA00..0x3580, shared by every SPURS workload) was ~30% of the
    SPU workers' time at ~2.5 ns per SPU instruction: its clusters contain channel instructions, so their registers stay
    in memory. Its hottest functions are now C++ (`runtime/spu_pm.cpp`), each entered at the head of its first block
    (`SKIPS` in `tools/spu_aot.py`) and leaving the local store, MFC state and the registers its callers read exactly as
    the SPU code does (quadword side values included):
    - 0x19f8, a job's buffer list (~65% of the PM's instructions with its helpers 0x2c00 / 0x2750 / 0x27b8 / 0x2848):
      buffer sets of 1 KB LS blocks in three job slots, 256-bit block maps, element choice by age and reuse of data the
      other slot already holds, write-back of conflicting outputs, the GETs (lists too) of the job's inputs;
    - 0x2d48 (DMA info of a buffer, tag allocation), 0x2ce8 + 0x2c80 (a job releases a buffer), 0x1670 up to 0x18e0 with
      0x2940 (job end: sets handed over, outputs flushed), 0x2288 (taking the next job from the queue; its GETLLAR /
      PUTLLC still go through the MFC, so the reservation rules are unchanged).
  - Interrupts: the PM's only event is the list stall-and-notify it raises itself (0x26e4, event mask 2). Functions run
    natively only where the original could take no interrupt inside them: 0x19f8 with interrupts off, 0x2ce8 (it opens
    a window at 0x2d14) with no event pending; 0x2940's notify is its last command, so the interrupt is taken at 0x18e0
    as before. Other cases go to the SPU code (none seen).
  - `F1CE_PM_CHECK` compares each function with the interpreter on copies of the SPU (dry DMA: puts logged, conditional
    stores into a private line copy). In a race: 0x19f8 87k compared (up to 8200 instructions, 36k with DMA), 0x2d48
    116k, 0x2ce8 47k, 0x1670 33k (27 with write-backs), 0x2288 36k: no differences, except 8 in 0x2288 in data fetched
    by plain GETs from lines other SPUs change between the two runs (wait counters).
  - Grid over three alternating pairs, same exe with `F1CE_PM_NATIVE=0`: 0x19f8 alone 54.5 -> 56.7, the first four
    54.4 -> 57.3 (every pair better); the job queue adds nothing measurable (failed PUTLLCs 192k -> 181k per second).
    A 70 s drive had no errors or warnings.
  - What is left of the PM: its DMA copies inside 0x19f8 (16 KB job data, lists), the queue atomics (~4% of worker
    samples), the interrupt entry 0xa30 (~2%, registers saved by code it writes on its stack) and ~20 small functions.
  - Later the same day, the interrupt entry (0xa2c, up to the event handler 0x21c0) and exit (0xa98, to the iret): the
    handler saves r0..r79 with 76 stores it writes onto the stack and calls, restores them the same way, and returns
    with `iret` or to a continuation in r78. Natively the same code words are written (the local store stays the same)
    and the registers saved and loaded directly. 32k checks each, every register compared: no differences.
  - Bug found on the way: native functions entered from cluster code with local registers got registers the cluster
    does not use from its local copy, where they are not valid (the interrupt entry saved a link register of 0xcc and
    an SPU jumped to the kernel area). `tools/spu_aot.py` now writes the cluster's registers back to the thread before
    the call there, and the native functions always work on the thread. The kernel-call fatal error prints srr0, the
    interrupt state and, with `F1CE_TRACE_WKL`, the last blocks run.
  - Grid, three alternating pairs: original 55.0 / 55.5 / 54.5 fps, native 57.9 in all three. A 70 s drive held ~60 with
    no errors.
  - Shadow passes priced again with `F1CE_SKIP_FN` (measurement only): 57.9 fps with them, 58.4 without the static one
    (0x2f141c), 57.5 without the blurred one (0x2f1bec), 58.6 without any shadow map (0x43f620), while SPU busy drops
    86% -> 67%. `F1CE_PROFILE` main-wait: the main thread waits on job counters (00551228) ~31% of its time and on the
    event flag ~9%, with all five workers busy on other jobs; the grid is limited by the job chain's latency here.
  - Without vsync (`F1CE_VSYNC=0`) the grid reaches only ~61 fps (62 with no shadow maps), with main sleeping 14-25%
    of its time and the renderer idle 0.6-2 ms per frame: the frame is close to the 60 fps pacing, and the 6-core CPU
    carries ~6.6-7.3 cores of load, so the remaining work competes for the same cores.
  - Tried and removed: the job runtime's command-stream loop (0x5a28 / 0x5c88 in the geometry jobs, the hottest job
    code in the SPU profile at ~5%: per command it copies 32 bytes of the stream into the frame six times and calls a
    handler from a table with r3 = context, r4/r5 = the two halves as vector arguments) as native code. Exact (207k
    checks; a first version broke the game because it did not set r4/r5: native replacements before an indirect call
    must set the callee's argument registers too), but six alternating pairs gave no fps change, so it was taken out.
- **Grid shake, root cause search (2026-11-05):**
  - The physics is tick-based: each tick runs 3 substeps of 1/180 s (0x116d38 for all cars, 0x11c150 tyre forces per car). The player
    car's state is in the heap around 0x38272000; on a shaking grid its tyre forces (0x382725d0..dc, ±4097 / ±3967 / ±3894 / ±3830 N),
    a second force set (0x38272640..4c, ±600) and the wheel values copied for rendering to 0x101106f0 (from 0x38272a40, written by the
    constraint builder 0x11ac0c) change sign on every tick: a period-2 limit cycle of the contact/tyre solver on a standing car.
  - It is bistable: the same setup sometimes settles (forces ~0.02 or 0) and sometimes stays in the cycle, depending on how the car
    arrives on the grid, so with/without experiments need several runs each. Single runs suggested a dependence on frame time (33 ms
    frames settled, 16 ms ones did not; the game's clock at 2x settled; the smoothed frame time is the float at 0x100ca4dc, read
    through the accessor 0x72bc0 by ~45 functions, none in the physics), but repeated runs showed unchanged builds settling too.
    Making non-blocking SPURS task-result reads wait did not remove it.
  - Open: a fix in the solver (damping the cycle of a standing car) or a hold that engages only while the cycle is present.
- **Grid shake, root cause found (2026-11-06):** it is the game's own wheel-spin integration, and the original has it too.
  - Car objects: 22 of 0x1390 bytes (0x367aefc0 + n * 0x1390 on the quick-boot path; the player is the last, car 21), passed as r3
    to the per-substep chain 0x117ed0 (wheels), 0x11b0dc (slip solve, calls 0x11ac0c), 0x11d66c, 0x11cf6c, 0x11c150. The render
    block 0x10110650 + id * 0x110 is copied from them by 0x16ebac (+0xa0 = car +0x910, +0xb0 = +0xa70, +0xc0 = +0xff0).
  - car +0x910 = the four wheels' angular velocity ω (rad/s), +0x6b0 = wheel radius 0.33 m, +0x38c = wheel inertia 3.0 (+0x390 =
    1/3), +0x510 = longitudinal tyre force (N). Each substep (dt = 1/180 s) 0x117ed0 does ω += dt * (-F * r) / I (pc 0x1187f0), and
    0x11ac0c computes the slip ratio as slip / max(|contact speed|, 16) (vrefp at 0x11b058), so below 16 the stiffness is constant.
  - With those constants the wheel's own feedback is ω - ω0 -> -(ω - ω0) * ~2.00 per substep (fit over consecutive substeps:
    K = 1080, gain dt * K / I = 2.0002): the stability limit of explicit Euler. ω swings ±0.19 rad/s on every substep and never
    settles; the amplitude is whatever the car arrives with, which is the "bistable" behaviour. The tyre force swings ±617 N with it,
    the suspension and body follow.
  - `F1CE_PHYS_PROBE=5` in normal 30 fps mode shows the same per-substep swing with the same amplitude: the original game has it.
    It is invisible there because a 30 fps frame is 2 ticks * 3 substeps (even: every frame sees the same phase); with
    `F1CE_60FPS` a frame is 3 substeps and the phases alternate on screen.
  - Not an emulation error: rounding `vrefp` results to 12 bits (the Cell gives an estimate, here exact) down or up left all nine
    runs shaking.
  - Debug tools: `PROBES` in `tools/recomp.py` (runtime code called before one instruction), `runtime/hle_physics.cpp`
    (`F1CE_PHYS_PROBE=<s>[,<ea of car +0x910>]` logs the ω update and the slip solve, `F1CE_PHYS_LOG` tyre-function calls),
    `F1CE_TICK_DUMP` takes several ranges and `F1CE_TICK_DUMP_FRAMES`. Small dumps only: a 96 MB dump slowed frames to 2 ticks
    and hid the swing.
  - Fix options (the 30 fps hold stays the default until one is chosen): change the game's low-speed tyre model (e.g. halve the
    wheel feedback below the clamp speed, or 4 substeps of 1/240 s per tick: gain 1.5, the swing dies within a tick, ~33% more
    physics time), or leave the physics as the original and draw the cars at the mean of the last two substeps (cancels the
    swing exactly, 2.8 ms of delay, needs the car transform fields).
- **CPU profile of the grid (2026-11-06, Vulkan, scale 1.5, `F1CE_60FPS=1 F1CE_60FPS_NOHOLD=1`, Silverstone, ~57 fps, ~7.1 cores):**
  - Main: job counters 00551228 28-32%, `cellSpursEventFlagWait` 10-13%, sleeping 11-19%; every main-wait sample has all five
    workers busy, so total SPU work is still the limit. (`gcm_interrupt` shows "100% fn 00489a2c": the profiler credits a thread
    waiting on a host condition variable to its last guest function; it is not spinning.)
  - SPU workers (`F1CE_SPU_PROFILE`): idle 13%, job manager ~24% (natively replaced parts included), job code ~62%. The PM's
    remaining hot pcs are MFC commands: 0x1eec / 0x1f24 issue the job-data GETs (list and 16 KB chunks), 0x22ac / 0x2388 are the
    job-queue GETLLAR / PUTLLC (~40k pairs per second per worker, ~22% of the PUTLLCs fail). Mean GET sizes are small (16-620 B).
  - New log line `SPU slow dispatch time`: 0.19 cores for all workers (~4% of their time), block rebuilds 0.045 (222k/s).
  - Renderer (`F1CE_HOST_PROFILE_RSX`): draw 21.8%, driver ~13%, front end loop 9.7% (~125k commands per frame), hash_range 8.9%,
    hash_fp 2.9%, sleeping/semaphore waits ~16%; idle ~1.5 ms per frame.
  - No single remaining item is above ~4% of a thread; with ~0.26% fps per 1% of SPU work saved, the cheap leads are used up.
- **Grid shake fixed by filtering what is drawn (2026-11-06, option (b) chosen by the user):**
  - Only the player car runs the full substep physics: 0x17b200 is its tick (3 calls of 0x116d38, one per substep; the other
    cars take a simpler path), 0x117ed0 is the first function of each substep of every car.
  - `runtime/hle_physics.cpp`: at the start of each substep (hook on 0x117ed0) the car object (0x1390 bytes) is copied; after
    the tick (hook on 0x17b200, which runs the original and then returns) each 16-byte quad whose words are zero or normal
    floats in the three states gets x = s3 - (s3 - 2 s2 + s1) / 4 (s1, s2: start of the last two substeps, s3: end). That
    removes a period-2 component exactly and leaves steady motion without delay (error a * dt^2 / 4). Before the next tick
    the physics values are put back (each word that still holds the filtered value), so the simulation is the original's;
    a tick that runs no substeps (some do) gets the previous filter again.
  - Fields with the swing (12 substep snapshots): orientation rows (+0xb0..+0xd8), wheel matrices (+0x1c0..+0x218),
    velocities (+0x220..), positions (+0x3d0, +0x3f8), wheel world positions (+0x840), wheel angles (+0x900), ω (+0x910),
    suspension (+0xa70), forces and slip (+0x4a0.., +0x4e0..+0x51c). The body object (+0x140) has none.
  - Check: one tick per frame (`F1CE_TICKS_FIXED`) and 12 frame dumps: without the filter the whole image moves ±1 px on
    alternate frames (mean frame difference 2.95); with it 0.2 and no shift. The probe still shows the physics swing.
    A 40 s drive was clean. On by default with `F1CE_60FPS`, and the 30 fps hold is off then.
  - Note: the tick dump (`F1CE_TICK_DUMP`) is taken on the RSX thread at flip time, so it can see the car during the next
    tick (restored values): use frame dumps to judge what is drawn.
- **CPU time for smaller machines / Steam Deck (2026-11-06):** goal a steady 30 fps on a Deck (4 Zen 2 cores / 8 threads,
  ~0.6x the per-thread speed of this Ryzen 5 5600G).
  - Waiting threads used CPU: in the original 30 fps mode the process took 5.9 cores with the SPU workers 49% busy (main 0.99
    core, renderer 1.00, each worker 0.72). New blocking waits (`LineWait`, core.cpp / ppu_runtime.h): a thread registers the
    128-byte guest line it polls and sleeps on a futex (WaitOnAddress / futex) until a writer of that line wakes it, 1 ms at
    most. Writers that wake: SPU DMA puts and list puts, PUTLLC / PUTLLUC and PPU atomics (`line_written`), HLE updates, the
    RSX's reference / semaphore releases and GET; plain PPU stores do not, except two probes after the game's own stores
    (`PROBES` in tools/recomp.py): the flip handler's label (0x489aa8) and libgcm's PUT (0x48b050).
  - Users: the job manager's job-counter waits (`PPU_SPIN`, 0x551228 / 0x5513bc: 20 us spin, then the counters' lines),
    libgcm's wait for the RSX reference (13000-19000 `usleep(30)` per second from 0x4ef074) and the PPU throttle
    (`rsx_ref_wait`), SPURS event flag and queue waits, idle SPU workers (the ready counts' line), the RSX's semaphore
    acquire (the flip handler's label) and its idle FIFO (PUT).
  - SPURS tasks (~500 per second, ~250 us each): a pool of task threads instead of a new thread per task (one is created
    only when none is idle, so tasks waiting for each other never block). Main and the task threads run above normal
    priority (critical path).
  - 30 fps mode, full CPU: 5.9 -> 4.0 cores (main 0.32, renderer 0.44, workers ~0.52 each). 60 fps mode, grid, three pairs
    against the old behaviour in the same exe: 57.6 -> 59.2 fps and 7.3 -> 6.1 cores; a 60 s drive at 59.4 fps on 4.7 cores.
  - Smaller machines (`F1CE_CPUS`, 60 fps mode uncapped, scale 1): 6 logical CPUs (3 cores) 32.0 -> ~38 fps (task pool
    32 -> 37.5 in three pairs, priorities +3% in four pairs), 8 logical CPUs 49 fps before the pool. 4 workers instead of 5
    are worse (~32 vs ~38); 4 logical CPUs are too few (12-14 fps).
  - The original 30 fps mode on 6 CPUs gave only 28.8 with dips to 26.8 (it holds the next frame back to a vblank); 60 fps
    mode with `F1CE_FPS_CAP=30` holds 30.0 on 6 and on 8 CPUs, 60 ticks/s; a 60 s drive on 6 CPUs held 29.9-30.0.
    `F1CE_CUBE_HZ=15` saves ~10% more (6 CPUs uncapped 33.5 -> 36.7).
  - Steam Deck estimate (never run there): ~32-36 fps uncapped on the Silverstone grid, so 30 with a small margin. Settings:
    Vulkan, 1280x800, "Up to 60 fps" + "Frame rate cap 30", reflections 15 per second. Under Proton the waits map to Linux
    futexes; the native Linux build still has no renderer.
- **Quick boot to the Press Start screen (2026-11-05):**
  - `F1CE_SKIP_INTRO=1` (launcher: "Skip the language menu and the intro") and `F1CE_LANGUAGE=<name>` (launcher: "Language"),
    `runtime/hle_boot.cpp`. Press Start now shows ~7 s after launch, against the language menu, then the logos (~5 s), then a load (~4 s) and the
    whole intro movie before.
  - The front end is Lua in `offline.wad`: Lua 5.0 instructions (op bits 0-5, C 6-14, B 15-23, A 24-31, constants from
    RK 250) in a 5.1-style container (`\x1bLuaQ`, format 1, little endian, float numbers; function: source, line, 4 bytes,
    lineinfo, locvars, upvalues, constants, protos, code); `tools/luadis.py <wad> <offset>` disassembles a chunk. The boot decisions go through properties of the game object, so the
    skip only sets the state the game itself reaches on other paths:
    - `Init/Language.lua` (the boot Init graph, after `NoPads`) skips its menu when `game.initBoot().firstBoot == 0`, applying
      `bootLanguage` with `game.language` (and mph/km/h). That flag (byte 0x10040010) is cleared by the boot code only when the game is
      started with a `{language}` argument, the return from `ONLINE.BIN`; `GameOrProfileMainSelector.lua` then skips Press Start for
      the profile autoload, so only the first `initBoot` call (Language.lua's) sees 0. The logos come with the menu path and go too.
    - `bootLanguage` (0x100C7620) is the system language mapped by 0x48F88; game codes (switch at 0x247878): 0 english,
      1 german, 2 french, 3 italian, 4 spanish, 5 finnish, 6 japanese, 7 american, 10 portuguese.
    - `GameOrProfileMainSelector.lua` plays the intro unless `game.attract()` (byte game + 0x70F4) is true, which `Attract.lua`
      sets after the intro: set at its first read, the selector goes to `DoStartScreen` (`Splash.lua`).
- **Freeze loading a Time Trial (2026-11-07, user log, Monaco):**
  - Main thread in `sys_lwmutex_lock` (audio mutex) owned by Audio_Thread, which held it in fn 0x4D31CC: a usleep(500)
    loop over its three job lists (fn 0x551358: min of queue head and per-SPU halfwords >= the list's count at +0x26).
    Every thread idle, no SPURS report after the load (a report needs a run to end). Read from the live process: list
    w6 counted 12 jobs, its queue line had top = head = 11 and entry 11 empty. The append (fn 0x55165C) stores the entry,
    then the top halfword, with plain stores; both were gone: the job manager's PUTLLC of the same 128-byte line (its head
    update) compared the line and then copied its old image back over the stores that landed in between. On the Cell any
    store loses the reservation.
  - Fix (`spu.cpp` putllc_store): each changed doubleword is swapped in with a compare-and-swap against the reservation
    copy; a store found in one fails the PUTLLC (doublewords already swapped are put back). PUTLLC failure rate unchanged
    (~24%). Not reproduced on demand; likely the same class as the older rare race-start freezes. The missing loading-screen
    background was probably only the frame shown when main stopped (not checked).
  - Watchdog now prints the SPU workers (workload, pc), every workload's ready count / running / idle time and the job
    manager queue heads, and for fn 0x4D31CC the three audio job lists (target, top, head and SPU slots).
- **Pit stop SPU code translated (2026-11-07):** a pit stop ran ~30 s of untranslated SPU code (40-60 M interpreted
  instructions/s, 50-52 fps instead of ~60). User recording `dbg/spu_blocks_pit1.txt` (223 blocks) added with
  `tools/spu_aot.py` (3696 blocks, 602 clusters).
- **Second Time Trial freeze, open (2026-11-07, Monaco, flying lap after a Silverstone race):** main in fn 0x5513B8 waiting
  for job list w2 (queue 0x10C29100) to reach 2330; jobs 2326-2328 (binary 0x100A1280, the in-order job type of w1/w2)
  spin on three SPUs. Each such job waits until the first incomplete job, from the per-SPU done windows at 0x10414F00
  (base + 96-bit mask per SPU), is within 95 of its own index (the PM's job index, r80). Entries 747-2325 are skips
  (kind 3, reserved by fn 0x48A948 -> 0x5515D0 with the count at object +0x20 in fn 0x43A11C), so the window never
  gets there. The original PM code (0x2288) skips kind 3 like the native one. In normal races (Silverstone, Monaco Quick
  Race, scripted Monaco Time Trial) skip runs (~100k/s, up to ~3400) come only at the end of a list, the next job being
  job 0 of the reset list; a job after >= 96 skips mid-list was seen only in this freeze. Not reproduced; cause (a wrong
  reserve count, a missed list reset, or emulation timing) unknown. Dumps (guest 0x10000000/0x30000000, LS and registers
  of the three SPUs) in the session scratchpad freeze2/. `job queue skips` line in the SPURS report; `F1CE_PM_SKIP_LOG=1`
  logs the job after >= 96 skips (over several grabs). Time Trial by script: IN="9:start,13:cross,17:cross,21:down,
  23:cross,27:cross,37:cross" (garage, leave), any track with `F1CE_QR_TRACK`.
- **Second Time Trial freeze: cause found (2026-11-07):** the cube map skip of `F1CE_60FPS` (hle_cubemap.cpp, every other
  frame at 30 updates/s). Both frozen lists were cube-skipped frames (no six face groups): main view, a reserve of ~1580
  skips, then ordered jobs (operation bytes `01 00 02 00 ... 03`) whose 95-job window cannot cross the reserve. With
  `F1CE_CUBE_HZ=0` the user drove 3 laps of Monaco Time Trial without a freeze (it froze ~20 s into the lap twice before).
  Left as it is for now by the user's choice (no per-mode rule such as "never skip in Time Trial": the same can happen in
  untested places); the launcher's 60 fps option now carries a warning. A real fix has to keep the job list as the game
  builds it when the pass is skipped, or not skip it.
  - Other 60 fps problems reported by the user (not investigated): shadows flashing at random, pit stop animations wrong,
    odd audio (sounds as if the audio buffer were tied to the frame rate).
- **Interlagos race freeze: job list race, fixed (2026-11-07):** two test users froze at Interlagos minutes into a race
  (Vulkan, NVIDIA; one in 30 fps mode with no cube map skips, so the cube skip above was not the cause there). Same
  signature as the Time Trial freeze: main in fn 0x5513B8 waiting for the end of a frame list, three in-order jobs of
  binary 0x100A1280 spinning at 0x90e8-0x96e8 because a long run of skip entries sits before them.
  - Reproduced here by toggling pause every 3 s during an Interlagos race (paused frames have almost no game update, so
    main ends one frame and starts building the next within microseconds): froze in 5 of 7 runs of ~6 min, also with
    `F1CE_PM_NATIVE=0`. Script pattern: `F1CE_QR_TRACK=brazil`, throttle held, `START` every 3 s from 75 s.
  - How the frame lists work: all jobs carry pre-assigned, increasing RSX command offsets (descriptor +0x4C) and the
    in-order ones (+0x30 = `01000200`) chain through the lock line 0x10414E80 (+0x30: next in-order index, command
    offset) and the per-SPU done windows at 0x10414F00 (base + 96-bit mask; an in-order job runs only within 95 entries of
    the first unfinished one; skip entries never finish). Main (fn 0x43A11C from 0x43B0A8) reserves ranges of skip
    entries (fn 0x48A948 -> 0x5515D0) and pushes a w0 writer job for each; the w0 jobs write the real jobs into the
    reserves (plain jobs in fixed groups, the last group always flush against the in-order reserve; in-order jobs at the
    lock's next index). At frame end main waits for w0 (fn 0x489C90 -> 0x55127C), waits for the previous list (0x5513B8)
    and resets it (0x55221C), compacts the current list in place (fn 0x48AC34: non-skip entries to the front, the rest
    skips) and kicks it.
  - Causes found with the new trace (`F1CE_JOBLIST_LOG`): (1) the SPURS scheduler's rule 2 (join a workload busy for
    > 1 ms) also admitted SPUs into a finished frame list whose ready count the job manager had cleared; such a PM took
    the next frame's reserve (2820 skip entries) while main was still building it, the writers then wrote the jobs behind
    the queue head (logged as `behind its head`) and the frame lost them; (2) the job manager's per-SPU halfwords, which
    the PPU waits read, briefly omit a job the grab took before it is in a PM slot, and the last PM of a list can still
    take entries between releasing its halfword and leaving: main's wait could return early, and in one capture main
    compacted a list 0.3 ms before a w0 job appended its last in-order jobs (they landed behind thousands of skips).
  - Fix: rule 2 only joins workloads that still signal work (ready count > 0; `F1CE_SPURS_JOIN_IDLE=1` = old rule);
    the native grab lists taken jobs until they ended and left their PM slot (`runtime/spu_pm.cpp`, g_taken), and
    probes at the returns of the PPU job waits (0x551278, 0x551354, 0x55140C) also wait for those, plus, after the
    waits for a whole frame list (0x5513B8) and for w0 (0x55127C), for the queue's job managers to leave the workload
    (bounded to 20 ms; `job waits:` line every 5 s when it waited, `job wait long:` for waits over 2 ms). The taken-jobs
    part needs the native PM (`F1CE_PM_NATIVE=0` keeps only the scheduler rule and the workload exit wait).
  - Result: pause repro 0 freezes in 12 runs (8 at 30 fps, 4 at 60 fps, ~6 min each); the same exe with
    `F1CE_SPURS_JOIN_IDLE=1` froze again; without the w0 exit wait 1 of 4 runs froze (compaction race). Grid A/B
    against the previous build (3 pairs, 60 fps mode): 45.5 vs 45.5 fps, 1% low ~39 both; 150 s Interlagos drive at
    30 fps: 1% low 18.9 before, 19.6 after. Normal racing: extra waits at most ~60 us (about 0.1 ms per second); while
    toggling pause, 1-19 waits over 2 ms per 6 min (median 3.3 ms, at most 20 ms: a job manager still inside the
    workload, likely a host thread that lost its time slice).
  - The Time Trial freeze above has the same shape (in-order jobs, a reserve of skips, more in-order jobs) and was
    probably the same race; the cube map skip only changed the timing (not retested).
  - Lessons: a host scheduler heuristic that lets an SPU into a workload the game did not signal breaks producer/consumer
    protocols that rely on the ready count; capture the freeze cycle, not the first job wait (the watchdog dump now fires
    on the same wait twice in a row); a deadlock that leaves the RSX replaying the last frame still shows "fps".
- **Linux build and AppImage (2026-11-07):**
  - The Linux build now uses the SDL host with the OpenGL and Vulkan renderers (it was headless). Runtime changes: GL
    1.0/1.1 entry points come from the loader outside Windows (`GL_FUNCS_1X` in `rsx_gl.cpp`: nothing links libGL),
    `F1CE_CACHE_DIR` and a portable shader cache folder (`/proc/self/exe` on Linux), thread names
    (`pthread_setname_np`), `F1CE_CPUS` (`sched_setaffinity`), raised priority for the critical-path threads through
    `setpriority` (usually refused without `CAP_SYS_NICE`: then logged once, all threads equal), SDL app metadata (app
    id `f1ce-recomp` for the window class and the audio stream), missing includes. Windows behaviour unchanged.
  - Cross build: SDL needs its backends' headers and the sonames it later loads. `tools/linux_sysroot.py` takes them
    from Debian 10 packages (glibc 2.28, the zig target); `tools/pkgconfig.py` answers SDL's pkg-config queries from the
    sysroot (PulseAudio, D-Bus). The sysroot holds copies instead of symbolic links (no symlink privilege on Windows),
    which made SDL load the development names (`libX11.so`, missing on systems without -dev packages): the toolchain
    file points SDL's `<NAME>_LIB` entries at the versioned files, so it loads `libX11.so.6`, `libpulse.so.0`,
    `libasound.so.2`, `libudev.so.1`. zig cannot emit SDL's precompiled header (`CMAKE_DISABLE_PRECOMPILE_HEADERS`).
    SDL backends: video x11 (dynamic), audio pulseaudio and alsa (dynamic), libudev, D-Bus; no Wayland.
  - Result: `build-linux/f1ce` needs libc, libm, libpthread, libdl and ld-linux only, symbol versions up to GLIBC_2.27.
  - `linux/make_appimage.py`: SquashFS 4.0 writer (zlib, no fragments, 128 KiB blocks) verified with PySquashfsImage
    on a 746-file test tree (directories over 256 entries, multi-block files) and on the AppImage itself (image at the
    runtime's ELF end, contents byte-identical). 15.8 MB.
  - `linux/AppRun`: game folder detection and picker, zenity settings dialog, Steam Deck defaults, XDG folders; tested
    with dash on fake folders and a fake zenity (first run, settings round trip, environment over file, Deck defaults
    in game mode, dialog choices over Deck defaults).
- **Audio workload print hang (2026-11-08):**
  - User report: music stopped in the menus, then the Time Trial loading screen froze. Watchdog: SPURS wkl 4 (audio)
    parked on spu4 at pc 0x160b8 since its 356th run; Audio_Thread in `sys_lwmutex_lock` (10cef478) owned by main, main
    in `sys_cond_wait` (0x86000009) under fn 0x5e217c.
  - The audio job printed through `sys_spu_thread_send_event` (port 15, data1 = LS 0x3fdb0, saved registers): the
    routine (`wrch OutMbox; wrch OutIntrMbox; rdch InMbox; ...; rdch InMbox`, found in guest memory at 0x1006132c and
    copies) waits for a second word from the PPU-side print handler. 0x160b8 is that second read. Tasks already got the
    completion (`event_needs_completion`, TerrainCollider); workload SPUs did not. The same hang is in an old run
    (dbg/opt_d7, 5 s in). Fix: workload SPUs complete the second read too (reset each run); `spurs_send_event` logs the
    printed text for tasks and workloads (`print:` in the send_event line).
- **Line wait slots leaked on thread exit (2026-11-08):**
  - User crash loading a career Grand Prix after a long session (similar ones entering the driver preview):
    `FATAL: line_wait: more than 16 waiting threads` in a new 'AsyncRender' thread. Each host thread that ever did a
    line wait (gcm label/flip waits, job manager waits) took one of 16 slots for good; the game starts a new
    AsyncRender thread for every loading screen, so the 10th one (plus main, SPU workers, renderer, ...) found none.
  - Fix: slots are a bitmask pool; `line_wait_thread_exit()` (PPU thread exit) clears the slot's lines, its waiter
    count and returns it. Smoke test (menus into a quick race, 5 AsyncRender threads) clean.
  - Not related: a tester's crash at the first race load on Vulkan / Intel UHD Graphics
    (`TEST_LOGS/f1ce_recomp_crash_on_racestart.log`, 70 s, 6 AsyncRender threads) is `vkWaitSemaphores` timing out
    after 5 s (VK_TIMEOUT) on a submitted serial: the GPU did not finish the first race frames. Open; OpenGL advised.
- **Audio timing with 60 fps (2026-11-08):**
  - User report: other cars' and external sounds broken at 60 fps, worse after a long session, fine at 30 fps.
  - The game's mixer (Audio_Thread, guest prio 100; fn 0xAA4E8 every ~1.3 ms: `usleep(1333)`) compares the cellAudio read
    index with its write index (+0x106CC) and mixes until it is two blocks ahead: the next block is due 5-11 ms after it
    is mixed. If the read index gets past the write index, the distance taken mod 8 is above 1 and it mixes nothing
    until the ring comes round (~35 ms of silence). Its per-tick update (fn 0xA0908) runs in the tick loop of 0x2494A4,
    so the game side does not follow the frame rate there.
  - Our side was fragile in exactly that window: the output thread at normal priority (below main and the SPU workers,
    ABOVE_NORMAL), `sleep_for` (~2 ms late on Windows), a burst of blocks after a late wake (which runs past the
    game's write index), a fixed period with no relation to the device clock and an SDL queue near empty (gaps on each
    late wake), and Audio_Thread itself at normal priority under main. 60 fps loads the CPU more, so all of these occur
    more often.
  - Changes (`hle_misc.cpp` audio_loop): precise sleep at HIGHEST priority; one block per period, the time of a late
    wake dropped instead of a burst; the period follows the SDL queue (target 30 ms, at most 2% faster or slower); blocks
    copied two periods late (`F1CE_AUDIO_LAG`); SDL limit raised from 100 to 250 ms as a backstop. PPU threads with
    guest priority 100 or better (Audio_Thread, AsyncRender) get main's host priority (`core.cpp` thread_body).
  - Quick races, 2.5 min each on this machine: 60 fps and 30 fps both had no late wakes and no lost mixes, one late mix
    per run played thanks to the lag, queue 25-36 ms, device clock ~0 ppm against the host clock. The fault did not
    show on this machine in that time, so the user still has to test the long 60 fps session. If the sound is still
    wrong while the `audio out:` lines stay clean, the cause is in the game's sound code at 60 fps (per-frame 3D or
    grain engine updates), not in buffering.
  - User test (Quick Race Silverstone, 60 fps): still broken, most audible during the pit stop when cars pass the
    finish line. Its `audio out:` lines were clean (no lost mixes, underruns or drops, queue 26-34 ms, device clock
    +237 ppm), so buffering is ruled out. Also ruled out: the 8-channel port (channels 2-7 stay silent: the game follows
    the stereo output state) and clipping (peaks below 1.0 at 30 and 60 fps alike). Other cars use the grain engine
    (`Audio/grainengine/<team>_external`, code around 0x5B000-0x5F000). Not found yet: which of its updates runs per
    frame. `F1CE_AUDIO_WAV` added to record the same moment at 30 and 60 fps.
  - User: the sound crackles at times, and the pitch is affected too. Sound engine ("AudioEE", 0x5B0000-0x5D0000): the game
    queues commands that Audio_Thread runs (fn 0x5BBB38, jump table 0x5BBBD0, ~100 commands, probe at 0x5BBBB4). Counted
    per second, every 3D command follows the frame rate: 75-78 (listener matrix rows: two axes, position, velocity) once per
    frame, 67/68 (emitter position at +0x14 / velocity at +0x24) and 62 (3D update of a source, fn 0x5C8260) per source per
    frame. Vectors are fixed point, 4096 = 1.
  - fn 0x5C8260 finds the nearest listener, then distance attenuation, pan and the Doppler pitch
    `(0x136000 - ((v_listener - v_source) . dir >> 12)) / 0x136` (speed of sound 310), passed to each voice of the source
    (virtual +0xC). The listener velocity is the player car's velocity from the frame before.
  - Traced positions and velocities (quick races, the player driving and the player left on the grid while the field
    passes) are clean at 60 fps: velocity = movement per tick (x 60 per 1-tick frame, x 30 per 2-tick frame), smooth,
    with no jitter beyond the car's real acceleration. So the Doppler inputs are correct. What does change: each voice
    gets a new volume/pan/pitch 60 times a second instead of 30. If a voice steps or crossfades on each change, that
    doubles the steps during a fast pass-by. `F1CE_AUDIO3D_HZ=30` puts the source updates back on a 30 Hz grid (checked:
    29.9 updates/s per source at 60 fps, race and audio unaffected); to be judged by ear in the pit stop scene.
  - User: `F1CE_AUDIO3D_HZ=30` changed nothing; recordings of the pit stop at 30 and 60 fps
    (`REFERENCE/CAPTURE_PITSTOP_AUDIO_BUG`). Spectrograms: at 30 fps the passing cars are clean harmonic lines with smooth
    Doppler sweeps, at 60 fps the same harmonics are smeared (fast pitch wobble), no dropouts in either.
  - Command 15 (object +0x18 = value) also doubles with the frame rate, but its objects are slow triangle sweeps (0..32678,
    163 per frame), not engine sounds. The engine sound of a car is fn 0x4C6280 (virtual), run by the audio thread once
    per mix block (187.5/s at both rates): it pops one entry of the car's sound ring (car +0x1170, 16 x 32 bytes, read
    index +0x1374, `0x10F444` pushes one per physics substep: +0x14 revs, +0x18 throttle, +0x1C) and feeds the grain
    engine update fn 0x5CF28 (dt constant 1/180 at 0x10110804).
  - Traced grain engine inputs (`engine_snd.csv`): at 60 fps the AI cars' throttle jumped 0 <-> 1 and the rev ratio
    zig-zagged while braking (7.6 throttle and 7.2 rev reversals per second per car, against 2.9 / 0.14 at 30 fps).
    `F1CE_SHAKE_FILTER=0` brought them back to 2.5 / 0.09; bisected with `F1CE_SHAKE_RANGE` to 0x1110-0x1250: the shake
    filter treated the sound ring as car state and wrote filtered mixtures of unrelated ring entries between ticks, which
    the audio thread then played. Fix (`hle_physics.cpp`): the ring (0x1170-0x1370) is not filtered. After: 2.7 / 0.11 at
    60 fps with the filter on. Confirmed by the user by ear (pit stop, 60 fps).
- **"QUIT GAME" button in the menus (2026-11-08):**
  - User request: a QUIT GAME entry at the bottom centre of the menus, in the game's font, chosen with the pad, closing
    the game like Alt+F4. It is drawn by the navigation bar (between BACK and SELECT, same pen as those labels); moving
    past the last item of a menu (or Up from the first) gives it the focus (the game's red chevrons on both sides, the
    menu's cursor hidden), Up/Down leave it, SELECT calls `os.exit`, which the runtime turns into the window-close
    shutdown (CELL_SYSUTIL_REQUEST_EXITGAME, "Shut Down Complete", exit 0). Text menus and config menus (options);
    screens without a menu (tables, notices) show the label but cannot focus it. Only the 2D front end: not over a 3D
    scene (garage, race pause menus), which the script asks with `os.getenv("F1CE_SCENE3D")` (hooked, >= 800 draws). German/French/Italian/Spanish/Finnish/Portuguese labels from `game.language()`.
  - Done as Lua source: the game links the Lua 5.0/5.1-work parser, so `runtime/hle_frontend.cpp` compiles its patch with
    `luaL_loadbuffer` (0x5A3CF8) after each script the loader (0x7521C) runs. The patch wraps methods of the shared menu
    classes (`gClassTextMenu`, `gClassConfigMenu`, made true globals by the engine's `xmove`) and `NavigationButtons.onRender`
    (found through `getfenv` of a NavigationBars.lua function; screen scripts run in their own environments).
  - Lua errors use setjmp/longjmp, which translated code cannot do (longjmp returned to its caller): `luaD_rawrunprotected`
    (0x58DE18) and `luaD_throw` (0x58DDA4) now run on host exceptions, so `pcall` works; an unprotected error is logged
    before the game's panic exit (the engine calls pad handlers unprotected).
  - Pitfalls: the parser allows at most 16 operators in one expression chain; host code must keep Lua stack positions as
    indices (`lua_gettop`/`lua_settop`), since running Lua can reallocate the thread's stack (a saved `L->top` corrupted
    the engine's next push much later and sent the bisection after unrelated causes); MainMenu animates its picture from
    `mPreviousMenuItem`, which is 0 until the first real move, so the focus step sets it as a move does.
- **PGO + LTO, and SPU polling on small machines (2026-11-09):**
  - PGO + ThinLTO release build (see [Build](#build)): grid 55.8 -> 58.2 fps uncapped (3 A/B pairs).
  - CPU proxy (`F1CE_CPUS`, 30 fps mode, scale 1): 12 and 8 CPUs held a 1% low of ~28 on the grid, 6 CPUs only 18-22, and
    heavy rain at Spain on 6 CPUs had 2.4-4.9 s stalls (none on 12 CPUs). During a stall main waited in fn 0x55127c while
    four SPUs sat in one workload polling: in-order jobs at 0x90e8-0x96e8 or job managers rescanning thousands of skip
    entries (7-12 M GETLLAR/s). On the Cell polling is free; here each SPU is a host thread, and the pollers took the CPUs
    from the threads they waited for.
  - Fix (`runtime/spu.cpp`, GETLLAR): after 8 reads in a row of an unchanged line (same write generation) the SPU thread
    yields. 6 CPUs: rain 1% low ~20 -> 24.7 with no stall in 4 runs (old behaviour: stalls or 650 ms frames), dry grid
    18-22 -> ~28; 12 CPUs uncapped grid unchanged (58.8 / 58.8). `F1CE_LLAR_SPIN=1` = old behaviour (A/B).
  - Diagnostic: a workload run longer than 1 s logs `long run:` (pc, reservation line) and saves the worker's LS
    (`long_run_w<k>_<n>.bin`). The `F1CE_SPU_PROFILE` sampler is biased on `F1CE_CPUS` proxies (it runs when workers
    block): take SPU hotspots from full-CPU runs. The heavy-rain sweep of all tracks found only one untranslated block.
  - SPU host profile in heavy rain (12 CPUs; `F1CE_HOST_PROFILE` now reads the f1ce.map next to the exe): dispatcher
    `SPUThread::run` 8.6% of worker samples (its hottest instruction is the dispatch-cache entry load), DMA emulation ~8.5%,
    the rain job's loop (cluster 0x47a8) 3.7%, the job command loop 2.5%, everything else under 1.2%.
  - The rain job (workload 3, one SPU, so its time is latency) searches up to 1024 triangles for the one under a point in
    the x/z plane (04824-048cc); only lane 1 of its quads decides. `spu_fast_tri_search` (`runtime/spu_ops.h`, a SKIP in
    `tools/spu_aot.py`) tests eight triangles per pass with AVX2 gathers and the same operations (sat, fused fnms), stops
    at the first hit and lets the translated code run that last pass, so every register comes out exact (only dead lanes
    1-3 of r10 differ). `F1CE_SPU_CULL_CHECK=1/2`: 12k searches compared, 0 differ (also with NaN/infinity input).
    Workload 3: 3.8% -> 0.7% of all SPU samples; 6-CPU heavy rain, 4 pairs: 1% low 23.8 -> 24.1, p99 47.6 -> 46.9 ms,
    4.41 -> 4.32 cores (better in every pair, small). `F1CE_SPU_NOFASTSEARCH=1` = translated.
