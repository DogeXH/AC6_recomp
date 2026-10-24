# Linux x86-64 (Steam Deck) build environment for AC6 Recomp + netplay.
# Recreates Cole's old distrobox "ac6-build" (archlinux:latest, clang 22, cmake 4, ninja,
# python) with the libraries the vendored rexglue SDK needs, plus Go for the lobby server.
#
#   docker build --platform linux/amd64 -t ac6-build:amd64 \
#       -f netplay/docker/linux-build.Dockerfile netplay/docker
FROM archlinux:latest

# pacman's download sandbox (seccomp/landlock) fails under Docker Desktop's amd64 emulation.
RUN sed -i 's/^\[options\]/[options]\nDisableSandbox\nDisableDownloadTimeout/' /etc/pacman.conf

RUN pacman -Syu --noconfirm --needed \
        base-devel git clang lld llvm compiler-rt cmake ninja python pkgconf \
        go \
        libx11 libxext libxrandr libxcursor libxi libxinerama libxfixes libxrender libxtst \
        libxxf86vm libxss libxkbcommon wayland wayland-protocols libdecor \
        alsa-lib libpulse pipewire jack2 sndio \
        mesa libglvnd vulkan-headers vulkan-icd-loader \
        dbus libusb ibus systemd-libs gtk3 \
        zlib zstd xz bzip2 \
    && pacman -Scc --noconfirm

# The app's linux-base preset names clang-20/clang++-20 (Deck-era toolchain); Arch ships the
# current clang without a version suffix.
RUN ln -s /usr/bin/clang /usr/local/bin/clang-20 && ln -s /usr/bin/clang++ /usr/local/bin/clang++-20

# Builds run as the host user (docker run --user), so keep HOME/GOPATH writable.
ENV HOME=/tmp/home GOPATH=/tmp/home/go GOCACHE=/tmp/home/.cache/go-build GOFLAGS=-mod=vendor
RUN mkdir -p /tmp/home && chmod 1777 /tmp/home
WORKDIR /src
