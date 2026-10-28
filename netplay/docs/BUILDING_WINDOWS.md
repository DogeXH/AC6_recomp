# Building AC6 Recomp with netplay for Windows x64

This builds `ac6recomp.exe` against the **vendored SDK** (`thirdparty/rexglue-sdk`, rexglue
0.8.0) with the netplay glue compiled in, the same source the Linux / Steam Deck build uses
(`BUILDING_LINUX.md`). The compiler is clang targeting the MSVC ABI
(`x86_64-pc-windows-msvc`) with the MSVC CRT and Windows SDK, as upstream builds it on Windows.
MinGW is not used.

There are two routes to the same executable:

1. **Cross-build from macOS or Linux** with clang + lld-link and a Windows SDK/CRT downloaded by
   [xwin](https://github.com/Jake-Shadle/xwin). This is how the release build is made.
2. **Native Windows** with Visual Studio Build Tools + LLVM and upstream's presets.

Both need `generated/` (the recompiled guest code) first; see section 1.

## 1. Generated guest code

`generated/` is Git-ignored and must come from the vendored SDK's 0.8.0 codegen plus the
marketplace null-record hand patch (`netplay/tools/deck_patch_generated.py`), exactly as for the
Linux build (`BUILDING_LINUX.md`, section 2). Codegen output does not depend on the host
or target OS, so the Deck tree's `generated/` can be copied as is:

```sh
rsync -a --exclude '*.pre-*' /path/to/deck-tree/generated/ generated/
```

The cross-build never runs codegen (it would need a host build of the `rexglue` tool). On native
Windows, codegen can run in place: `cmake --build --preset <preset> --target ac6recomp_codegen`
with your own `assets/default.xex`, then the hand patch, then reconfigure.

## 2. Cross-build from macOS or Linux

### Tools

| Tool | Version used | macOS (Homebrew) | Debian/Ubuntu |
|---|---|---|---|
| clang, llvm-rc, llvm-mt, llvm-ar | 21.1.8 (18 or newer works) | `brew install llvm@21` | `apt install clang-21 llvm-21` |
| lld-link | 21.1.8 | `brew install lld@21` | `apt install lld-21` |
| CMake, Ninja | 4.4.0, 1.13 (CMake 3.25+) | `brew install cmake ninja` | `apt install cmake ninja-build` |
| Python 3 | 3.13 (glslang's build-info script) | system | system |
| xwin | 0.10.0 | `cargo install xwin` | `cargo install xwin` |
| Wine (optional, smoke test) | 8.0.1 (CrossOver 23.7.1) | `brew install --cask wine-stable` (or Gcenx's `wine-crossover`) | `apt install wine64` |

### Windows SDK and CRT

xwin downloads the MSVC CRT and Windows SDK from Microsoft's Visual Studio manifest and lays
them out for cross-compilers. Running it means **accepting Microsoft's license** for those
packages (`--accept-license`); read it first (`xwin` prints the link).

```sh
xwin --accept-license --cache-dir ~/xwin-cache splat --output ~/xwin
```

This fetches MSVC CRT 14.44 (VS 2022 17.14) and Windows SDK 10.0.26100 for x86_64: about
450 MB of downloads and 810 MB unpacked. The cache directory can be deleted afterwards. On a
case-insensitive file system (default macOS APFS) xwin skips its case-alias symlinks, which is
fine there; on Linux they let `#include <Windows.h>` and `Kernel32.lib` resolve.

### Toolchain file and presets

- `cmake/toolchains/win-amd64-xwin.cmake`: `CMAKE_SYSTEM_NAME Windows`, clang/clang++ in GNU
  driver mode with `--target=x86_64-pc-windows-msvc` and `-fms-compatibility-version=19.44`,
  the xwin include directories (after clang's own resource headers, as on Windows), lld-link
  through the clang driver (`-fuse-ld=lld`), `llvm-rc`, `llvm-mt`, `llvm-ar`, and the xwin
  library paths. Inputs: `XWIN_DIR` (required), `REX_LLVM_BIN` and `REX_LLD_BIN` (defaults:
  Homebrew `llvm@21` / `lld@21`, then `/usr/lib/llvm-21/bin`).
  The output carries no build-machine paths: `-ffile-prefix-map` maps the source tree to `.`,
  the xwin splat to `xwin`, clang's resource dir to `clang` (and an out-of-tree build dir to
  `build`), and `-Wl,/pdbaltpath:%_PDB%` makes the exe name only `ac6recomp.pdb`. Check with
  `strings -a ac6recomp.exe | grep -c "$HOME"` (0) and `llvm-readobj --coff-debug-directory`.
  After changing the toolchain file, reconfigure with `cmake --fresh`.
- `CMakePresets.json`: `win-amd64-cross-relwithdebinfo` (D3D12 + Vulkan, upstream's
  `-march=x86-64-v3 -w`, line-table-only CodeView debug info in a PDB) and
  `win-amd64-cross-release` (`-O3`, no debug info). Both read `XWIN_DIR` from the environment
  and have no `hostSystemName == Windows` condition.

### Build

```sh
export XWIN_DIR=~/xwin
cmake --preset win-amd64-cross-relwithdebinfo
cmake --build --preset win-amd64-cross-relwithdebinfo -- -k 0 -j 7     # target ac6recomp only
```

Output: `out/build/win-amd64-cross-relwithdebinfo/ac6recomp.exe` (+ `ac6recomp.pdb`).

The presets link the static CRT (below). For a dynamic-CRT (`/MD`) build in the same directory,
reconfigure with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`
(`cmake -S . -B out/build/win-amd64-cross-relwithdebinfo -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`);
switching between the two recompiles everything, the generated code included. Both were built
on 2026-10-23: `/MT` (preset, staged in `out/stage/win-amd64/`, 39.6 MB, imports only Windows
DLLs) and `/MD` (`out/stage/win-amd64-md/`, 38.7 MB). Two build directories share the SDK's
library output folder `thirdparty/rexglue-sdk/out/win-amd64/`, so keep one per runtime or
rebuild clean when switching.

What the build does and needs:

- About 910 Ninja steps. On this Mac (M-series, 8 cores, 24 GB, `-j 7`) a clean build took
  about 7 minutes with the machine otherwise quiet, and over 30 minutes at `-j 4` while other
  jobs had it swapping. A change to a global compile flag or definition, or to the CRT,
  recompiles everything; a plain CMake re-run (it happens whenever a globbed source directory
  such as `netplay/core/src` gains a file) only recompiles the few files that include
  `rex/version.h`, which carries the configure time. `ac6recomp` itself is built with ThinLTO
  (the app's `check_ipo_supported` enables it for RelWithDebInfo and Release); lld-link
  does the LTO step in about a minute.
- No host tools run during the build: the shader bytecode is checked in, and only glslang's
  `build_info.py` runs (with the host `python3`). The `rexglue` codegen tool is not built
  because only the `ac6recomp` target is requested.
- The cross presets link the **static CRT** (`/MT`: `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`,
  with `CMAKE_POLICY_DEFAULT_CMP0091=NEW` so third-party projects with an old
  `cmake_minimum_required`, such as glslang and xxHash, follow it instead of falling back to
  `/MD`). The exe then needs no Visual C++ redistributable. Upstream's native presets keep the
  CMake default (`/MD`, dynamic CRT); see section 4 for what that needs.
- `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` is defined for the SDK, its third-party code, ac6net
  and the app's own sources on Windows (top-level `CMakeLists.txt`). MSVC STL 14.40+ gives
  `std::mutex` a constexpr constructor whose layout only `msvcp140.dll` 14.40+ understands;
  with an older runtime (an old redistributable, or Wine 8's builtin `msvcp140`) the first
  lock deadlocks or crashes. The first `/MD` build hung exactly so under Wine 8, in
  `TimerQueue::QueueTimer` during kernel start-up. With `/MT` the STL is linked in and the
  issue cannot occur, but the define keeps `/MD` builds (native route) safe. It is kept off
  `generated/`: it only changes the constructor, and the generated guest code never
  constructs a mutex (it only locks the SDK's), so those large TUs need no rebuild.
- Windows fixes in the vendored SDK (the Deck tree was Linux-only), all guarded so Linux
  builds the same code as before:
  - `src/system/xthread.cpp`, `src/system/xtimer.cpp`: the Deck's CPU-6 pinning of the audio
    threads uses glibc `pthread_setaffinity_np`; it is compiled only on Linux. Windows leaves
    those threads to the scheduler, as upstream does.
  - `src/graphics/vulkan/pipeline_cache.cpp`: with the Microsoft ABI a bit-field never
    straddles its 32-bit unit, so `PipelineDescription` is 60 bytes instead of the 58-byte
    packed layout that the native renderer's decoder and pipeline store read. The export to
    the native renderer re-packs it into the 58-byte layout and hashes those bytes (checked
    field by field with a probe under Wine). On Linux it is the same `memcpy` +
    `GetHash()` as before.
  - `src/native/core/threading_win.cpp`: the Deck tree made `KeSetEvent`, `NtSetEvent`,
    `KePulseEvent` and `KeResetEvent` return the event's previous state to the guest; the
    POSIX backend computes it, and the Win32 backend now gets it from ntdll's
    `NtSetEvent`/`NtResetEvent`/`NtPulseEvent` (it returned 0 every time before).
  - `src/kernel/xam/netplay_glue.cpp`: the netplay data directory is passed to ac6net as
    UTF-8 (`path::string()` throws on Windows for user names outside the ANSI code page), and
    defaults to `%LOCALAPPDATA%\ac6recomp\netplay` (section 4).

Check that netplay is compiled in (`REX_HAVE_AC6NET=1` on `rexkernel` and `rexsystem`, `ac6net`
linked):

```sh
B=out/build/win-amd64-cross-relwithdebinfo
grep -c 'REX_HAVE_AC6NET=1' $B/build.ninja                          # > 0 (rexkernel + rexsystem objects)
llvm-pdbutil dump -publics $B/ac6recomp.pdb | grep -c 'ac6net@@'      # several hundred ac6net symbols
strings $B/ac6recomp.exe | grep -m1 'netplay: no glue workers'
```

List the DLLs the exe imports:

```sh
llvm-objdump -p $B/ac6recomp.exe | grep 'DLL Name'
```

Stage the build for the test rig (`netplay/tools/win_deploy.sh` reads `out/stage/win-amd64/`):

```sh
mkdir -p out/stage/win-amd64
cp -p $B/ac6recomp.exe $B/ac6recomp.pdb out/stage/win-amd64/
```

### Smoke test under Wine (macOS or Linux)

This only shows that the exe starts, reads its config and logs; it is not a gameplay test.
Use a private Wine prefix and a scratch folder with its own `ac6recomp.toml`:

```sh
SM=$PWD/out/smoke/win-amd64; mkdir -p $SM/user
ln -f out/stage/win-amd64/ac6recomp.exe $SM/
cat > $SM/ac6recomp.toml <<EOF2
log_file = "Z:$SM/ac6recomp.log"
user_data_root = "Z:$SM/user"
ac6_performance_mode = false     # keep logging on
ac6_graphics_backend = "vulkan"
EOF2
WINEPREFIX=$HOME/.wine-ac6-smoke WINEDEBUG=-all wine $SM/ac6recomp.exe "Z:/path/to/assets"
```

Results on 2026-10-23 (Wine 8.0.1 / CrossOver 23.7.1 on macOS, only `default.xex` as game
data):

- The `/MD` exe starts, writes `boot.log`, loads `ac6recomp.toml` (`Loaded config:
  ac6recomp.toml`), logs the game source and user data paths and the AC6 config lines,
  selects the Vulkan backend, initialises SDL 3.5.0 and input, and then hangs during kernel
  start-up: the main thread blocks in `std::mutex::lock` inside `TimerQueue::QueueTimer`
  while the timer thread sits in `std::condition_variable::wait_until`. Wine 8's builtin
  `msvcp140` is older than what MSVC STL 14.44 code expects; `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`
  alone does not get past it. Under Proton 10/11, whose prefixes carry Microsoft's
  `msvcp140` 14.42, the same exe gets past this point (lobby sign-in and the title screen on
  Proton 11). A `/MT` exe does not use `msvcp140.dll` at all.
- The `/MT` exe gets past that point under Wine 8: kernel start-up, WASAPI audio, the
  read-only game mount, the Vulkan instance (MoltenVK, "Apple M3"), and then stops cleanly
  at GPU device creation (`Apple M3 doesn't support geometryShader` -> `Failed to initialize
  GPU`), which is expected on macOS. The `/MT` exe has not been run on Proton yet.
- To see where a hung exe is, attach winedbg in the same prefix:

  ```sh
  WINEPREFIX=... wine winedbg --command "info proc"          # find the Windows pid
  printf 'bt all\ndetach\nquit\n' | WINEPREFIX=... wine winedbg 0x<pid>
  ```

  With `ac6recomp.pdb` next to the exe the frames carry function names and source lines.
- Stop the run with `WINEPREFIX=... wineserver -k` (only that prefix).


## 3. Native Windows build

1. Install:
   - Visual Studio 2022 Build Tools (or Community) with the **Desktop development with C++**
     workload (MSVC v143 and a Windows 11 SDK);
   - LLVM 18 or newer (`clang` and `clang++` on `PATH`, for example
     `winget install LLVM.LLVM`);
   - CMake 3.25+, Ninja and Python 3 (`winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.13`).
2. Open the **x64 Native Tools Command Prompt for VS 2022** (it sets `INCLUDE`, `LIB` and
   `PATH` for the CRT and SDK; clang picks them up from there).
3. From the repository root, with `generated/` in place:

   ```bat
   cmake --preset win-amd64-vulkan-relwithdebinfo
   cmake --build out\build\win-amd64-vulkan-relwithdebinfo --target ac6recomp
   ```

   `win-amd64-vulkan-relwithdebinfo` builds D3D12 + Vulkan (the same backends as the cross
   preset); `win-amd64-relwithdebinfo` is D3D12 only. The cross presets also work on Windows
   if `XWIN_DIR` points at an xwin splat, but the native presets are the supported route there.

Output: `out\build\win-amd64-vulkan-relwithdebinfo\ac6recomp.exe`.

## 4. What players need

What goes into a Windows release folder:

| File | Needed | Notes |
|---|---|---|
| `ac6recomp.exe` | yes | 39.6 MB (`/MT`) |
| `ac6recomp.pdb` | no | about 74 MB, line tables only; lets crash reports name functions and lines |
| `ac6recomp.toml` | no | settings, read from the exe's folder (defaults apply without it) |
| `assets/` | yes | the player's own game files (or `game_iso = "..."` in the toml) |

No DLLs ship with the exe. What the 2026-10-23 `/MD` build imports (`llvm-objdump -p`):
`KERNEL32`, `USER32`, `GDI32`, `SHELL32`, `ole32`, `ADVAPI32`, `IMM32`, `dwmapi`, `WINMM`,
`WS2_32`, `IPHLPAPI`, `bcrypt`, `SETUPAPI`, `HID`, `dxgi` (all part of Windows), the UCRT
(`api-ms-win-crt-*`, part of Windows 10/11) and the VC++ runtime (`VCRUNTIME140`,
`VCRUNTIME140_1`, `MSVCP140`, `MSVCP140_ATOMIC_WAIT`). A `/MT` build (the cross presets'
default) links the VC++ runtime and the UCRT statically, so only the Windows DLLs remain.
`D3D12.dll` is loaded at run time; Vulkan comes from the GPU driver's `vulkan-1.dll` (loaded
by volk). `dxcompiler.dll`, `dxilconv.dll` and `D3DCompiler_47.dll` are optional debugging
aids and not needed. SDL3 (audio, joysticks, HID) is linked statically.

Player requirements:

- Windows 10 or 11, 64-bit.
- For a `/MD` build (the one staged on 2026-10-23, and the native presets): the current
  **Microsoft Visual C++ Redistributable 2015-2022, x64**
  (<https://aka.ms/vs/17/release/vc_redist.x64.exe>) for `VCRUNTIME140.dll`,
  `VCRUNTIME140_1.dll`, `MSVCP140.dll` and `MSVCP140_ATOMIC_WAIT.dll`; without it Windows
  reports one of them missing at start-up. Most gaming PCs have it already; Proton prefixes
  ship 14.42. A `/MT` build needs no redistributable.
- A CPU with AVX2 (`-march=x86-64-v3`, as upstream: Intel Haswell / AMD Zen or newer). Older
  CPUs stop at start-up with an illegal-instruction crash.
- A GPU with Direct3D 12 or Vulkan 1.2+ drivers (`ac6_graphics_backend = "d3d12"`,
  `"vulkan"` or `"auto"`).

Online play: see `PLAYING_ONLINE.md` for the `[netplay]` section of `ac6recomp.toml`. On
Windows the online identity (the account's key pair) is stored in
`%LOCALAPPDATA%\ac6recomp\netplay`, not under Documents (often synced by OneDrive and
guarded by Controlled Folder Access); with `user_data_root` set in the toml it is
`<user_data_root>\netplay`, as on Linux. Saves stay in `Documents\ac6recomp` (upstream's
default) unless `user_data_root` is set.

## 5. Release notices

The exe statically links FFmpeg (libavcodec/libavutil) and libmspack under the LGPL 2.1 plus
about 35 permissively licensed components (SDL3, glslang, SPIRV-Tools, volk, imgui, fmt,
spdlog, xxHash, Monocypher, KCP, nlohmann/json and others). Every release zip must carry,
next to `ac6recomp.exe` and `LICENSE.txt`:

- `netplay/release/THIRD_PARTY_NOTICES.txt` (component, version, license, copyright, upstream
  URL, and the LGPL source/relink statement), with the line
  `Source for this build: <URL to be filled in before publishing>` filled in with the public
  URL of this source tree (branch or tag of the build);
- the `netplay/release/licenses/` folder (full license texts).

To regenerate after a dependency change, run `netplay/release/update_licenses.sh
[out/build/win-amd64-cross-mt]`: it re-copies the license texts from the source tree into
`licenses/`, lists the static libraries on the `ac6recomp.exe` link line (warning about any it
does not know) and the third-party directories whose headers are compiled in (from
`.ninja_deps`), and prints the version macros. Update `THIRD_PARTY_NOTICES.txt` by hand from
that output. It only reads the build dir (it copies `build.ninja`/`.ninja_deps` to a temp
dir). Files it marks `manual` have no license text in the tree and were written by hand;
check them against upstream before publishing. FFmpeg must stay built with `CONFIG_GPL 0`
and `CONFIG_NONFREE 0` (`FFmpeg/config_windows_x86_64.h`); the script prints both.
