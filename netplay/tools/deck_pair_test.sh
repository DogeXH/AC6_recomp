#!/bin/bash
# Mac <-> Steam Deck Player Match test. Instance A runs on this Mac, instance D on the Deck
# (run/instances/D/remote names the host and its instance dir; np-launch.sh / np-stop.sh live
# next to the Deck's ac6-launch.py). The first instance in AC6NP_PAIR hosts (default "A D").
#   deck_pair_test.sh [seconds]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
secs="${1:-120}"
D="$RUN/instances/D"
read -r rhost rdir < "$D/remote"
ssh_d() { ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" \
  -o ControlPersist=20m -o "UserKnownHostsFile=$D/known_hosts" "$rhost" "$@"; }
"$HERE/stop_instance.sh" A >/dev/null 2>&1 || true
ssh_d "$(dirname "$rdir")/np-stop.sh" || true
rm -rf "$RUN/instances/A/repl" "$D/repl"; mkdir -p "$RUN/instances/A/repl" "$D/repl"
"$RUN/instances/A/launch.sh" >/dev/null
ssh_d "$(dirname "$rdir")/np-launch.sh"
sleep 15
AC6NP_PAIR="${AC6NP_PAIR:-A D}" python3 -I "$HERE/ac6_drive.py" "$RUN/instances/A" pair-test "$secs"
