#!/bin/sh
# Linux build from Windows (Git Bash): zig cross build of f1ce with SDL3 (X11, PulseAudio/ALSA) and both renderers,
# then out/F1CE-x86_64.AppImage (+ .tar.gz with its executable bit). Runs on glibc 2.28+ (2018 distributions on).
#
#   sh linux/build_from_windows.sh            sysroot (first time), configure, build, package
#   sh linux/build_from_windows.sh --clean    the same from a fresh build-linux/
set -eu
cd "$(dirname "$0")/.."
WINGET="$LOCALAPPDATA/Microsoft/WinGet/Packages"
command -v cmake >/dev/null || PATH="$PATH:$(ls -d "$WINGET"/Kitware.CMake_*/cmake-*/bin 2>/dev/null | tail -n 1)"
command -v ninja >/dev/null || PATH="$PATH:$(ls -d "$WINGET"/Ninja-build.Ninja_* 2>/dev/null | tail -n 1)"

[ "${1:-}" = "--clean" ] && rm -rf build-linux
[ -f third_party/linux-sysroot/usr/include/xcb/xcb.h ] || python tools/linux_sysroot.py
[ -f build-linux/build.ninja ] ||
    cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-zig.cmake -DF1CE_VULKAN=ON
cmake --build build-linux -j "${JOBS:-8}"
python linux/make_appimage.py --exe build-linux/f1ce
