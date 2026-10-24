#!/usr/bin/env bash
# Runs the ac6net unit tests and the [server] tests inside the ac6-build container. Use
# `docker run --network none` so the fixed test ports exist only in the container's loopback.
# Requires `build-linux.sh tests` first (out/ac6net-linux/ac6net_tests, out/ac6lobby-linux/ac6lobby).
set -uo pipefail
cd /src
T=out/ac6net-linux/ac6net_tests
L=out/ac6lobby-linux/ac6lobby
rc=0
echo "== 1. unit tests"
"$T" || rc=1
echo "== 2. [server] tests, self-started lobby (AC6NET_TEST_SERVER_BIN)"
AC6NET_TEST_SERVER_BIN="$PWD/$L" "$T" "[server]" || rc=1
echo "== 3. [server] tests against an external lobby on 127.0.0.1:47870"
data=$(mktemp -d)
"$L" -data "$data" -udp 127.0.0.1:47870 -http "" -log-level warn >"$data/server.log" 2>&1 &
lobby_pid=$!
for _ in $(seq 1 100); do [ -s "$data/server.pub" ] && break; sleep 0.1; done
key=$(tr -d '\n' <"$data/server.pub" 2>/dev/null || true)
if [ ${#key} -eq 64 ]; then
  AC6NET_TEST_SERVER=127.0.0.1:47870 AC6NET_TEST_SERVER_KEY="$key" "$T" "[server]" || rc=1
else
  echo "lobby did not start"; cat "$data/server.log"; rc=1
fi
kill "$lobby_pid" 2>/dev/null; wait "$lobby_pid" 2>/dev/null
rm -rf "$data"
echo "== overall rc=$rc"
exit $rc
