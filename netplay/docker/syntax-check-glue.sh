#!/usr/bin/env bash
# Compile-checks the netplay-touched vendored-SDK sources with the exact flags CMake generated
# for them (out/build/<preset>/build.ninja), netplay on (REX_HAVE_AC6NET=1, as configured) and
# compiled out (-UREX_HAVE_AC6NET). Light: one clang process at a time. Run inside ac6-build.
#   netplay/docker/syntax-check-glue.sh [extra clang args, e.g. -c -o /dev/null]
set -uo pipefail
cd /src
B=${B:-out/build/linux-amd64-relwithdebinfo}
MODE=${MODE:--fsyntax-only}
FILES=(
  thirdparty/rexglue-sdk/src/kernel/xam/netplay_glue.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/xam_net.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/xam_user.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/xam_msg.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/xam_ui.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/apps/xgi_app.cpp
  thirdparty/rexglue-sdk/src/kernel/xam/apps/xlivebase_app.cpp
  thirdparty/rexglue-sdk/src/system/kernel_state.cpp
  thirdparty/rexglue-sdk/src/input/input_system.cpp
  thirdparty/rexglue-sdk/src/native/ui/window.cpp
  thirdparty/rexglue-sdk/src/native/ui/window_gtk.cpp
)
fail=0
for f in "${FILES[@]}"; do
  block=$(awk -v f="/src/$f" '$1=="build" && index($0, " " f " ")>0 {p=1; next} p && /^  (DEFINES|INCLUDES|FLAGS) =/ {print} p && /^$/ {exit}' "$B/build.ninja")
  if [ -z "$block" ]; then echo "NOFLAGS $f"; fail=1; continue; fi
  defs=$(sed -n 's/^  DEFINES = //p' <<<"$block"); incs=$(sed -n 's/^  INCLUDES = //p' <<<"$block")
  flags=$(sed -n 's/^  FLAGS = //p' <<<"$block")
  for variant in on off; do
    d="$defs"; [ "$variant" = off ] && d=$(sed 's/-DREX_HAVE_AC6NET=1//' <<<"$defs")
    log=$(mktemp)
    # shellcheck disable=SC2086
    if clang++ $d $incs $flags $MODE "$@" "$f" >"$log" 2>&1; then
      w=$(grep -c "warning:" "$log"); nw=$(grep "warning:" "$log" | grep -c "netplay")
      echo "OK   [$variant] $f (warnings: $w, in netplay files: $nw)"
    else
      echo "FAIL [$variant] $f"; grep -E "error" "$log" | head -20; fail=1
    fi
    rm -f "$log"
  done
done
exit $fail
