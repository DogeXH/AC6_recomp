#!/bin/bash
# Starts the dev server and two instances: A ("Talisman") and B ("Avril").
#
#   launch_pair.sh [--force-relay] [--script-a FILE] [--script-b FILE] [--stagger SECONDS]
#                  [--dry-run]
#
# Instances are created on first use (saves cloned) and their config refreshed every time.
# B starts --stagger seconds after A (default 5) so their startup work does not overlap.
# --dry-run still starts the server and prepares the instances, but only prints the game
# commands. Stop everything with stop_all.sh.
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

relay_flag=()
script_a=""
script_b=""
stagger=5
dry_run=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --force-relay) relay_flag=(--force-relay) ;;
    --script-a)
      [ "$#" -ge 2 ] || die "--script-a needs a file"
      script_a="$2"
      shift
      ;;
    --script-b)
      [ "$#" -ge 2 ] || die "--script-b needs a file"
      script_b="$2"
      shift
      ;;
    --stagger)
      [ "$#" -ge 2 ] || die "--stagger needs a number of seconds"
      case "$2" in "" | *[!0-9]*) die "--stagger needs a number of seconds" ;; esac
      stagger="$2"
      shift
      ;;
    --dry-run) dry_run=(--dry-run) ;;
    -h | --help)
      sed -n '2,11p' "$0" >&2
      exit 2
      ;;
    *) die "unknown option '$1'" ;;
  esac
  shift
done

"$AC6NP_TOOLS_DIR/dev_server.sh" start
"$AC6NP_TOOLS_DIR/make_instance.sh" A Talisman ${relay_flag[@]+"${relay_flag[@]}"}
"$AC6NP_TOOLS_DIR/make_instance.sh" B Avril ${relay_flag[@]+"${relay_flag[@]}"}

# Coordination flags from an earlier run would release a waiting script too early.
for flag in "$AC6NP_INSTANCES"/*.flag; do
  [ -f "$flag" ] || continue
  note "removing old coordination flag $flag"
  rm -f "$flag"
done

launch() {
  local name="$1" script="$2" args=()
  [ -z "$script" ] || args=(--input-script "$script")
  "$(instance_dir "$name")/launch.sh" ${args[@]+"${args[@]}"} ${dry_run[@]+"${dry_run[@]}"}
}

launch A "$script_a"
if [ "${#dry_run[@]}" -eq 0 ] && [ "$stagger" -gt 0 ]; then
  note "waiting ${stagger}s before starting B"
  sleep "$stagger"
fi
launch B "$script_b"

note "pair started. Traces: $(instance_dir A)/trace.jsonl and $(instance_dir B)/trace.jsonl"
note "stop everything with: $AC6NP_TOOLS_DIR/stop_all.sh"
