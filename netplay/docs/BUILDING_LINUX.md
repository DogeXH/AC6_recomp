# Building AC6 Recomp with netplay for Linux x86-64 (and the Steam Deck)

This builds the Linux `ac6recomp` against the **vendored SDK** (`thirdparty/rexglue-sdk`,
rexglue 0.8.0) with the netplay glue compiled in. The Windows build uses the same source
(`BUILDING_WINDOWS.md`). With `[netplay] enabled = false` (the default) the game runs as it does
without online support; to build without the glue at all, configure with `-DAC6_NETPLAY=OFF`.

The commands below run in a Docker container, so they work the same on a Linux x86-64 machine and
on a Mac (Docker Desktop runs the linux/amd64 image under Rosetta; a full build takes about an
hour there). Run them from the repository root.

## 1. Build container

`netplay/docker/linux-build.Dockerfile` is Arch Linux with clang, cmake, ninja, python, Go and
the X11/Wayland/GTK3/audio/Vulkan development packages the vendored SDK needs.

```sh
docker build --platform linux/amd64 -t ac6-build:amd64 \
    -f netplay/docker/linux-build.Dockerfile netplay/docker
```

Notes:
- pacman's download sandbox fails under emulation; the Dockerfile sets `DisableSandbox`.
- The `linux-base` preset uses the image's `clang`/`clang++` (Arch's current clang).
- glibc in the image is 2.44, newer than SteamOS 3.8 (2.41) and most distributions. The release
  package therefore ships its own glibc loader and libraries (section 4).

## 2. Generated guest code

`generated/` is Git-ignored. It is the game's code, recompiled from **your own** `default.xex`,
and must come from the vendored SDK's codegen (0.8.0), followed by one hand patch.

```sh
# your game files (at least default.xex) in assets/, read-only
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -v "$PWD":/src -v /path/to/your/AC6/assets:/src/assets:ro -w /src ac6-build:amd64 \
    bash -lc "cmake --preset linux-amd64-relwithdebinfo && \
              cmake --build out/build/linux-amd64-relwithdebinfo --target ac6recomp_codegen -j 8"
python3 netplay/tools/deck_patch_generated.py .
```

`deck_patch_generated.py` re-adds the marketplace null-record guard (the retail code dereferences
a null colour record when no marketplace data is available). Run it after every codegen; it
stops with an error unless it finds its anchor exactly once. The codegen config is
`ac6recomp_config.toml`; it already lists the 37 extra indirect-call functions netplay needs.
Codegen output does not depend on the host, so the same `generated/` builds Linux and Windows.

## 3. Build the game

```sh
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -v "$PWD":/src -w /src ac6-build:amd64 \
    bash -lc "cmake --preset linux-amd64-relwithdebinfo && \
              cmake --build out/build/linux-amd64-relwithdebinfo --target ac6recomp -j 8"
```

Output: `out/build/linux-amd64-relwithdebinfo/ac6recomp`. The image has LLVM's archive tools,
so the app enables ThinLTO; the final link needs several GB of memory (Docker Desktop's VM had
8 GB).

## 4. Running it

The binary needs glibc 2.44. On a system with an older glibc, run it through the loader and
libraries from the build image, with the loader in the game's folder: the game reads
`ac6recomp.toml` and finds the `.iso` and `assets/` in the folder of the program the system
started, which is then the loader. The Linux release package is laid out that way:

```
ac6recomp                  the game
ld-linux-x86-64.so.2       glibc 2.44 loader (from the build image)
lib/                       libc, libm, libstdc++, libgcc_s and friends from the build image
ac6recomp.sh               runs ./ld-linux-x86-64.so.2 --library-path ./lib ./ac6recomp
```

Everything else (Vulkan, X11/Wayland, audio) comes from the system. The online identity is stored
in `~/.local/share/ac6recomp/netplay` (or `<user_data_root>/netplay`).

## 5. Netplay core tests and the lobby server

The online layer (`netplay/core`, "ac6net") and the lobby server (`netplay/server`) build and
test without the game:

```sh
cmake -S netplay/core -B out/ac6net-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DAC6NET_BUILD_TESTS=ON
ninja -C out/ac6net-linux
(cd netplay/server && go build -o ../../out/ac6lobby-linux/ac6lobby . && go test ./...)
```

Then `netplay/docker/test-linux.sh` (in the build container, ideally with `--network none`) runs
the ac6net unit tests and the `[server]` tests against lobbies it starts on loopback.
