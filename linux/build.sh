#!/bin/sh
# Native Linux build: f1ce with SDL3 (X11 and Wayland, PipeWire/PulseAudio/ALSA), the OpenGL and Vulkan renderers,
# then the AppImage (out/F1CE-x86_64.AppImage).
#
#   linux/build.sh                 build (dependencies must be installed, see --deps)
#   linux/build.sh --deps          print the package install command for this distribution and exit
#   linux/build.sh --no-appimage   build only (build-linux-native/f1ce)
#
# Needs the generated sources (generated/, made on Windows or here by tools/recomp.py from input/EBOOT.elf),
# third_party/SDL3-3.4.18 and third_party/glslang, as in the Windows build. In a container: linux/Dockerfile.
# The AppImage needs the build machine's glibc version or newer; the zig cross build from Windows (README) needs 2.28.
set -eu
cd "$(dirname "$0")/.."

DEB="clang lld cmake ninja-build python3 pkg-config libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev \
libxrandr-dev libxrender-dev libxss-dev libxtst-dev libx11-xcb-dev libxcb1-dev libwayland-dev wayland-protocols \
libxkbcommon-dev libegl-dev libdecor-0-dev libpulse-dev libasound2-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev"
RPM="clang lld cmake ninja-build python3 pkgconf-pkg-config libX11-devel libXext-devel libXcursor-devel libXi-devel \
libXfixes-devel libXrandr-devel libXrender-devel libXScrnSaver-devel libXtst-devel libxcb-devel wayland-devel \
wayland-protocols-devel libxkbcommon-devel mesa-libEGL-devel libdecor-devel pulseaudio-libs-devel alsa-lib-devel \
pipewire-devel systemd-devel dbus-devel"
ARCH="clang lld cmake ninja python pkgconf libx11 libxext libxcursor libxi libxfixes libxrandr libxrender libxss \
libxtst libxcb wayland wayland-protocols libxkbcommon libglvnd libdecor libpulse alsa-lib pipewire systemd-libs dbus"

appimage=1
for a in "$@"; do
    case "$a" in
        --deps)
            if command -v apt-get >/dev/null; then echo "sudo apt-get install $DEB"
            elif command -v dnf >/dev/null; then echo "sudo dnf install $RPM"
            elif command -v pacman >/dev/null; then echo "sudo pacman -S --needed $ARCH"
            else echo "Debian/Ubuntu: $DEB"; echo "Fedora: $RPM"; echo "Arch: $ARCH"; fi
            exit 0 ;;
        --no-appimage) appimage=0 ;;
        *) echo "unknown option $a (see the top of $0)"; exit 1 ;;
    esac
done

for t in cmake ninja python3; do
    command -v $t >/dev/null || { echo "$t not found: install the dependencies (linux/build.sh --deps)"; exit 1; }
done
[ -d third_party/SDL3-3.4.18 ] || { echo "third_party/SDL3-3.4.18 missing (SDL3 3.4.18 sources)"; exit 1; }
if ! ls generated/ppu_*.cpp >/dev/null 2>&1; then
    [ -f input/EBOOT.elf ] || { echo "no generated sources and no input/EBOOT.elf to make them"; exit 1; }
    python3 tools/recomp.py
fi

# clang if present (the generated functions are huge: gcc takes much longer on them), else the system compiler
if command -v clang >/dev/null && command -v clang++ >/dev/null; then
    set -- -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    command -v ld.lld >/dev/null && set -- "$@" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
else
    set --
fi
vulkan=ON
[ -d third_party/glslang ] || { echo "third_party/glslang missing: building without the Vulkan renderer"; vulkan=OFF; }
cmake -S . -B build-linux-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DF1CE_VULKAN=$vulkan "$@"
cmake --build build-linux-native
echo "built build-linux-native/f1ce"

[ "$appimage" = 1 ] && python3 linux/make_appimage.py --exe build-linux-native/f1ce
exit 0
