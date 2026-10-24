#!/bin/bash
# Sends one batch of script lines to a running instance whose input script is 'repl <dir>/repl'.
#
#   repl_cmd.sh <instance-dir> [--timeout SEC] [--shot NAME] -- <line> [<line> ...]
#
# Writes <dir>/repl/cmd_NNNN.txt atomically (the next free N), waits for done_NNNN.txt and
# prints its contents. --shot NAME appends a screenshot to <dir>/shots/repl/NAME.ppm and also
# converts it to NAME.png (sips) and prints the PNG path.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: repl_cmd.sh <instance-dir> [--timeout SEC] [--shot NAME] -- <line>..." >&2; exit 2; }
dir="$1"; shift
timeout=120
shot=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --timeout) timeout="$2"; shift 2 ;;
    --shot) shot="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
# Remote instance (for example the Steam Deck): <dir>/remote holds "<user@host> <remote-dir>".
# Commands, done files and screenshots then go over one persistent SSH connection; <dir> is a
# local mirror that keeps the command counter and the converted screenshots.
if [ -f "$dir/remote" ]; then
  read -r rhost rdir < "$dir/remote"
  ssh_opts=(-o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C"
            -o ControlPersist=20m)
  [ -f "$dir/known_hosts" ] && ssh_opts+=(-o "UserKnownHostsFile=$dir/known_hosts")
  mkdir -p "$dir/repl"
  counter="$dir/repl/.next"
  n=1
  [ -f "$counter" ] && n="$(cat "$counter")"
  name="$(printf 'cmd_%04d.txt' "$n")"
  rdone="$rdir/repl/$(printf 'done_%04d.txt' "$n")"
  tmp="$dir/repl/.cmd.tmp"
  : > "$tmp"
  for line in "$@"; do printf '%s\n' "$line" >> "$tmp"; done
  [ -z "$shot" ] || printf 'screenshot %s\n' "$rdir/shots/repl/$shot.ppm" >> "$tmp"
  ssh "${ssh_opts[@]}" "$rhost" "mkdir -p '$rdir/repl' '$rdir/shots/repl' && cat > '$rdir/repl/.cmd.tmp' && mv '$rdir/repl/.cmd.tmp' '$rdir/repl/$name'" < "$tmp"
  echo $((n + 1)) > "$counter"
  if ! ssh "${ssh_opts[@]}" "$rhost" "for i in \$(seq 1 $((timeout * 20))); do [ -f '$rdone' ] && { cat '$rdone'; exit 0; }; sleep 0.05; done; exit 1"; then
    echo "repl_cmd.sh: timed out waiting for $rhost:$rdone" >&2
    exit 1
  fi
  if [ -n "$shot" ]; then
    mkdir -p "$dir/shots/repl"
    rppm="$rdir/shots/repl/$shot.ppm"
    ppm="$dir/shots/repl/$shot.ppm"
    ssh "${ssh_opts[@]}" "$rhost" "for i in \$(seq 1 50); do [ -f '$rppm' ] && break; sleep 0.1; done; gzip -1 -c '$rppm'" | gunzip > "$ppm" 2>/dev/null || true
    if [ -s "$ppm" ]; then
      sips -s format png "$ppm" --out "$dir/shots/repl/$shot.png" > /dev/null 2>&1 && echo "$dir/shots/repl/$shot.png"
    else
      echo "repl_cmd.sh: no screenshot written (see the remote instance log)" >&2
    fi
  fi
  exit 0
fi

repl="$dir/repl"
[ -d "$repl" ] || { echo "no repl dir $repl" >&2; exit 1; }
counter="$repl/.next"
n=1
[ -f "$counter" ] && n="$(cat "$counter")"
name="$(printf 'cmd_%04d.txt' "$n")"
done_file="$repl/$(printf 'done_%04d.txt' "$n")"
tmp="$repl/.cmd.tmp"
: > "$tmp"
for line in "$@"; do printf '%s\n' "$line" >> "$tmp"; done
if [ -n "$shot" ]; then
  mkdir -p "$dir/shots/repl"
  printf 'screenshot %s\n' "$dir/shots/repl/$shot.ppm" >> "$tmp"
fi
mv "$tmp" "$repl/$name"
echo $((n + 1)) > "$counter"

deadline=$(( $(date +%s) + timeout ))
while [ ! -f "$done_file" ]; do
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "repl_cmd.sh: timed out waiting for $done_file" >&2
    exit 1
  fi
  sleep 0.1
done
cat "$done_file"
if [ -n "$shot" ]; then
  ppm="$dir/shots/repl/$shot.ppm"
  for _ in $(seq 1 50); do [ -f "$ppm" ] && break; sleep 0.1; done
  if [ -f "$ppm" ]; then
    sips -s format png "$ppm" --out "$dir/shots/repl/$shot.png" > /dev/null 2>&1 && echo "$dir/shots/repl/$shot.png"
  else
    echo "repl_cmd.sh: no screenshot written (see the instance log)" >&2
  fi
fi
