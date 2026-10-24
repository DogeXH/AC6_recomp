#!/bin/bash
# Local ac6lobby dev server for two-instance testing.
#
#   dev_server.sh [start]   build netplay/server, start it in the background, print the pubkey
#   dev_server.sh stop      stop the server started here (by its recorded PID)
#   dev_server.sh status    running or not, pubkey, /status counts
#   dev_server.sh pubkey    print the server public key (64 hex)
#
# Defaults: UDP 127.0.0.1:47800, HTTP status 127.0.0.1:47801, data dir <run>/server (server.key
# stays outside the repo). Override with AC6NP_* variables (see common.sh).
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

build_server() {
  [ -f "$AC6NP_SERVER_SRC/go.mod" ] || die "no Go server at $AC6NP_SERVER_SRC (set AC6NP_SERVER_SRC)"
  command -v go > /dev/null || die "go is not installed"
  mkdir -p "$(dirname "$AC6NP_SERVER_BIN")"
  local mod_flag=()
  [ -d "$AC6NP_SERVER_SRC/vendor" ] && mod_flag=(-mod=vendor)
  note "building $AC6NP_SERVER_SRC -> $AC6NP_SERVER_BIN"
  (cd "$AC6NP_SERVER_SRC" && go build ${mod_flag[@]+"${mod_flag[@]}"} -trimpath -o "$AC6NP_SERVER_BIN.new" .) ||
    die "go build failed"
  mv -f "$AC6NP_SERVER_BIN.new" "$AC6NP_SERVER_BIN"
}

print_key() {
  local key
  key="$(server_pubkey)" || die "no server key yet in $AC6NP_SERVER_DATA (start the server once)"
  printf 'server public key: %s\n' "$key"
}

start() {
  local pid
  if pid="$(pid_from_file "$AC6NP_SERVER_PIDFILE" ac6lobby)"; then
    note "already running (PID $pid) on udp $AC6NP_SERVER_UDP"
    print_key
    return 0
  fi
  local udp_port http_port
  udp_port="$(port_of "$AC6NP_SERVER_UDP")"
  http_port="$(port_of "$AC6NP_SERVER_HTTP")"
  [ -z "$(udp_port_pids "$udp_port")" ] ||
    die "UDP port $udp_port is already in use by PID(s) $(udp_port_pids "$udp_port" | tr '\n' ' ')"
  [ -z "$(tcp_port_pids "$http_port")" ] ||
    die "TCP port $http_port is already in use by PID(s) $(tcp_port_pids "$http_port" | tr '\n' ' ')"

  build_server
  mkdir -p "$AC6NP_SERVER_DATA"
  chmod 700 "$AC6NP_SERVER_DATA"
  [ ! -f "$AC6NP_SERVER_LOG" ] || mv -f "$AC6NP_SERVER_LOG" "$AC6NP_SERVER_LOG.prev"

  nohup "$AC6NP_SERVER_BIN" -udp "$AC6NP_SERVER_UDP" -http "$AC6NP_SERVER_HTTP" \
    -data "$AC6NP_SERVER_DATA" -log-level "${AC6NP_SERVER_LOG_LEVEL:-info}" \
    < /dev/null >> "$AC6NP_SERVER_LOG" 2>&1 &
  pid=$!
  printf '%s\n' "$pid" > "$AC6NP_SERVER_PIDFILE"

  # Wait until it listens (the log names the bound address) or dies.
  local i=0
  while [ "$i" -lt 100 ]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      rm -f "$AC6NP_SERVER_PIDFILE"
      sed -e 's/^/  server: /' "$AC6NP_SERVER_LOG" >&2 || true
      die "server exited during startup (log: $AC6NP_SERVER_LOG)"
    fi
    if grep -q 'msg=listening' "$AC6NP_SERVER_LOG" 2>/dev/null && server_pubkey > /dev/null; then
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  [ "$i" -lt 100 ] || die "server did not report listening within 10 s (PID $pid, log $AC6NP_SERVER_LOG)"
  note "started PID $pid on udp $AC6NP_SERVER_UDP (status http://$AC6NP_SERVER_HTTP/status)"
  note "pidfile $AC6NP_SERVER_PIDFILE, log $AC6NP_SERVER_LOG, data $AC6NP_SERVER_DATA"
  print_key
}

status() {
  local pid
  if pid="$(pid_from_file "$AC6NP_SERVER_PIDFILE" ac6lobby)"; then
    note "running (PID $pid) on udp $AC6NP_SERVER_UDP"
  else
    note "not running"
  fi
  server_pubkey > /dev/null && print_key
  if [ -n "${pid:-}" ] && command -v curl > /dev/null; then
    curl -fsS --max-time 2 "http://$AC6NP_SERVER_HTTP/status" && printf '\n'
  fi
}

case "${1:-start}" in
  start) start ;;
  stop) stop_pidfile "$AC6NP_SERVER_PIDFILE" ac6lobby "dev server" 5 ;;
  status) status ;;
  pubkey) server_pubkey || die "no server key yet in $AC6NP_SERVER_DATA" ;;
  -h | --help | help) sed -n '2,11p' "$0" ;;
  *) die "unknown command '$1' (start, stop, status, pubkey)" ;;
esac
