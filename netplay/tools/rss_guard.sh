#!/bin/bash
# Memory watchdog for one test instance (started by launch_instance.sh).
#
#   rss_guard.sh <pidfile> <limit_mib> <logfile>
#
# Samples the game's resident memory every second and stops it (TERM, then KILL) once it passes
# the limit, like the cutoff in run-ac6-guarded.sh. Exits when the game exits.
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ "$#" -eq 3 ] || die "usage: rss_guard.sh <pidfile> <limit_mib> <logfile>"
pidfile="$1"
limit_mib="$2"
log="$3"
case "$limit_mib" in "" | *[!0-9]*) die "limit must be a number of MiB" ;; esac
limit_kib=$((limit_mib * 1024))
peak_kib=0

log_line() {
  printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$log"
}

pid="$(pid_from_file "$pidfile" ac6recomp)" || exit 0
log_line "watching PID $pid, limit $limit_mib MiB"
while pid_from_file "$pidfile" ac6recomp > /dev/null; do
  rss_kib="$(ps -o rss= -p "$pid" 2> /dev/null | tr -cd '0-9')"
  if [ -n "$rss_kib" ]; then
    [ "$rss_kib" -le "$peak_kib" ] || peak_kib="$rss_kib"
    if [ "$rss_kib" -gt "$limit_kib" ]; then
      log_line "RSS $((rss_kib / 1024)) MiB is over the $limit_mib MiB limit; stopping PID $pid"
      stop_pidfile "$pidfile" ac6recomp "the game" 5 >> "$log" 2>&1
      break
    fi
  fi
  sleep 1
done
log_line "PID $pid gone; peak RSS $((peak_kib / 1024)) MiB"
# launch_instance.sh records this guard's PID next to the log; drop it once we are done.
guard_pidfile="$(dirname "$log")/rss_guard.pid"
if [ "$(tr -cd '0-9' < "$guard_pidfile" 2> /dev/null)" = "$$" ]; then
  rm -f "$guard_pidfile"
fi
