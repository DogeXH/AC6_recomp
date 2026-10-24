#!/bin/bash
# Stops every test instance under <run>/instances and the dev server, each by its recorded PID.
# Processes that this tooling did not start are never touched.
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

if [ -d "$AC6NP_INSTANCES" ]; then
  for dir in "$AC6NP_INSTANCES"/*/; do
    [ -f "$dir/ac6recomp.pid" ] || continue
    name="$(basename "$dir")"
    stop_pidfile "$dir/ac6recomp.pid" ac6recomp "instance $name" 15
  done
fi
"$AC6NP_TOOLS_DIR/dev_server.sh" stop
