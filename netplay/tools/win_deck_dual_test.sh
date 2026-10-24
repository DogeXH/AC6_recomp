#!/bin/bash
# Windows <-> Linux Player Match (or Ranked Match) test on the Steam Deck, driven from this Mac:
# Windows instance W (ac6recomp.exe under Proton, /run/media/deck/SN256/ACE6-Netplay-Win/W) and
# Linux instance D (/home/deck/Games/ACE6-Netplay-Test/inst), side by side in the desktop session.
# Reuses ac6_drive.py pair-test (boot both, host creates, client quick-matches, ready, start,
# record N seconds of match screenshots). See netplay/docs/TESTING_WINDOWS.md.
#
#   win_deck_dual_test.sh [--host W|D] [--win W|W2] [--peer D|W2] [--ranked] [--force-relay |
#                          --force-relay-w | --force-relay-d] [--impair SPEC] [--keep] [seconds]
#
#   --host W|D       which one hosts: the Windows side (W, default) or the peer (D)
#   --win W2         use instance W2 instead of W
#   --peer W2        the peer is Windows instance W2 instead of Linux D (Windows <-> Windows);
#                    "D" in --host and --force-relay-d then means that peer
#   --ranked         Ranked Match instead of Player Match (AC6NP_RANKED=1 for ac6_drive.py)
#   --force-relay    every peer packet relayed by the lobby (both); -w / -d for one side only
#   --impair SPEC    TEST-ONLY impairment for both, e.g. "delay=50,jitter=15,loss=0.02"
#   --keep           leave both instances running afterwards (default: stop both)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
host=W; win=W; peer=D; ranked=""; relay_w=""; relay_d=""; imp="${AC6NP_IMPAIR:-}"; keep=0; secs=120
while [ $# -gt 0 ]; do
  case "$1" in
    --host) host="$2"; shift ;;
    --win) win="$2"; shift ;;
    --peer) peer="$2"; shift ;;
    --ranked) ranked=1 ;;
    --force-relay) relay_w=1; relay_d=1 ;;
    --force-relay-w) relay_w=1 ;;
    --force-relay-d) relay_d=1 ;;
    --impair) imp="$2"; shift ;;
    --keep) keep=1 ;;
    [0-9]*) secs="$1" ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
D="$RUN/instances/D"; Wm="$RUN/instances/$win"; Pm="$RUN/instances/$peer"
[ "$peer" != "$win" ] || { echo "--win and --peer are the same instance" >&2; exit 2; }
[ -f "$Wm/remote" ] && [ -f "$Pm/remote" ] || "$HERE/win_deploy.sh" --no-exe "$win" $([ "$peer" = D ] || echo "$peer")
read -r rhost rdir < "$D/remote"
T="$(dirname "$rdir")"                      # Linux test install
read -r _ wdir < "$Wm/remote"
WT="$(dirname "$wdir")"                     # Windows test dir on the SD card
ssh_d() { ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" \
  -o ControlPersist=20m -o "UserKnownHostsFile=$D/known_hosts" "$rhost" "$@"; }
if [ "$peer" = D ]; then peer_stop="$T/np-stop.sh D"; peer_launch="$T/np-launch.sh D"
else peer_stop="$WT/win-stop.sh $peer"; peer_launch="$WT/win-launch.sh $peer"; fi
stop_both() { ssh_d "$WT/win-stop.sh $win; $peer_stop" || true; }
stop_both
# Only our own test instances may use the Deck GPU: anything left after stopping ours is Cole's.
other=$(ssh_d "pgrep -af -i 'acecombat|ac6recomp' | grep -v pgrep || true")
if [ -n "$other" ]; then echo "the Deck is running another AC6, not starting:"; echo "$other"; exit 1; fi
[ "$keep" = 1 ] || trap stop_both EXIT
rm -rf "$Pm/repl" "$Wm/repl"; mkdir -p "$Pm/repl" "$Wm/repl"
wargs=""; dargs=""
[ -n "$relay_w" ] && wargs="--netplay_force_relay=true"
[ -n "$relay_d" ] && dargs="--netplay_force_relay=true"
# Wine/Proton debug knobs set on the Mac reach the Windows instance(s) (TESTING_WINDOWS.md).
wenv=""
for v in AC6NP_WINEDEBUG AC6NP_PROTON_LOG AC6NP_FAST_SYNC AC6NP_PROTON PROTON_CRASH_REPORT_DIR; do
  [ -n "${!v:-}" ] && wenv="$wenv $v='${!v}'"
done
ssh_d "export AC6NP_DUAL=1 AC6NET_TEST_IMPAIR='$imp'$wenv; $peer_launch $dargs && sleep 4 && $WT/win-launch.sh $win $wargs"
sleep 20
if [ "$host" = W ]; then pair="$win $peer"; else pair="$peer $win"; fi
echo "win_deck_dual_test: host ${pair%% *}, client ${pair##* }${ranked:+, ranked}${relay_w:+, W relayed}${relay_d:+, D relayed}${imp:+, impair $imp}"
rc=0
AC6NP_PAIR="$pair" AC6NP_RANKED="$ranked" python3 -I "$HERE/ac6_drive.py" "$Wm" pair-test "$secs" || rc=$?
"$HERE/win_shot.sh" "dual_end" "${pair%% *}" "${pair##* }" || true
exit $rc
