#!/bin/bash
# Sends one repl batch to a Deck test instance (Windows W/W2 or Linux D/E) through its Mac mirror
# $RUN/instances/<name> (created by win_deploy.sh) and prints the done file; see repl_cmd.sh.
#   win_repl.sh <W|W2|D|E> [--timeout SEC] [--shot NAME] -- <line>...
# Paths in lines are Deck paths: a Windows instance opens /run/media/... paths as they are (Wine
# maps them to its D:/Z: drives), e.g. "screenshot /run/media/deck/SN256/ACE6-Netplay-Win/W/shots/x.ppm".
set -euo pipefail
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
[ "$#" -ge 1 ] || { echo "usage: win_repl.sh <W|W2|D|E> [--timeout SEC] [--shot NAME] -- <line>..." >&2; exit 2; }
inst="$RUN/instances/$1"; shift
[ -f "$inst/remote" ] || { echo "no mirror $inst/remote (run win_deploy.sh --no-exe)" >&2; exit 1; }
# win-launch.sh / np-launch.sh clear the Deck's repl queue on every launch, so after a relaunch
# the local counter (repl/.next) is stale and the game would wait forever for cmd_0001. Resync it
# from the Deck. The game runs cmd_<d+1> after done_<d>: if cmd_<d+1> exists (queued or running),
# continue after the last contiguous cmd; otherwise any higher cmd files are stale (sent with an
# old counter, never to run) and are removed, and the next command is d+1.
read -r rhost rdir < "$inst/remote"
next=$(ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" -o ControlPersist=20m \
  -o "UserKnownHostsFile=$inst/known_hosts" "$rhost" "cd '$rdir/repl' 2>/dev/null || { echo 1; exit; }
  d=\$(ls | sed -n 's/^done_0*\([0-9][0-9]*\)\.txt\$/\1/p' | sort -n | tail -1); d=\${d:-0}; n=\$((d + 1))
  if [ -f \$(printf cmd_%04d.txt \$n) ]; then while [ -f \$(printf cmd_%04d.txt \$n) ]; do n=\$((n + 1)); done
  else for f in cmd_*.txt; do [ -f \"\$f\" ] && rm -f \"\$f\"; done; fi; echo \$n" || true)
mkdir -p "$inst/repl"; echo "${next:-1}" > "$inst/repl/.next"
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/repl_cmd.sh" "$inst" "$@"
