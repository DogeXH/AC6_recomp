#!/usr/bin/env bash
# Refresh netplay/release/licenses/ from the source tree and check the Windows link line.
#
#   netplay/release/update_licenses.sh [BUILD_DIR]
#
# 1. Copies every license text listed in MAP below from the vendored sources into
#    netplay/release/licenses/<component>.txt (whole file, or a line range for licenses that
#    only exist as a source-file header). Files marked "manual" are hand-written (no license
#    text in the tree) and are only checked for existence.
# 2. With BUILD_DIR (default out/build/win-amd64-cross-mt), lists the static libraries on the
#    ac6recomp.exe link line and warns about any library this script does not know, so a new
#    dependency cannot slip into a release without a notice.
# 3. Prints the version macros of the main components; compare with THIRD_PARTY_NOTICES.txt.
#
# Read-only on the build dir (it copies build.ninja/.ninja_deps to a temp dir first, so it never
# touches a build that is running).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/netplay/release/licenses"
SDK="thirdparty/rexglue-sdk"
TP="$SDK/thirdparty"
mkdir -p "$OUT"

# dest | source (relative to repo root) | first line | last line   ("manual" = hand-written)
MAP="
ReXGlue-SDK.txt|$SDK/LICENSE||
FFmpeg-LGPL-2.1.txt|$TP/FFmpeg/COPYING.LGPLv2.1||
FFmpeg-LICENSE.md.txt|$TP/FFmpeg/LICENSE.md||
FFmpeg-IJG-notice.txt|$TP/FFmpeg/libavcodec/jfdctfst.c|1|37
FFmpeg-faandct-notice.txt|$TP/FFmpeg/libavcodec/faandct.c|1|20
libmspack-LGPL-2.1.txt|$TP/libmspack/libmspack/COPYING.LIB||
SDL3.txt|$TP/sdl3/LICENSE.txt||
SDL3-hidapi-BSD.txt|$TP/sdl3/src/hidapi/LICENSE-bsd.txt||
SDL3-yuv2rgb.txt|$TP/sdl3/src/video/yuv2rgb/LICENSE||
SDL3-fdlibm-notice.txt|$TP/sdl3/src/libm/e_atan2.c|2|11
glslang.txt|$TP/glslang/LICENSE.txt||
SPIRV-Tools-Apache-2.0.txt|$TP/spirv-tools/LICENSE||
SPIRV-Headers.txt|$TP/spirv-headers/LICENSE||
Vulkan-Headers.txt|$TP/vulkan-headers/LICENSE.md||
Vulkan-Headers-MIT.txt|$TP/vulkan-headers/LICENSES/MIT.txt||
volk.txt|$TP/volk/LICENSE.md||
VulkanMemoryAllocator.txt|$TP/vulkan-memory-allocator/LICENSE.txt||
imgui.txt|$TP/imgui/LICENSE.txt||
imgui-ProggyClean-notice.txt|$TP/imgui/imgui_draw.cpp|6163|6167
fmt.txt|$TP/fmt/LICENSE||
spdlog.txt|$TP/spdlog/LICENSE||
xxHash.txt|$TP/xxHash/LICENSE||
snappy.txt|$TP/snappy/COPYING||
tomlplusplus.txt|$TP/tomlplusplus/LICENSE||
CLI11.txt|$TP/cli11/LICENSE||
utfcpp-BSL-1.0.txt|$TP/utfcpp/LICENSE||
simde.txt|$TP/simde/COPYING||
o1heap.txt|$TP/o1heap/LICENSE||
aes_128.txt|$TP/aes_128/LICENSE||
cppDES.txt|$TP/crypto/des/LICENSE||
TinySHA1.txt|$TP/crypto/TinySHA1.hpp|1|24
rijndael-alg-fst.txt|$TP/crypto/rijndael-alg-fst.c|1|25
picosha2.txt|$TP/picosha2/picosha2.h|1|23
renderdoc_app.txt|$TP/renderdoc/renderdoc_app.h|1|23
freedreno-instr-a2xx.txt|$SDK/include/rex/graphics/format/ucode.h|59|82
DXBCChecksum-notice.txt|$TP/dxbc/DXBCChecksum.cpp|1|9
pe_image-notice.txt|$TP/pe/pe_image.h|1|4
Monocypher.txt|netplay/core/third_party/monocypher/LICENCE.md||
KCP.txt|netplay/core/third_party/kcp/LICENSE||
nlohmann-json.txt|manual||
disruptorplus.txt|manual||
sha256-Brumme-zlib.txt|manual||
VIXL.txt|manual||
DirectXShaderCompiler-NCSA.txt|manual||
Microsoft-CRT.txt|manual||
"

fail=0
while IFS='|' read -r dest src first last; do
  [ -z "$dest" ] && continue
  if [ "$src" = "manual" ]; then
    [ -s "$OUT/$dest" ] || { echo "MISSING hand-written $dest"; fail=1; }
    continue
  fi
  if [ ! -f "$ROOT/$src" ]; then echo "MISSING source $src (for $dest)"; fail=1; continue; fi
  if [ -n "$first" ]; then
    { echo "(Extracted from $src, lines $first-$last.)"; echo; sed -n "${first},${last}p" "$ROOT/$src"; } > "$OUT/$dest"
  else
    cp "$ROOT/$src" "$OUT/$dest"
  fi
done <<< "$MAP"
echo "licenses: $(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"

# --- link-line check -------------------------------------------------------------------------
BUILD="${1:-$ROOT/out/build/win-amd64-cross-mt}"
KNOWN="rexcore rexsystem rexkernel rexgraphics rexui rexinput rexaudio rexfilesystem mspack aes128
imgui SPIRV-Tools SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler xxhash
snappy volk spdlog fmt SDL3-static libavcodec libavutil o1heap ac6net ac6recomp"
if [ -f "$BUILD/build.ninja" ]; then
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  mkdir -p "$tmp/CMakeFiles"
  cp "$BUILD/build.ninja" "$tmp/"; cp "$BUILD/CMakeFiles/rules.ninja" "$tmp/CMakeFiles/"
  [ -f "$BUILD/.ninja_deps" ] && cp "$BUILD/.ninja_deps" "$tmp/"
  libs=$(cd "$tmp" && ninja -t inputs ac6recomp.exe 2>/dev/null | grep -E '\.lib$' | sed -E 's|.*/||; s/\.lib$//; s/rd$//' | sort -u)
  echo "static libs on the ac6recomp.exe link line:"; echo "$libs" | tr '\n' ' '; echo
  for l in $libs; do
    case " $(echo $KNOWN) " in *" $l "*) ;; *) echo "WARNING: unknown library $l: add it to THIRD_PARTY_NOTICES.txt and KNOWN"; fail=1;; esac
  done
  # Header-only third-party code compiled in (from the compiler's dependency log).
  if [ -f "$tmp/.ninja_deps" ]; then
    echo "third-party dirs whose headers are compiled in:"
    (cd "$tmp" && ninja -t deps 2>/dev/null) | sed -n 's|^ *||p' | grep -o -E "(rexglue-sdk/thirdparty|netplay/core/third_party)/[^/]+" | sort -u | sed "s|.*/||" | grep -v -x CMakeFiles | tr '\n' ' '; echo
  fi
else
  echo "(no build dir at $BUILD: link-line check skipped)"
fi

# --- versions --------------------------------------------------------------------------------
v() { grep -h -m1 -E "$2" "$ROOT/$1" 2>/dev/null | tr -s ' \t' ' ' || true; }
echo "versions:"
echo "  FFmpeg $(cat "$ROOT/$TP/FFmpeg/RELEASE") $(v "$TP/FFmpeg/config_windows_x86_64.h" 'FFMPEG_LICENSE')"
v "$TP/FFmpeg/config_windows_x86_64.h" 'define CONFIG_GPL ' ; v "$TP/FFmpeg/config_windows_x86_64.h" 'define CONFIG_NONFREE '
v "$TP/FFmpeg/config_windows_x86_64.h" 'define CONFIG_VERSION3 '
echo "  SDL $(grep -h -E 'define SDL_(MAJOR|MINOR|MICRO)_VERSION' "$ROOT/$TP/sdl3/include/SDL3/SDL_version.h" | awk '{print $3}' | paste -sd. -)"
v "$TP/imgui/imgui.h" 'define IMGUI_VERSION '; v "$TP/fmt/include/fmt/base.h" 'define FMT_VERSION'
v "$TP/spdlog/include/spdlog/version.h" 'SPDLOG_VER_MINOR'; v "$TP/vulkan-headers/include/vulkan/vulkan_core.h" 'define VK_HEADER_VERSION '
v "$TP/volk/volk.h" 'VOLK_HEADER_VERSION'; v "$TP/xxHash/xxhash.h" 'XXH_VERSION_RELEASE '
v "$TP/cli11/include/CLI/Version.hpp" 'define CLI11_VERSION '; v "$TP/snappy/CMakeLists.txt" 'project\(Snappy'
v "netplay/core/third_party/nlohmann/json.hpp" 'version [0-9]'
exit $fail
