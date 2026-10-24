#!/bin/bash
# Builds and runs driver_runtime_test.cpp: the harness driver against the SDK's real
# InputSystem from an existing librexruntime.dylib (no game, window or GPU; nothing is rebuilt
# in the game's build directory).
#
#   run_driver_test.sh <work-dir>
#
# Environment: AC6NP_BUILD_DIR (default the mac-arm64-release build, for the dylib and
# generated SDK headers), AC6NP_SDK_DIR (default /Users/cole/rexglue-mac-netplay), CXX.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
[ "$#" -eq 1 ] || { echo "usage: run_driver_test.sh <work-dir>" >&2; exit 2; }
WORK="$1"
B="${AC6NP_BUILD_DIR:-/Users/cole/AC6_recomp_netplay/out/build/mac-arm64-release}"
SDK="${AC6NP_SDK_DIR:-/Users/cole/rexglue-mac-netplay}"
CXX="${CXX:-/opt/homebrew/bin/clang++}"
[ -f "$B/librexruntime.dylib" ] || { echo "no $B/librexruntime.dylib" >&2; exit 1; }

mkdir -p "$WORK"
rm -rf "$WORK/run"
# src/main.cpp's defines and SDK include directories from build.ninja. The app build also
# force-includes src/ac6_sdk_compat.h (which pulls in generated/); the harness sources do not
# need it, and the syntax check with the full app flags covers that combination.
"$CXX" -std=gnu++23 -O1 -g -arch arm64 -mmacosx-version-min=12.0 -D_LIBCPP_DISABLE_AVAILABILITY \
  -DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL \
  -I"$REPO/src" -I"$SDK/include" -I"$B/rexglue-sdk/include" \
  -I"$SDK/thirdparty/simde" -I"$SDK/thirdparty/fmt/include" -I"$SDK/thirdparty/spdlog/include" \
  -isystem "$SDK/thirdparty/tomlplusplus/include" \
  -Wall -Wextra -Wno-unused-parameter -Wno-asm-operand-widths \
  "$HERE/driver_runtime_test.cpp" "$REPO/src/netplay_input_driver.cpp" \
  "$REPO/src/netplay_input_script.cpp" \
  -L"$B" -lrexruntime -Wl,-rpath,"$B" -o "$WORK/driver_runtime_test"
"$WORK/driver_runtime_test" "$WORK/run"
