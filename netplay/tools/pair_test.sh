#!/bin/bash
# Full two-instance Player Match test on one machine:
#   restarts instances A and B (repl input scripts), boots both, A hosts, B quick-matches,
#   B readies, A starts, and match frames are recorded for <seconds> (default 120).
#   pair_test.sh [seconds] [--force-relay]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
secs="${1:-120}"
for n in A B; do "$HERE/stop_instance.sh" "$n" >/dev/null 2>&1 || true; done
for n in A B; do
  d="$RUN/instances/$n"
  rm -rf "$d/repl"; mkdir -p "$d/repl"
done
"$RUN/instances/A/launch.sh" >/dev/null
sleep 4
"$RUN/instances/B/launch.sh" >/dev/null
sleep 15
python3 -I "$HERE/ac6_drive.py" "$RUN/instances/A" pair-test "$secs"
