#!/bin/bash
# Stops one test instance by its recorded PID.
#
#   stop_instance.sh <name>
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ "$#" -eq 1 ] || die "usage: stop_instance.sh <name>"
valid_instance_name "$1" || die "bad instance name '$1'"
dir="$(instance_dir "$1")"
[ -d "$dir" ] || die "no instance $1 in $AC6NP_INSTANCES"
stop_pidfile "$dir/ac6recomp.pid" ac6recomp "instance $1" 15
