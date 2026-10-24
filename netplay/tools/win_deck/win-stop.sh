#!/bin/bash
# Stops only the Windows instance (W default, or W2) started by win-launch.sh. Its launcher closes
# the game's windows, then ends that instance's own Wine prefix (wineserver -k), then the container.
. /run/media/deck/SN256/ACE6-Netplay-Win/win-env.sh "${1:-W}"
[ -f "$I/launcher.pid" ] || { echo "not running"; exit 0; }
p=$(cat "$I/launcher.pid")
if kill -0 "$p" 2>/dev/null && grep -q win-run.py "/proc/$p/cmdline" 2>/dev/null; then
  kill -TERM "$p"
  for _ in $(seq 1 60); do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done
  kill -0 "$p" 2>/dev/null && echo "launcher $p still running" >&2
fi
rm -f "$I/launcher.pid"; echo stopped
