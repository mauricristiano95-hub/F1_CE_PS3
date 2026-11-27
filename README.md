# F1 Championship Edition (PS3) – static recompilation

Join the Discord server: https://discord.gg/Ctr4k6HQ9

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
| `runtime/self_decrypt.cpp` | decrypts the disc's `USRDIR/EBOOT.BIN` (retail SELF) in memory and checks it is the 01.00 executable; also used by the launchers |
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
| `launcher/` | the launchers: Win32 `f1ce_launcher.cpp` + `wheel_tab.cpp` (old), Dear ImGui `ui.cpp`, `options.cpp`, `ini.cpp`, `wheel_*.cpp`, `input_page.cpp` (new); shared `version.h`, `f1ce_launcher.rc`/`.ico`, `wheels/` profiles |
| `third_party/` | SDL3, Dear ImGui and Exo 2 font sources (not in git; see [Launchers](#launchers) and Build) |

## Build

Requirements: Python 3, CMake, Ninja, llvm-mingw (Windows), zig (Linux cross build), SDL3 3.4.18
sources in `third_party/SDL3-3.4.18`, the decrypted `EBOOT.elf` copied to `input/EBOOT.elf`.

```sh
python tools/recomp.py                     # regenerate generated/*.cpp
python tools/recomp.py online              # ONLINE.BIN (input/ONLINE.elf) -> generated_online/*.cpp, builds f1ce_online
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

### Launchers

Both launchers are targets of the same CMake project and are built by a plain `cmake --build build-win` along with
the game. Neither takes PGO/LTO flags, so they are always built from `build-win`, not `build-pgo`. Both write and
read `f1ce_launcher.ini` next to their executable, start `f1ce.exe` (`f1ce` on Linux) from their own folder and run
it there (log, `ps3_data/` saves and `tvcam_tune.ini` stay next to the executables), and copy `launcher/wheels/`
next to themselves after every build. Version and icon come from `launcher/version.h` and `launcher/f1ce_launcher.rc`
(bump `F1CE_VERSION*` at every release).

**Old launcher** (`f1ce_launcher`, Windows only): plain Win32 with tabs, plus SDL for the Steering Wheel tab.
Sources `launcher/f1ce_launcher.cpp`, `launcher/wheel_tab.cpp`, `launcher/f1ce_launcher.rc`; no extra downloads.

```sh
cmake --build build-win -j 6 --target f1ce_launcher     # -> build-win/f1ce_launcher.exe + build-win/wheels/
```

**New launcher** (`f1ce_launcher_new`, Windows and Linux): Dear ImGui on SDL3's renderer, gamepad-first, with the
fonts and logo compiled in (`launcher/embed.cmake` turns them into headers in `<build>/launcher_gen/`). It needs two
more downloads in `third_party/` (not in git):

- `third_party/imgui-1.93.0/`: Dear ImGui 1.93.0 (the release's source tree: `imgui*.cpp`, `imgui*.h`, `imstb_*.h`,
  `backends/imgui_impl_sdl3.*` and `backends/imgui_impl_sdlrenderer3.*`)
- `third_party/exo2/`: the Exo 2 static fonts `Exo2-BlackItalic.ttf`, `Exo2-ExtraBoldItalic.ttf`,
  `Exo2-SemiBoldItalic.ttf`, `Exo2-Medium.ttf` (Google Fonts, SIL OFL; keep `OFL.txt`)

```sh
cmake --build build-win -j 6 --target f1ce_launcher_new     # -> build-win/f1ce_launcher_new.exe + build-win/wheels/
cmake --build build-linux -j 6 --target f1ce_launcher_new   # Linux cross build (the AppImage's usr/bin/f1ce_launcher)
```

The new launcher hides its DEVELOPER section (and does not pass its options) unless `F1CE_DEV_MODE=1` is set in the
environment or under `[game]` in the ini. It is not yet tested starting the game, with a wheel live view or a gamepad,
or on Linux; once it is, the two targets swap names.

For a release zip, strip the executables (`llvm-strip build-win/f1ce_launcher.exe`, or `-s` at link time), ship the
template `f1ce_launcher.ini` (game path placeholder, no local paths) and the `wheels/` folder next to the launcher.

`F1CE_HOST` selects the platform backend (`SDL` default, `NULL` = headless).
`-DF1CE_TRACE_PC=ON` (off by default since 2026-10-31) records the guest PC of memory accesses for crash reports and
enables `F1CE_WATCH_LINE` on PPU stores; without it crash reports give the function and LR only.

## Run

```sh
f1ce <path-to>/PS3_GAME [--trace]
```

Or use `build-win/f1ce_launcher.exe`: it sets the display and speed options below, remembers them in
`build-win/f1ce_launcher.ini` and starts the game in `build-win/`, without a console (the new launcher,
`f1ce_launcher_new.exe`, does the same with sections instead of tabs; see [Launchers](#launchers)). Tabs: General (Video Settings, Game Settings), Quick Race Settings (`F1CE_QR_*` below), Steering Wheel (see below) and Developer Tools (the debugging variables below, grouped, each with its description as a tooltip; value fields; "Other variables" takes any `NAME=value` list).

A 1280x720 window opens. Log in `f1ce_recomp.log`, saves in `ps3_data/` (both in the current folder).
The console shows warnings and errors only; the log file has everything (`F1CE_LOG_CONSOLE=1` prints every line,
which slows the game noticeably in Git Bash / the Windows console).

Keyboard: arrows = d-pad, **Z** = ✕, **X** = ◯, **A** = □, **S** = △, **Q/W** = L1/R1, **1/2** = L2/R2,
**Enter** = Start, **Backspace** = Select, **I/J/K/L** = right stick (turns the external cameras). Gamepads are supported through SDL.

### Multiplayer

The front end's Multiplayer entry starts the game's second executable, `USRDIR/ONLINE.BIN` (`sys_game_process_exitspawn`
with the argument `{0}`); here that is `f1ce_online`, built next to `f1ce` and started by it with the same game folder
(and back to `f1ce` when you leave Multiplayer). ONLINE.BIN is decrypted at start like EBOOT.BIN and checked against
its own hash. Its online framework is SCE-RT "Unity" (Medius lobby, DME networking, SVML menu pages in online.wad).
Play Online needs the Medius servers and PSN, which are gone; **Play LAN** works and is made to work over the internet
the way Yuzu/Eden rooms do (`runtime/hle_net.cpp`):

- Each game is a member of a virtual LAN 10.13.37.0/24 relayed by a room: the host's game runs the room (TCP port
  51015, the only port to forward) and joins it as 10.13.37.1; the others join by address. Broadcasts go to every
  member, other packets to their address. The game's whole LAN session (discovery, lobby, race) is one UDP socket on
  port 3658, broadcast discovery included.
- `F1CE_NET_MODE=host|join`, `F1CE_NET_ADDR=<host>[:port]` (join), `F1CE_NET_PORT` (default 51015),
  `F1CE_NET_NAME`, `F1CE_NET_PASSWORD`, `F1CE_NET_LOG=1` (every socket call). The new launcher's Multiplayer section
  sets them. `f1ce_online --room` runs a room without a game. Without a room the LAN has only this game.
- In the game: Multiplayer > Play LAN > user name (the game's keyboard) > Create Game (host) / Join Game (the others).
- cellNetCtl reports a wired connection with the member's address; sceNp is never signed in (Play Online stops there).

Not done: the room relays the game's UDP over TCP (a lost packet delays the ones after it); EBOOT's runtime hooks
(widescreen field of view, camera, physics smoothing, 60 fps, mods, performance probes) are not ported to
ONLINE.BIN's addresses, so f1ce_online runs without them.

### Steering wheels

Any wheel SDL sees (Logitech, Thrustmaster, Fanatec, MOZA, PXN, Simucube, Cammus... by USB vendor or SDL's wheel
type) is given to the game as a **Logitech G25**: the game has its own driver for Logitech wheels (`cellUsbd` extra
LDDs for c283 c285 c293 c294 c295 c298 c299 ca03, report decoder 0x55a660) and its own force feedback, so
`runtime/hle_usbd.cpp` emulates the USB device (descriptors, control/interrupt pipes, a G25 input report every 4 ms)
and `runtime/wheel_sdl.cpp` reads the real wheel and plays the game's forces as SDL haptic effects. With a wheel
attached the game makes it the player's controller (the pad's ✕ no longer starts a race).

- Settings: the `[wheel]` section of `f1ce_launcher.ini` (`F1CE_WHEEL_INI=<file>` names another), written by the
  launcher's Steering Wheel tab (modelled on the Thrustmaster control panel): device, steering axis/invert/centre, the
  wheel's rotation as set in its own driver, the game's steering lock (default: the game's 900°, which it asks for
  with the Driving Force Pro command `f8 02`), pedals calibrated by pressing them, button assignment by pressing them,
  H-shifter gears, force-feedback gains (overall, road/tyre forces, centring spring, damper, wheel weight, minimum
  force), auto-centre, soft lock, and test effects. Keys and defaults: `runtime/wheel_config.h`; without saved
  settings the vendor's usual layout applies.
- Profiles: `wheels/<name>.ini` next to the launcher (`[profile]` name, `wheels` = the vid:pid list it suits, notes;
  `[wheel]` the settings without device and enabled), loaded and saved by the tab's Profile row. Built-in ones in
  `launcher/wheels/` (copied next to the launcher on every build; ship the folder in release zips), axes, paddles,
  H-shifter buttons, rotation and minimum force after Assetto Corsa's controller presets: Logitech G25, G27,
  G29/G923 PS, G920/G923 Xbox, Thrustmaster T300/TX/T248, TX + Ferrari SF1000 (from this PC's Assetto Corsa setup),
  T500 RS, T150/TMX, Fanatec CSL DD/ClubSport, MOZA R series, PXN. The first time the tab sees a wheel without saved
  settings it loads the profile listing its vid:pid. Thrustmaster wheels: throttle Rz (axis 5), brake Y (axis 1),
  right paddle button 1.
- Separate pedal sets (Fanatec/MOZA/Simucube/Heusinkveld/T-LCM pedals on their own USB cable): Pedals -> Separate
  pedal set On in the Steering Wheel section, then the Pedal set row (Automatic or a device) and each pedal's Axis row
  (press it fully to calibrate on the pedal set's axes). Keys `pedals_separate`, `pedals_device` (GUID; not stored in
  profiles, and a profile or the wheel's defaults leave a separate set's calibration alone). Steering, buttons and
  force feedback stay on the wheel. Automatic choice (`wheelcfg::pick`, shared by game and launcher): the wheel is
  SDL's wheel type before a wheel vendor's other devices (Fanatec/MOZA pedals carry the vendor's USB id); the pedal
  set is the first other non-gamepad joystick, a wheel vendor's first. The game rescans every second while the set
  is missing (pedals read as released) and logs `wheel: pedal set ...`.
- A Fanatec base can show up as two game controllers (DirectInput), often with the same GUID, and only one is the
  wheel; which comes first varies. With two or more wheel candidates, `runtime/wheel_joys.h` opens each once (kept
  per SDL id) for its force feedback and axis count, and `wheelcfg::extras` marks the ones with no axes, or without
  force feedback while another candidate has it: `pick` takes real devices first (also among same-GUID twins, and a
  saved extra's GUID falls back to the same vendor's real device), never a force-feedback or axis-less device as the
  pedal set. The launchers list an extra as "not a wheel" (left out when it shares the wheel's GUID); the game logs
  `wheel: ... is not the wheel itself`.
- Twins: a tester's Fanatec showed two "FANATEC Wheel" devices alike in force feedback and axes, one with no input.
  `wheelcfg::Twins` (wheel_joys.h) opens the devices with the wheel's name and USB ids beside it and, 0.3 s after
  opening, takes the first one whose axes (more than 1000), buttons or hat change while the wheel's do not (force
  feedback moves with it). Game log: `wheel: N more device(s) named ...` and `wheel: input from the other ... device
  (<path>) ...: using it`. The ImGui launcher lists one entry per GUID and numbers devices of the same name.
- Buttons, measured on the controller's action flags: G25 byte 1 bits 0/1 (the paddles) = gear up/down, Start =
  pause, R2 = look back, L2 = change camera, Select, ✕ accept / ◯ back in menus, the hat = d-pad (in-race menus).
- Force feedback: the game sends the Logitech classic protocol, but its send path 0x558160 overwrites byte 0 (slot and
  command) with 3, so each force type is treated as its own slot: `08` variable force (level above 0x80 pushes left;
  sign checked with the steering held by script), `0b` high-resolution centring spring (stiffness with speed),
  `03 00..` all off. The game only computes forces (0x356c94) when the controller's force-feedback flag +0x14 is set
  (0 in 01.00 profiles, like the vibration flag): `hle_rumble.cpp` sets it from the wheel's force-feedback setting.
- Only one process can own a wheel's effects (a second one waits in `SDL_OpenHapticFromJoystick`): the launcher
  releases the wheel before it starts the game.
- The game's "Steering assistance" driving aid (on by default) still applies to wheel input.

| Variable | Effect |
|---|---|
| `F1CE_WHEEL=0` | no wheel: the game sees no USB device |
| `F1CE_WHEEL_LOG=1` | logs the LDD registrations, the reports (twice a second), every force command, the forces sent to the wheel (4/s), the player controller's action table and action flags |
| `F1CE_WHEEL_INPUT=<script>` | scripted wheel input for tests, `F1CE_INPUT`'s clock: `51:cross,59:throttle=1:30,64:steer=0.5:2,70:gear=2:5` (buttons by their `[wheel]` key names); a wheel is attached even when none is plugged in |

## Linux and Steam Deck

Built and packaged on Windows. First run on a Steam Deck by the user (2026-11-13, direct start): runs well, below
30 fps at the race start, 22-23 W at peak. Not run on a desktop distribution yet.

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
else asks for the folder (zenity or kdialog). It needs the disc's `USRDIR/EBOOT.BIN`, which the game decrypts itself, as on Windows.

With a display, AppRun opens the ImGui launcher (`usr/bin/f1ce_launcher`) with `F1CE_DATA_DIR=~/.local/share/f1ce-recomp`:
its ini, saves, log, `MODS/` and `wheels/` live there (the AppImage is read-only) and the game runs there. Its first
start seeds `f1ce_launcher.ini` with the game folder found, the settings.conf options and the Deck defaults below,
and copies the built-in wheel profiles (kept if edited). Without a display, with `--direct` or `F1CE_DIRECT=1`, the
game starts at once with settings.conf.

| | |
|---|---|
| `--direct`, `--settings` | start without the launcher; `--settings` shows the old zenity dialog first (renderer, resolution, full screen, vsync, MSAA, FXAA, 60 fps mode and cap, vibration, intro skip, language); `--direct` is also the desktop entry's action |
| `--game-dir <dir>`, `--reset`, `--where`, `--trace` | set the game folder (also in the launcher's ini) / forget all settings (settings.conf and the launcher's ini) / print the paths below / passed to the game (direct only) |
| `~/.config/f1ce-recomp/settings.conf` | direct start: `F1CE_GAME_DIR` and any `F1CE_*` variable of this README, one `NAME=value` per line; variables set in the environment win (Steam launch options `F1CE_SCALE=2 %command%`; with the launcher only for options it does not set) |
| `~/.local/share/f1ce-recomp/` | working folder: `f1ce_launcher.ini`, `ps3_data/` (saves), `f1ce_recomp.log`, `MODS/`, `wheels/` |
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
| `F1CE_INPUT="12:cross,16.5:down"` | scripted pad presses (seconds since start, 200 ms each) for automated tests; `f8` presses the F8 key |
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
| `F1CE_MOD_PACK=<name>`, `F1CE_MODS_DIR=<dir>` | mod pack (season pack): files of `<dir>/<name>/` (default dir `MODS` in the current folder; the launcher passes the `MODS` folder next to it) replace or add game files at run time, `.wad` entries included (`runtime/mods.cpp`; layout in the repo root's `MODDING_IDEAS.md`). `python tools/ptbr_blf.py <USRDIR> MODS/Traducao_PT_BR` builds a Brazilian Portuguese (PT-BR) localization pack from the retail PT-PT `.blf` tables in `offline.wad` |
| `F1CE_WAD_NAMES=<file>` | appends every archive entry path the game asks for to `<file>`, once each (probe at 0x4A528 in `Wad::open`, `runtime/mods.cpp`); `python -I tools/wad_names_merge.py <USRDIR> <file>...` adds the new ones, and their siblings in every driver / team / track folder, to `tools/wad_names.txt` |
| `F1CE_AI_DRIVE=key` or `<s>`, F8 | the AI drives the player's car, as after a black flag (the game's own debug cheat `toggle_ai_control`; `runtime/hle_aidrive.cpp`, notes in `MODDING_IDEAS.md`). While the variable is set, F8 toggles AI control during a race, either way. `key`: F8 only; a number s: also hands the car over s seconds after the player first drives in each race (0 = at once) and logs `ai drive:` lines with the car's speed every 2 s. Launcher: Developer Tools, `F1CE_AI_DRIVE` (Off / F8 toggles AI driving / AI from the start). Scripted input can press it (`F1CE_INPUT="60:f8"`) |
| `F1CE_LUA_PRINT=1`, `F1CE_LUA_LOG=1`, `F1CE_LUA_PATCH=<file>` | front-end Lua (`runtime/hle_frontend.cpp`): log the scripts' `print` output (a stub in the retail game), log every script load, run a Lua file instead of the built-in patch after each script the loader runs |
| `F1CE_SKIP_INTRO=1`, `F1CE_LANGUAGE=<name>` | starts at the "Press Start" screen: no language menu, no Studio Liverpool / SCE logos, no intro movie (`runtime/hle_boot.cpp`; launcher: General tab). The language is `english` (default), `french`, `german`, `italian`, `spanish`, `portuguese` or `finnish`; without the skip it is only the one the language menu starts on |
| `F1CE_SAVE_UI=0` | no save data dialog: list save/load pick the first directory (or the game's new-data one) by themselves, delete does nothing (the behaviour before 2026-11-12; headless builds always work this way) |
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
| `F1CE_MEM_LOG=<s>`, `F1CE_MEM_HEAPS=1` | `mem:` + `rsx mem:` lines every s seconds (default 30, 0 = off): working set and committed RAM, the resident part of each guest range, GPU memory of the process (DXGI: local / shared), the renderer's caches. `F1CE_MEM_HEAPS=1` adds every heap's bytes in use and its largest allocation sizes (slow: a hitch per line) |
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
| `F1CE_GEOM_HLE=0` | the geometry jobs of workloads w1 / w2 run through the SPURS job manager again (default: taken from their queues by the SPU workers and run without it, `runtime/hle_geom.cpp`, GEOMETRY_JOB.md) |
| `F1CE_GEOM_NJ=0` | no native geometry job: the job's own transform and cull run (default: computed natively for the common mesh pieces, `runtime/geom_native_job.cpp`; `=2` also runs every 16th job twice and compares the outputs) |
| `F1CE_GEOM_SK=0` | skinned mesh pieces (drivers, pit crew) run the job's own command handlers (default: native skinning, vertex decode and output, `runtime/geom_skin_job.cpp`; `F1CE_GEOM_SK_CHECK=1` compares every native handler with the SPU one) |
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
| `F1CE_TVCAM=0/1` | onboard TV camera (default on; launcher: "Onboard TV camera"): the second TV pod camera of the view cycle (camera id 3, the game's "TV pod offset") becomes a broadcast-style T-cam, placed relative to the car's own TV pod camera (id 2) by `F1CE_TVCAM_UP`, `F1CE_TVCAM_BACK`, `F1CE_TVCAM_SIDE` (metres, defaults 0.10 / 0.05 / 0), tilted down `F1CE_TVCAM_PITCH` degrees (20) with a vertical field of view of `F1CE_TVCAM_FOV` degrees (68); matched on the Honda against a 2005 BAR Honda onboard shot (`runtime/hle_camera.cpp`). `F1CE_TVCAM=tune` or `F1CE_TVCAM_TUNE=1` (launcher: "numpad tunes it"): the numpad moves the camera live while driving in that view: 8/2 up/down, 4/6 left/right, 9/3 back/forward, 7/1 tilt up/down, +/- field of view, 0 back to the defaults, 5 prints the values; each change is printed on the console (with the equivalent variables) and saved to `tvcam_tune.ini` in the working directory (`F1CE_TVCAM_FILE`), which is loaded on every start and overrides the variables: delete it to return to them |
| `F1CE_TVPOD_SHAKE=0/1` | camera shake of both TV pod cameras (ids 2 and 3, also the onboard TV camera; default on = original; launcher General tab: "TV pod camera shake"): 0 skips the camera shake 0x36defc (called by the fixed-camera update 0x36ead8, acts on the TV pod and cockpit/nose ids) for the TV pod ids 2, 3 and 0xc, the way the game itself skips it when the controller's +0x88 byte is set (`camshake_hook`, `runtime/hle_camera.cpp`) |
| `F1CE_REPLAY_DIRECTOR=1` | Advanced Replay Mode (default off = the original replay; launcher General tab: "Advanced replay mode"; `runtime/hle_replay.cpp`): in a replay square play / pause, L2 / R2 slower / faster (1/4x to 8x, whole ticks per frame through the main loop's tick count, probes 0x249504 / 0x249654), L1 / R1 the car watched (world +0x21c), with a time bar drawn by `runtime/overlay.cpp`. `F1CE_REPLAY_LOG=1` logs the replay object, the world's camera stack and the cars at the replay start and what changes during it (also without the director). Mouse: the bar shows while the mouse moves, a click or drag on its timeline seeks (the game's keyframe at or before the target is loaded as the end of playback does, 0x17E05C, and the rest run as fast ticks without sound, ~600 ticks/s), the play symbol pauses; the sound fades out while paused or seeking. The camera shakes, the crowd and other per-frame effects follow the speed (the frame clock's real time, probes after 0x56E738 in 0x345D70 / 0x346A94 / 0x346BB4; the crowd step at 0x2FB998 / 0x2FBCB8), so a paused replay stands still. In F1TV Watch a race there is no driver, so no car choice. Test aids: `F1CE_REPLAY_SEEK_TEST=<input s>:<fraction>[k],...` clicks the timeline (k: from the oldest keyframe; the log compares the cars after seeks to the same tick), `F1CE_MOUSE_INPUT=<t>:<x>:<y>:<button>,...` scripted mouse (window fractions, F1CE_INPUT's clock) |
| `F1CE_STEER_RESPONSE=0/1/2` | pad (and keyboard) steering speed (default 1 = responsive; launcher General tab: "Pad steering response"; `runtime/hle_handling.cpp`): 0x112818 slews the front wheels towards input x lock at the rate of the car's steering set (dynamics.bin section 6, copied to car +0x5d0): a pad gets max(0.6, 2.4 - 0.025 v) lock units a second, a wheel 20. 1 = max(2, 6 - 0.05 v) with centring 20 (about 3x), 2 = the wheel's rates, 0 = original. The lock and its loss with speed stay the game's; wheels are not affected |
| `F1CE_WHEEL_RATIO=0/1` | steering wheels: constant steering ratio (normally `[wheel] constant_ratio`, default off = original; launcher Steering Wheel page, Steering group: "Wheel steering ratio"; the variable overrides the ini for tests; `runtime/hle_handling.cpp`): the wheel's steering set loses lock with speed (+0x5dc, 0.011 a m/s); 1 sets that word to 0 while 0x112818 runs, so the same wheel turn gives the same front wheel angle at any speed |
| `F1CE_KERBS=0/1/2` | kerb handling (default 1 = improved low; launcher General tab: "Kerb handling"; `runtime/hle_handling.cpp`): the suspension 0x1177f4 clamps its damper at +-24000 N (dynamics.bin section 9); a kerb edge makes 10-17 kN of damper at one corner, the rebound unloads the wheels and the car flies ~0.15 s with a yaw kick. 1 limits the damper to 4500 N while 0x1177f4 runs (normal tarmac stays under 2.6 kN 99% of the time); 2 = improved high: also no tyre grip in the air (the tyres use a load smoothed over ~0.5 s, +0x4c0: a wheel hanging for two substeps loses 20% of it a substep and catches up fast once it touches again); 0 = original |
| `F1CE_AI_PACE=0/balanced/fast/<float>` | track AI pace balancing (default off = original; launcher General tab: "AI pace"; `runtime/hle_ai.cpp`): `0x1D5F50` loads pre-baked AI corner speeds from each track's `.tbf` (`TrackData + 0xEC`), which were authored much slower on circuits like Magny-Cours, Bahrain, Hungary and Nürburgring than on Melbourne or Suzuka. `balanced` (or `1`) computes each corner's 3D curvature from `actualracingline` at track load, lifts under-baked corners to match the Melbourne/Suzuka Hard-AI benchmark (leaving Melbourne and Suzuka untouched), and scales `FakeTimes/*.fkt` (`0x1C04DC`) in sync so simulated qualifying times match on-track pace; `fast` (or `2`) raises pace on every circuit; a number `0.70..1.25` multiplies all corner speeds directly |
| `F1CE_AI_WET_DROP=0/0.20/0.22/0.25/<float>` | wet-weather AI cornering slowdown (default off = original `-17%`; launcher General tab: "AI wet pace"; `runtime/hle_ai.cpp`, `0xE22B0`): retail applies `1 - 0.17 * wetness` with an 80 m/s (288 km/h) cutoff and a 20 m/s (72 km/h) hairpin floor while skipping AI aquaplaning penalties (`0x1148A0`). `0.20` (`realistic`), `0.22` (`balanced`), or `0.25` (`cautious`) replaces it with a calibrated `1 - drop * wetness` curve, lowers the wet hairpin floor to 14 m/s (50 km/h), and smoothly covers 80–84 m/s fast corners while leaving dry tracks and flat-out straights untouched |
| `F1CE_AI_STRATEGIES=0/1` | realistic AI pit stop strategies (default off = original; launcher General tab: "AI pit strategies"; `runtime/hle_ai.cpp`): `AssignAIPitStrategies` (`0xCB0E4`) skips its randomiser in 100% races and assigns the same stop count to all 11 teams, even though `RaceStrategies/<track>_100.bin` provides 1-, 2- and 3-stop windows. `1` mixes 1-stop, 2-stop and 3-stop strategies across AI teams in 50%/75%/100% races, staggers teammates so they never double-stack, and recalculates stint fuel via `0x11E7B0` |
| `F1CE_TYRE_WEAR=0/1.5/2.0/1/2/<float>` | realistic tyre wear and grip degradation (default off = original; launcher General tab: "Tyre wear"; `runtime/hle_ai.cpp`): `UpdateTyreTempAndWear` (`0x113F90`) uses low dry wear rates (`car + 0x37C/0x380`: `0.070/0.0175` hard, `0.085/0.0175` soft), a 37.5% deadzone (`+0x384`), and only 6.5-7.5% max grip loss (`+0x388`), while skipping wear grip loss for AI (`car + 0x2E0 != 0`). `1.5` (`mild`, ideal for 50% scaled races) scales dry wear 1.5x with 15-18% max grip loss from 22% wear; `2.0` (`medium`) uses 2.0x; `1` (`realistic`, 2.5x) scales dry wear 2.5x (soft faster than hard, wet/inter 1.25x), starts grip drop at 20% wear up to 16-19%, and applies proportional grip drop to AI cars (`car + 0x4D0`); `2` (`high`) uses 3.5x wear and 22-26% max grip loss |
| `F1CE_DISCORD_RPC=1`, `F1CE_DISCORD_APP_ID=<id>` | Discord Rich Presence over local IPC (`runtime/hle_discord.cpp`, no external SDK): shows current circuit, team and lap (or menu state) with a session timer. Set `F1CE_DISCORD_APP_ID` to a numeric Application ID from the Discord Developer Portal (optional Rich Presence art asset key `f1ce`) |
| `F1CE_HANDLING_LOG=1` | logs the player's car every tick: steering input / angle / lock, speed, wheel surfaces, loads, filtered tyre loads, tyre forces, compression, heading |
| `F1CE_CAM_ORBIT=0/1` | the external (chase) cameras turn around the car with the right stick (default on; launcher: "Chase camera look-around"): left/right up to 180°, up/down raises or lowers the view; springs back when released. `F1CE_CAM_ORBIT_TEST=<yaw>,<pitch>` holds a fixed orbit (degrees) for frame tests |
| `F1CE_CAM_LOG=1`, `F1CE_CAMERA=<id>` | logs the player camera state once a second (camera list, record, world and view matrices, car matrix), forces a camera id (0 external, 1 far, 2 TV pod, 3 onboard TV, 4 cockpit, 5 nose) |
| `F1CE_WINDOW=<w>x<h>`, `F1CE_STRETCH=1` | initial window size / fill the window instead of keeping the internal image's aspect ratio (black bars) |
| `F1CE_COMPILE_THREADS=<n>`, `F1CE_NATIVE_GLSL=1` | shader compile threads (default 1) / the scale-1 shader sources of before |
| `F1CE_NO_PRECOMPILE=1` | skips the start-up shader phase (the program database is still recorded) |
| `F1CE_TEX_EVICT_S=<s>` | Textures no draw has used for s seconds are released (default 180, 0 = never); Vulkan also releases render targets neither drawn into nor sampled for as long |
| `F1CE_MEM_TARGETS=1` | Vulkan: with each `rsx mem:` line, a `rsx target:` line per render target (size, frames since last drawn into / sampled) |
| `F1CE_GL_PRELOAD=1` | OpenGL: every program of the database stays linked for the whole session (up to 0.4.1; ~1.2 MB of RAM per program on AMD, +1 GB). By default the start-up phase only makes sure each one has a cached binary, loaded when first drawn |
| `F1CE_NO_LINEGEN=1` | SPU PUTLLC compares the line data only (no write generation: the old behaviour) |
| `F1CE_SCALE=1..4`, `<w>x<h>`, `480p`, `576p` | internal resolution (x 1280x720; 1.5 = 1920x1080, 3 = 3840x2160): fractions allowed, supersampled to the window. `<w>x<h>` (640x360 to 5120x2880), at another aspect ratio: the 3D scene takes that ratio and a wider field of view, its interface stays 16:9 in the middle, menus are 16:9 between black bars (`runtime/hle_aspect.cpp`). `480p`, `576p` (`:4x3` for 4:3): the PS3's own video modes, with the game's layouts for them |
| `F1CE_MSAA=1/2/4/8` | samples of the targets the game antialiases (default 4, as on the RSX; 1 = off, launcher "Off") |
| `F1CE_FXAA=1` | FXAA on the final image |
| `F1CE_GPU=<vvvv:dddd>` | the GPU, by PCI vendor:device ids (hex, e.g. `10de:28e1`; Vulkan logs every GPU as `rsx: GPU ...`): Vulkan picks it, else a discrete GPU. OpenGL cannot pick one: the launcher's GPU choice sets Windows' graphics preference of `f1ce.exe` (`GpuPreference` under `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, the Settings > Display > Graphics entry; Automatic = high performance unless the player set one there), which names only the power-saving or the high-performance GPU; a mismatch is logged |
| `F1CE_VSYNC=1/0/-1` | wait for the display's vblank (default) / off / adaptive: late frames tear instead of waiting |
| `F1CE_60FPS=1` | up to 60 fps with the game speed unchanged (ticks per frame follow the frame time); a stopped car in a race stays at 30 fps (`F1CE_60FPS_NOHOLD=1` turns that off) |
| `F1CE_SHOW_FPS=1` | live FPS and frame-time overlay in the bottom-left corner (`runtime/overlay.cpp`; launcher Video tab: "Show FPS counter"; also toggled live in-game with `F3`) |
| `F1CE_SHOW_LAPTIMES=1` | live AI Fastest Lap vs player best lap & delta overlay in the top-right corner during sessions (`runtime/overlay.cpp`; launcher Video tab: "Show fastest lap"; toggled live in-game with `F4`; also logs every fastest-lap update to `f1ce_recomp.log`) |
| `F1CE_RAIN_SMOOTH=0` | the rain drops on the screen as the game's water map gives them (default: read through a 3x3 tent filter, see the 2026-11-03 rain entry) |
| `F1CE_CUBE_FACES=1..6` | reflection cube map faces rendered per frame, in turn (default 6, as on the RSX; saves renderer time only, not the SPU jobs) |
| `F1CE_CUBE_HZ=<n>` | reflection cube map updates per second at most (default 30 with `F1CE_60FPS`, as the original game at 30 fps, else every frame; 0: every frame; `half`: every frame with three of the six faces, alternating, ~30% less SPU work for the pass; `runtime/hle_cubemap.cpp`); `F1CE_CUBE_SKIP=1` never renders it (measurement only) |
| `F1CE_SHADOW_RES=<k>` | shadow maps at k times their original 1024x2048 (0.5 = lighter on the GPU, 2 = sharper); default: they follow `F1CE_SCALE` like every render target |
| `F1CE_CUBE_RES=<k>` | reflection cube map faces at k times their original 256x256, 0.25-4 (launcher "Reflection resolution"; default: they follow `F1CE_SCALE`) |
| `F1CE_CUBE_DIST=<m>` | reflection distance: the cube faces' far plane at most m metres (original 1000) |
| `F1CE_REFLECTIONS=0` | reflections off: the faces are drawn during a scene's first 2 s with a 0.2 m far plane (clear colour and sky only), then kept: a plain sheen on the cars, no pass cost after that (`runtime/hle_cubemap.cpp`) |
| `F1CE_REFLECTIONS=static` (or `F1CE_CUBE_STATIC=1`) | static reflections: the cube map is rendered during the first 2 s of a scene, then kept until the next one (`runtime/hle_cubemap.cpp`) |
| `F1CE_TEX_RES=1/2` | texture resolution: the 1 or 2 largest mip levels of mipmapped textures are left out (half / quarter size; textures without mips, linear, main-memory and texel-addressed ones keep their size, short side at least 32 texels; `tex_levels_dropped` in `runtime/rsx_common.cpp`) |
| `F1CE_ANISO=2/4/8/16` | anisotropic filtering: at least that much on the textures the game filters trilinearly, with the game's -1.5 LOD bias given back per pixel as far as the filtering replaces it (unset = the game's own, 2x on a few textures; 1 = none anywhere; `F1CE_ANISO_KEEP_BIAS=1` debug: bias kept; `tex_anisotropy` in `runtime/rsx_common.cpp`). It changes the shader text: the first start compiles the program database again |
| `F1CE_CAR_LOD=<k>` | car detail distance: the game's car level-of-detail distances (15, 25, 50 m) times k (`runtime/hle_gfxopts.cpp`); `0` = LOD0 only: the full model always, shadow casters included |
| `F1CE_CROWD=0` | no spectators in any session (Time Trial and multiplayer have none anyway; the crowd's sound stays); `runtime/hle_gfxopts.cpp` |
| `F1CE_MOTION_BLUR=0`, `F1CE_HEAT_HAZE=0` | no motion blur (the game's own Motion Blur.Enable read as off) / no heat haze behind the cars (the EngineHeat particle effect is never spawned); `runtime/hle_gfxopts.cpp` |
| `F1CE_DOF=0`, `F1CE_BLOOM=0`, `F1CE_LIGHT_RAYS=0`, `F1CE_POSTFX=0` | the game's own Post Effects switches cleared every frame at the post-effects render (Depth Of Field / Bloom / Crob Rays .Enable); `F1CE_POSTFX=0` = all three plus motion blur and heat haze; `runtime/hle_gfxopts.cpp` |
| `F1CE_ZPASS_LOG=1` | logs a burst of 200 occlusion reports (offset, count) every 10 s |
| `F1CE_ZPASS_PRECISE=0` / `flip` | occlusion reports: by default the renderers write each report when the GPU is done with it, and `cellGcmSetFlip` waits (`wait_reports`, `runtime/hle_gcm.cpp`) until the reports of two frames ago are written, which the game reads in the next frame (`zpass:` line every 10 s); `0`: never waited for (late values); `flip`: all of a frame's reports written at its flip, the RSX thread waiting for the GPU (the old way, ~30% slower at the race start) |
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
- [x] Several profiles: the PS3 save data list (load / create / overwrite / delete) as an overlay dialog (2026-11-12)
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