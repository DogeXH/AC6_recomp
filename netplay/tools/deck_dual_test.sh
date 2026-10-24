#!/bin/bash
# Player Match test with two instances on the Steam Deck, side by side in its desktop session:
# D (inst) and E (inst-e), driven from this Mac. run/instances/{D,E}/remote name the remote dirs;
# np-launch.sh / np-stop.sh <D|E> live next to the Deck's ac6-launch.py. AC6NP_PAIR picks host
# and client (default "D E").
#   deck_dual_test.sh [seconds]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
secs="${1:-120}"
D="$RUN/instances/D"
E="$RUN/instances/E"
read -r rhost rdir < "$D/remote"
T="$(dirname "$rdir")"
ssh_d() { ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" \
  -o ControlPersist=20m -o "UserKnownHostsFile=$D/known_hosts" "$rhost" "$@"; }
ssh_d "$T/np-stop.sh D; $T/np-stop.sh E" || true
rm -rf "$D/repl" "$E/repl"; mkdir -p "$D/repl" "$E/repl"
# AC6NP_IMPAIR="delay=50,jitter=15,loss=0.02" starts both instances with the TEST-ONLY network
# impairment (AC6NET_TEST_IMPAIR, see netplay/docs/TESTING.md); unset = normal network.
imp="${AC6NP_IMPAIR:-}"
ssh_d "export AC6NP_DUAL=1 AC6NET_TEST_IMPAIR='$imp'; $T/np-launch.sh D && sleep 4 && $T/np-launch.sh E"
sleep 15
AC6NP_PAIR="${AC6NP_PAIR:-D E}" python3 -I "$HERE/ac6_drive.py" "$D" pair-test "$secs"
