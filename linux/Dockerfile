# Container for the native Linux build (linux/build.sh): SDL3 with X11 and Wayland, PipeWire/PulseAudio/ALSA.
# From recomp/ (Docker or Podman):
#   docker build -t f1ce-linux linux
#   docker run --rm -v "$PWD":/src -u "$(id -u):$(id -g)" f1ce-linux
# Output: build-linux-native/f1ce and out/F1CE-x86_64.AppImage (needs glibc 2.36+, Debian 12 / Ubuntu 22.10 /
# SteamOS 3 or newer).
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
        clang lld cmake ninja-build python3 pkg-config \
        libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxrender-dev libxss-dev \
        libxtst-dev libx11-xcb-dev libxcb1-dev libwayland-dev wayland-protocols libxkbcommon-dev libegl-dev \
        libdecor-0-dev libpulse-dev libasound2-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# (the AppImage runtime is downloaded once into third_party/appimage-runtime)
CMD ["sh", "linux/build.sh"]
