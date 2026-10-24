#!/bin/bash
# Integration test for netplay/tools (no game needed).
#
#   netplay/tools/tests/test_tools.sh [work-dir]
#
# Uses the harness lane's test ports (UDP 47840, HTTP 47841), a scratch run root, a fake save
# source and a small compiled stand-in for the game binary. Builds the real Go server from
# TEST_SERVER_SRC (default: <repo>/netplay/server, else the server lane's tree). Every process
# it starts is stopped by PID before it exits.
set -uo pipefail

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$TOOLS/../.." && pwd)"
WORK="${1:-${TMPDIR:-/tmp}/ac6np_tools_test.$$}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd -P)"

if [ -z "${TEST_SERVER_SRC:-}" ]; then
  if [ -f "$REPO/netplay/server/go.mod" ]; then
    TEST_SERVER_SRC="$REPO/netplay/server"
  else
    TEST_SERVER_SRC="/Users/cole/ac6np-lanes/server/netplay/server"
  fi
fi

export AC6NP_RUN_ROOT="$WORK/run"
export AC6NP_SERVER_UDP="127.0.0.1:47840"
export AC6NP_SERVER_HTTP="127.0.0.1:47841"
export AC6NP_SERVER_SRC="$TEST_SERVER_SRC"
export AC6NP_SAVE_SRC="$WORK/fake_saves"
export AC6NP_ASSETS="$WORK/fake_assets"
export AC6NP_BINARY="$WORK/fake_bin/ac6recomp"

failures=0
checks=0
started_pids=()

pass() { checks=$((checks + 1)); }
fail() {
  checks=$((checks + 1))
  failures=$((failures + 1))
  printf '  FAILED: %s\n' "$*" >&2
}
check() {  # check "description" command...
  local what="$1"
  shift
  if "$@" > /dev/null 2>&1; then pass; else fail "$what"; fi
}
check_not() {  # check_not "description" command...  (passes when the command fails)
  local what="$1"
  shift
  if "$@" > /dev/null 2>&1; then fail "$what"; else pass; fi
}
section() { printf '== %s\n' "$*"; }

cleanup() {
  "$TOOLS/stop_all.sh" > /dev/null 2>&1 || true
  local p
  for p in ${started_pids[@]+"${started_pids[@]}"}; do
    kill "$p" 2> /dev/null || true
  done
}
trap cleanup EXIT

# --- fixtures ---------------------------------------------------------------------------
section "fixtures"
mkdir -p "$AC6NP_SAVE_SRC/B13EBABEBABEBABE/4E4D07D1" "$AC6NP_SAVE_SRC/0000000000000000/4E4D07D1" \
  "$AC6NP_SAVE_SRC/dlc/PKG1" "$AC6NP_SAVE_SRC/cache/shaders/shareable" "$AC6NP_ASSETS" \
  "$WORK/fake_bin"
printf 'save-bytes' > "$AC6NP_SAVE_SRC/B13EBABEBABEBABE/4E4D07D1/SAVE"
printf 'marketplace' > "$AC6NP_SAVE_SRC/0000000000000000/4E4D07D1/LIC"
printf 'dlc' > "$AC6NP_SAVE_SRC/dlc/PKG1/data"
printf 'shader' > "$AC6NP_SAVE_SRC/cache/shaders/shareable/s.bin"
printf 'other' > "$AC6NP_SAVE_SRC/unrelated.txt"
: > "$AC6NP_ASSETS/default.xex"
source_digest() {
  (cd "$AC6NP_SAVE_SRC" && find . -print0 | sort -z | xargs -0 stat -f '%N %z %m %p' 2> /dev/null)
}
before_digest="$(source_digest)"

cat > "$WORK/fake_game.c" << 'EOF'
/* Stand-in for the game: records argv/cwd/env, then waits; grows RSS on request. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char** argv) {
  FILE* f = fopen("fake_game_args.txt", "w");
  char cwd[4096];
  if (!f || !getcwd(cwd, sizeof cwd)) return 1;
  fprintf(f, "cwd=%s\n", cwd);
  for (int i = 1; i < argc; ++i) fprintf(f, "arg=%s\n", argv[i]);
  const char* m = getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS");
  fprintf(f, "mvk_sync=%s\n", m ? m : "");
  fclose(f);
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--grow") == 0) {
      size_t n = 64u << 20;
      char* p = malloc(n);
      if (p) memset(p, 1, n);
    }
  }
  for (;;) pause();
}
EOF
check "compile fake game" cc -O1 -o "$AC6NP_BINARY" "$WORK/fake_game.c"

# --- dev server ---------------------------------------------------------------------------
section "dev_server.sh"
out="$("$TOOLS/dev_server.sh" start 2>&1)"
rc=$?
printf '%s\n' "$out" | sed -e 's/^/  | /'
check "server starts" test "$rc" -eq 0
key="$(printf '%s\n' "$out" | sed -n 's/^server public key: \([0-9a-f]\{64\}\)$/\1/p' | tail -n 1)"
check "pubkey printed" test "${#key}" -eq 64
check "pidfile written" test -s "$AC6NP_RUN_ROOT/server.pid"
server_pid="$(cat "$AC6NP_RUN_ROOT/server.pid" 2> /dev/null)"
check "server alive" kill -0 "$server_pid"
check "server.key is 0600" test "$(stat -f '%Lp' "$AC6NP_RUN_ROOT/server/server.key")" = 600
check "server.key outside the repo" test ! -e "$REPO/netplay/server/server.key"
check "listening on test port" test -n "$(lsof -nP -ti udp:47840)"
if command -v curl > /dev/null; then
  check "status page" curl -fsS --max-time 2 "http://127.0.0.1:47841/status"
fi
out="$("$TOOLS/dev_server.sh" start 2>&1)"
check "second start reports running" grep -q "already running" <<< "$out"
check "pubkey subcommand" test "$("$TOOLS/dev_server.sh" pubkey)" = "$key"

# --- make_instance ------------------------------------------------------------------------
section "make_instance.sh"
check "instance A" "$TOOLS/make_instance.sh" A Talisman --force-relay
check "instance B" "$TOOLS/make_instance.sh" B Avril
A="$AC6NP_RUN_ROOT/instances/A"
B="$AC6NP_RUN_ROOT/instances/B"
check "A saves cloned" test "$(cat "$A/B13EBABEBABEBABE/4E4D07D1/SAVE")" = save-bytes
check "A marketplace cloned" test -f "$A/0000000000000000/4E4D07D1/LIC"
check "A dlc cloned" test -f "$A/dlc/PKG1/data"
check "A shaders cloned" test -f "$A/cache/shaders/shareable/s.bin"
check "unrelated files not copied" test ! -e "$A/unrelated.txt"
check "launch.sh executable" test -x "$A/launch.sh"
check "shots dir" test -d "$A/shots"
cat > "$WORK/check_toml.py" << 'EOF'
import sys, tomllib
path, name, tag, relay, key, udp = sys.argv[1:7]
with open(path, "rb") as f:
    c = tomllib.load(f)
n = c["netplay"]
want = {"enabled": True, "server": udp, "server_key": key, "gamertag": tag, "port": 0,
        "force_relay": relay == "true", "privacy": False, "lan": True,
        "input_script": "", "dump_spa": ""}
for k, v in want.items():
    assert n[k] == v, (k, n[k], v)
assert n["trace"].endswith("/instances/" + name + "/trace.jsonl"), n["trace"]
assert n["screenshot_dir"].endswith("/instances/" + name + "/shots"), n["screenshot_dir"]
assert c["fullscreen"] is False and c["window_width"] == 960 and c["window_height"] == 540
assert c["log"]["levels"]["netinput"] == "info"
EOF
check "A toml" python3 -I "$WORK/check_toml.py" "$A/ac6recomp.toml" A Talisman true "$key" 127.0.0.1:47840
check "B toml" python3 -I "$WORK/check_toml.py" "$B/ac6recomp.toml" B Avril false "$key" 127.0.0.1:47840
printf 'played' > "$A/B13EBABEBABEBABE/4E4D07D1/SAVE"
check "refresh A" "$TOOLS/make_instance.sh" A Talisman
check "refresh keeps instance saves" test "$(cat "$A/B13EBABEBABEBABE/4E4D07D1/SAVE")" = played
check "refresh rewrites toml" python3 -I "$WORK/check_toml.py" "$A/ac6recomp.toml" A Talisman false "$key" 127.0.0.1:47840
check_not "bad name rejected" "$TOOLS/make_instance.sh" "a/b" X
check_not "long gamertag rejected" "$TOOLS/make_instance.sh" C ThisNameIsTooLong
check_not "bad gamertag rejected" "$TOOLS/make_instance.sh" C 'Bad"Tag'
check_not "unknown option rejected" "$TOOLS/make_instance.sh" C Tag --bogus
check "save source unchanged" test "$(source_digest)" = "$before_digest"

# --- launch -------------------------------------------------------------------------------
section "launch_instance.sh"
out="$("$A/launch.sh" --dry-run 2>&1)"
check "dry run names user_data_root" grep -q -- "--user_data_root=$A" <<< "$out"
check "dry run names assets" grep -q -- "--game_data_root=$AC6NP_ASSETS" <<< "$out"
check "dry run starts nothing" test ! -e "$A/ac6recomp.pid"
printf 'wait 10\n' > "$WORK/script.txt"
: > "$A/trace.jsonl"
printf '{"ev":"old"}\n' > "$A/trace.jsonl"
check "launch A" "$A/launch.sh" --input-script "$WORK/script.txt" --max-rss-mib 0 -- --extra_flag=1
a_pid="$(cat "$A/ac6recomp.pid" 2> /dev/null)"
check "A running" kill -0 "$a_pid"
check "A comm" test "$(basename "$(ps -p "$a_pid" -o comm=)")" = ac6recomp
sleep 0.3
args="$(cat "$A/fake_game_args.txt" 2> /dev/null)"
check "cwd is the instance" grep -qx "cwd=$A" <<< "$args"
check "user_data_root arg" grep -qx "arg=--user_data_root=$A" <<< "$args"
check "input script arg (per-instance copy)" grep -qx "arg=--netplay_input_script=$A/input_script.txt" <<< "$args"
check "script copied" cmp -s "$WORK/script.txt" "$A/input_script.txt"
check "extra flag passed" grep -qx "arg=--extra_flag=1" <<< "$args"
check "MoltenVK env" grep -qx "mvk_sync=1" <<< "$args"
check "old trace moved to history" test -n "$(ls "$A/history"/trace-*.jsonl 2> /dev/null)"
check_not "second launch refused" "$A/launch.sh"
section "stop and PID safety"
check "stop A" "$TOOLS/stop_instance.sh" A
sleep 0.2
check_not "A stopped" kill -0 "$a_pid"
check "A pidfile removed" test ! -e "$A/ac6recomp.pid"
sleep 30 &
decoy=$!
started_pids+=("$decoy")
printf '%s\n' "$decoy" > "$A/ac6recomp.pid"
"$TOOLS/stop_instance.sh" A > /dev/null 2>&1
check "foreign PID in pidfile is not killed" kill -0 "$decoy"
kill "$decoy" 2> /dev/null
wait "$decoy" 2> /dev/null || true
check "launch over a stale pidfile" "$A/launch.sh" --max-rss-mib 0
check "stop A again" "$TOOLS/stop_instance.sh" A

section "rss_guard.sh"
check "launch with a tiny RSS limit" "$A/launch.sh" --max-rss-mib 1 -- --grow
g_pid="$(cat "$A/ac6recomp.pid" 2> /dev/null)"
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  kill -0 "$g_pid" 2> /dev/null || break
  sleep 0.5
done
check_not "guard stopped the game" kill -0 "$g_pid"
check "guard logged it" grep -q "over the 1 MiB limit" "$A/rss_guard.log"

# --- pair ---------------------------------------------------------------------------------
section "launch_pair.sh / stop_all.sh"
touch "$AC6NP_RUN_ROOT/instances/host_session_ready.flag"
out="$("$TOOLS/launch_pair.sh" --dry-run 2>&1)"
check "pair dry run" grep -q -- "--user_data_root=$B" <<< "$out"
check "old flag removed" test ! -e "$AC6NP_RUN_ROOT/instances/host_session_ready.flag"
check "pair launch" "$TOOLS/launch_pair.sh" --stagger 0 --script-b "$WORK/script.txt"
pa="$(cat "$A/ac6recomp.pid" 2> /dev/null)"
pb="$(cat "$B/ac6recomp.pid" 2> /dev/null)"
check "pair A running" kill -0 "$pa"
check "pair B running" kill -0 "$pb"
check "stop_all" "$TOOLS/stop_all.sh"
sleep 0.3
check_not "A stopped by stop_all" kill -0 "$pa"
check_not "B stopped by stop_all" kill -0 "$pb"
check_not "server stopped by stop_all" kill -0 "$server_pid"
check "server pidfile removed" test ! -e "$AC6NP_RUN_ROOT/server.pid"
check "port 47840 released" test -z "$(lsof -nP -ti udp:47840)"
check "save source still unchanged" test "$(source_digest)" = "$before_digest"

# --- trace_wait.py ------------------------------------------------------------------------
section "trace_wait.py"
TW="$TOOLS/trace_wait.py"
T="$WORK/trace.jsonl"
rm -f "$T"
(sleep 0.4 && printf '{"ev":"a"}\n{"ev":"xam.' >> "$T" && sleep 0.3 && printf 'live"}\n' >> "$T") &
writer=$!
started_pids+=("$writer")
out="$(python3 -I "$TW" "$T" '"ev":"xam.live"' 5)"
check "waits for a file that appears later" test "$out" = '{"ev":"xam.live"}'
wait "$writer"
python3 -I "$TW" "$T" xam.live 0.3 --from-end > /dev/null 2>&1
check "from-end ignores old lines (exit 1)" test $? -eq 1
printf '{"ev":"xam.live","n":2}\n' >> "$T"
check "count 2" python3 -I "$TW" "$T" xam.live 1 --count 2 --quiet
python3 -I "$TW" "$T" never 0.2 > /dev/null 2>&1
check "timeout exit 1" test $? -eq 1
python3 -I "$TW" "$T" > /dev/null 2>&1
check "usage exit 2" test $? -eq 2
yes old | head -n 1000 > "$T"
(sleep 0.4 && printf 'fresh\n' > "$T") &
started_pids+=("$!")
check "truncated file is reread" python3 -I "$TW" "$T" fresh 3 --quiet

printf '%d checks, %d failed\n' "$checks" "$failures"
if [ "$failures" -eq 0 ] && [ -z "${KEEP_WORK:-}" ]; then
  trap - EXIT
  cleanup
  rm -rf "$WORK"
fi
[ "$failures" -eq 0 ]
