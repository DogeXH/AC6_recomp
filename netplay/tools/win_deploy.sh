#!/bin/bash
# Deploys the Windows build to the Steam Deck's Proton test instances (netplay/docs/TESTING_WINDOWS.md).
#
#   win_deploy.sh [--scripts] [--setup] [--no-exe] [--stage DIR] [--exe-only NAME] [W] [W2]
#
#   (default)      rsync the build lane's stage (out/stage/win-amd64/) into <inst>/bin on the Deck,
#                  keeping the previous exe as bin/ac6recomp.exe.prev. W gets the copy; W2 gets
#                  hard links to W's files (same build, no extra space). Default instances: W W2.
#   --scripts      also push netplay/tools/win_deck/* (win-env.sh, win-launch.sh, win-stop.sh,
#                  win-run.py, win-setup.sh, the toml template) to the Deck's ACE6-Netplay-Win/
#   --setup        also run win-setup.sh for each instance (creates it / regenerates its toml;
#                  pass WIN_SETUP_ARGS="--backend d3d12" etc.)
#   --no-exe       skip the exe copy (scripts/setup only)
#   --stage DIR    deploy another staged dir (for example out/win_probe)
#   --pdb          also copy *.pdb (skipped by default: large, and Wine does not use them)
# Also creates the Mac-side mirrors $RUN/instances/{W,W2} (remote + known_hosts) that
# repl_cmd.sh / ac6_drive.py use. Refuses to touch an instance that is running.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
DECK="${AC6NP_DECK:-deck@steamdeck.local}"
WT=/run/media/deck/SN256/ACE6-Netplay-Win
stage="$ROOT/out/stage/win-amd64"
scripts=0; setup=0; exe=1; pdb=0; insts=()
while [ $# -gt 0 ]; do
  case "$1" in
    --scripts) scripts=1 ;;
    --setup) setup=1 ;;
    --no-exe) exe=0 ;;
    --pdb) pdb=1 ;;
    --stage) stage="$2"; shift ;;
    W|W2) insts+=("$1") ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
[ ${#insts[@]} -gt 0 ] || insts=(W W2)
KH="$RUN/instances/D/known_hosts"
SSH=(ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" -o ControlPersist=20m)
[ -f "$KH" ] && SSH+=(-o "UserKnownHostsFile=$KH")
deck() { "${SSH[@]}" "$DECK" "$@"; }
RSYNC_SSH="${SSH[*]}"

for i in "${insts[@]}"; do
  m="$RUN/instances/$i"; mkdir -p "$m/repl" "$m/shots"
  echo "$DECK $WT/$i" > "$m/remote"
  [ -f "$KH" ] && cp "$KH" "$m/known_hosts"
done

running=$(deck "for i in ${insts[*]}; do p=$WT/\$i/launcher.pid; [ -f \$p ] && kill -0 \$(cat \$p) 2>/dev/null && echo \$i; done; true")
if [ -n "$running" ]; then echo "running on the Deck, stop first ($WT/win-stop.sh <inst>): $running" >&2; exit 1; fi

if [ "$scripts" = 1 ]; then
  deck "mkdir -p $WT"
  rsync -a -e "$RSYNC_SSH" "$HERE/win_deck/" "$DECK:$WT/"
  echo "scripts -> $DECK:$WT/"
fi
if [ "$setup" = 1 ]; then
  for i in "${insts[@]}"; do deck "$WT/win-setup.sh $i ${WIN_SETUP_ARGS:-}"; done
fi
if [ "$exe" = 1 ]; then
  [ -d "$stage" ] || { echo "no stage dir $stage" >&2; exit 1; }
  first="${insts[0]}"
  free=$(deck "df -Pm $WT | awk 'NR==2{print \$4}'")
  excl=(--exclude ac6recomp.toml --exclude '*.prev' --exclude '*.log' --exclude '*.iso')
  [ "$pdb" = 1 ] || excl+=(--exclude '*.pdb')
  need=$(( $(du -sm "$stage" | cut -f1) + 500 ))
  [ "$free" -gt "$need" ] || { echo "SD card has ${free} MiB free, need ${need}" >&2; exit 1; }
  deck "cd $WT/$first/bin && for f in *.exe; do [ -f \"\$f\" ] && ln -f \"\$f\" \"\$f.prev\"; done; true"
  rsync -a "${excl[@]}" -e "$RSYNC_SSH" "$stage/" "$DECK:$WT/$first/bin/"
  echo "$(basename "$stage") -> $first/bin"
  for i in "${insts[@]:1}"; do
    deck "cd $WT/$i/bin && for f in *.exe; do [ -f \"\$f\" ] && ln -f \"\$f\" \"\$f.prev\"; done
          cd $WT/$first/bin && find . -type f ! -name ac6recomp.toml ! -name '*.prev' ! -name '*.log' ! -name '*.pdb' \
            -exec sh -c 'mkdir -p \"$WT/$i/bin/\$(dirname \"\$1\")\" && ln -f \"\$1\" \"$WT/$i/bin/\$1\"' _ {} \;"
    echo "$first/bin -> $i/bin (hard links)"
  done
  deck "ls -la $WT/$first/bin | grep -i -E '\.exe|\.toml'"
fi
