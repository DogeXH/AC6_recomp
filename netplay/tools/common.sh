# shellcheck shell=bash
# Shared settings and helpers for the netplay test tools. Source it; do not run it.
# Bash 3.2 compatible (macOS /bin/bash). See netplay/docs/TESTING.md.
#
# Every path and address can be overridden from the environment:
#   AC6NP_RUN_ROOT     run directory (server data, instances, logs; never inside the repo)
#   AC6NP_SERVER_UDP   lobby UDP address         (default 127.0.0.1:47800)
#   AC6NP_SERVER_HTTP  lobby HTTP status address (default 127.0.0.1:47801)
#   AC6NP_SERVER_SRC   Go server sources         (default <repo>/netplay/server)
#   AC6NP_BINARY       game executable inside the app bundle
#   AC6NP_ASSETS       game data root (the extracted game files)
#   AC6NP_SAVE_SRC     user data to clone saves/DLC/shaders from (read only)

# shellcheck disable=SC2034  # variables are used by the scripts that source this file

AC6NP_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AC6NP_REPO_DIR="$(cd "$AC6NP_TOOLS_DIR/../.." && pwd)"

AC6NP_RUN_ROOT="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
AC6NP_SERVER_UDP="${AC6NP_SERVER_UDP:-127.0.0.1:47800}"
AC6NP_SERVER_HTTP="${AC6NP_SERVER_HTTP:-127.0.0.1:47801}"
AC6NP_SERVER_SRC="${AC6NP_SERVER_SRC:-$AC6NP_REPO_DIR/netplay/server}"
AC6NP_BINARY="${AC6NP_BINARY:-/Users/cole/AC6_recomp_netplay/out/build/mac-arm64-release/AC6 Recomp 1.0.0.app/Contents/MacOS/ac6recomp}"
AC6NP_ASSETS="${AC6NP_ASSETS:-/Users/cole/AC6_recomp_netplay/assets}"
AC6NP_SAVE_SRC="${AC6NP_SAVE_SRC:-$HOME/.local/share/ac6recomp}"

AC6NP_SERVER_DATA="$AC6NP_RUN_ROOT/server"
AC6NP_SERVER_BIN="$AC6NP_RUN_ROOT/bin/ac6lobby"
AC6NP_SERVER_LOG="$AC6NP_RUN_ROOT/server.log"
AC6NP_SERVER_PIDFILE="$AC6NP_RUN_ROOT/server.pid"
AC6NP_INSTANCES="$AC6NP_RUN_ROOT/instances"

die() {
  printf '%s: error: %s\n' "${0##*/}" "$*" >&2
  exit 1
}

note() {
  printf '%s: %s\n' "${0##*/}" "$*"
}

# The port part of "host:port" or "[v6]:port".
port_of() {
  printf '%s\n' "${1##*:}"
}

# Prints the PID stored in a pidfile if that process is alive and its executable is named
# $2 (basename). Returns 1 otherwise. A recycled PID that now belongs to another program is
# never reported, so callers never signal a process they did not start.
pid_from_file() {
  local pidfile="$1" expected="$2" pid comm
  [ -f "$pidfile" ] || return 1
  pid="$(tr -cd '0-9' < "$pidfile")"
  [ -n "$pid" ] || return 1
  comm="$(ps -p "$pid" -o comm= 2>/dev/null)" || return 1
  [ -n "$comm" ] || return 1
  [ "${comm##*/}" = "$expected" ] || return 1
  printf '%s\n' "$pid"
}

# Stops the process recorded in a pidfile (TERM, then KILL after a grace period) if it is
# still the program we started, and removes the pidfile.
stop_pidfile() {
  local pidfile="$1" expected="$2" what="$3" grace="${4:-10}" pid i
  if ! pid="$(pid_from_file "$pidfile" "$expected")"; then
    if [ -f "$pidfile" ]; then
      note "$what is not running (stale pidfile removed)"
      rm -f "$pidfile"
    else
      note "$what is not running"
    fi
    return 0
  fi
  note "stopping $what (PID $pid)"
  kill -TERM "$pid" 2>/dev/null || true
  i=0
  while [ "$i" -lt $((grace * 10)) ]; do
    pid_from_file "$pidfile" "$expected" > /dev/null || break
    sleep 0.1
    i=$((i + 1))
  done
  if pid_from_file "$pidfile" "$expected" > /dev/null; then
    note "$what did not exit after ${grace}s; sending KILL to PID $pid"
    kill -KILL "$pid" 2>/dev/null || true
    sleep 0.2
  fi
  rm -f "$pidfile"
}

# PIDs listening on a local UDP port (empty if none).
udp_port_pids() {
  lsof -nP -ti "udp:$1" 2>/dev/null || true
}

tcp_port_pids() {
  lsof -nP -ti "tcp:$1" -sTCP:LISTEN 2>/dev/null || true
}

# The server's public key (64 hex digits) from the run directory, or nothing.
server_pubkey() {
  local key
  [ -f "$AC6NP_SERVER_DATA/server.pub" ] || return 1
  key="$(tr -cd '0-9a-fA-F' < "$AC6NP_SERVER_DATA/server.pub")"
  [ "${#key}" -eq 64 ] || return 1
  printf '%s\n' "$key"
}

timestamp() {
  date +%Y%m%d-%H%M%S
}

# TOML basic-string escaping for values we write (backslash and double quote).
toml_string() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '"%s"' "$s"
}

instance_dir() {
  printf '%s/%s\n' "$AC6NP_INSTANCES" "$1"
}

valid_instance_name() {
  case "$1" in
    "" | *[!A-Za-z0-9_-]*) return 1 ;;
  esac
  [ "${#1}" -le 32 ]
}
