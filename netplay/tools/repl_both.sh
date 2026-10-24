#!/bin/bash
# Sends the same repl batch to instances A and B in parallel and captures a screenshot of each.
#   repl_both.sh <shot-name> -- <line>...
set -euo pipefail
R="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}/instances"
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/repl_cmd.sh"
name="$1"; shift; [ "${1:-}" = "--" ] && shift
"$T" "$R/A" --timeout 300 --shot "A_$name" -- "$@" &
pa=$!
"$T" "$R/B" --timeout 300 --shot "B_$name" -- "$@" &
pb=$!
wait $pa; wait $pb
