#!/usr/bin/env bash
# Builds and runs the standalone vendored-SDK glue tests (marshal layouts, QoS results, select
# plans; no SDK or game needed) with the container's clang, plain and with UBSan.
set -euo pipefail
cd /src
X=thirdparty/rexglue-sdk/src/kernel/xam
O=out/glue-tests-linux
mkdir -p "$O"
for t in netplay_xam_test netplay_net_test; do
  clang++ -std=c++20 -O2 -g -Wall -Wextra -Werror -Wno-comment -Inetplay/core/include -I"$X" \
    "$X/$t.cpp" -o "$O/$t"
  "$O/$t"
  clang++ -std=c++20 -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined -Wno-comment \
    -Inetplay/core/include -I"$X" "$X/$t.cpp" -o "$O/${t}_ubsan"
  "$O/${t}_ubsan"
done
