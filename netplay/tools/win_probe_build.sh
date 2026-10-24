#!/bin/bash
# Builds win_probe.exe (the rig self-test, see netplay/docs/TESTING_WINDOWS.md) with the Windows
# build's toolchain: clang targeting x86_64-pc-windows-msvc, lld-link and the xwin CRT/SDK splat.
#   win_probe_build.sh [out.exe]     (default: <worktree>/out/win_probe/win_probe.exe)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
XWIN="${AC6NP_XWIN:-/Users/cole/ac6np-win/xwin}"
LLVM="${AC6NP_LLVM:-/opt/homebrew/opt/llvm@21/bin}"
LLD="${AC6NP_LLD:-/opt/homebrew/opt/lld@21/bin}"
out="${1:-$ROOT/out/win_probe/win_probe.exe}"
mkdir -p "$(dirname "$out")"
PATH="$LLD:$LLVM:$PATH" "$LLVM/clang" --target=x86_64-pc-windows-msvc -O2 -fuse-ld=lld \
  -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations \
  -isystem "$XWIN/crt/include" -isystem "$XWIN/sdk/include/ucrt" -isystem "$XWIN/sdk/include/um" \
  -isystem "$XWIN/sdk/include/shared" \
  -I "$ROOT/thirdparty/rexglue-sdk/thirdparty/vulkan-headers/include" \
  "$HERE/win_probe.c" -o "$out" \
  -L "$XWIN/crt/lib/x86_64" -L "$XWIN/sdk/lib/um/x86_64" -L "$XWIN/sdk/lib/ucrt/x86_64" \
  -Wl,-subsystem:console -lws2_32 -luser32 -lgdi32 -lkernel32 -llibcmt -llibucrt -llibvcruntime
echo "$out"
