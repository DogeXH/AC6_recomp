#!/bin/bash
# Starts Windows netplay test instance W (default) or W2: bin/ac6recomp.exe under Proton, the way
# Steam runs it, in the Deck's desktop session (works over SSH). Mirrors np-launch.sh: rotates the
# previous run's trace/log into <inst>/history, clears the repl queue, records launcher.pid.
#   win-launch.sh [W|W2] [game args...]
#   AC6NP_WIN_EXE=win_probe.exe win-launch.sh W <args>   runs another exe from bin/ instead
#   AC6NP_WIN_CONTAINER=0   run proton directly instead of inside the Steam Linux Runtime
#   AC6NP_DUAL=1            two instances at once: no GPU pin from this launcher
set -euo pipefail
. /run/media/deck/SN256/ACE6-Netplay-Win/win-env.sh "${1:-W}"
[ $# -gt 0 ] && shift
[ -d "$I/bin" ] || { echo "no instance $I (run win-setup.sh $INST)" >&2; exit 1; }
[ -x "$PROTON/proton" ] || { echo "no Proton at $PROTON" >&2; exit 1; }
exe="${AC6NP_WIN_EXE:-ac6recomp.exe}"
[ -f "$I/bin/$exe" ] || { echo "no $I/bin/$exe (deploy it with win_deploy.sh)" >&2; exit 1; }
mkdir -p "$I/history" "$I/repl" "$I/shots" "$I/pfx" "$I/cache"
if [ -f "$I/launcher.pid" ] && kill -0 "$(cat "$I/launcher.pid")" 2>/dev/null; then
  echo "already running: launcher $(cat "$I/launcher.pid")" >&2; exit 1
fi
stamp=$(date +%Y%m%d-%H%M%S)
for f in trace.jsonl ac6recomp.log console.log repl_script.txt.progress probe_result.txt; do
  [ -s "$I/$f" ] && mv -f "$I/$f" "$I/history/${f%.*}-$stamp.${f##*.}"
done
rm -rf "$I/repl"; mkdir -p "$I/repl"
# Old history beyond the newest 40 files goes (the SD card is shared with Steam).
ls -t "$I/history" 2>/dev/null | tail -n +41 | sed "s|^|$I/history/|" | xargs -r rm -f
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:0 WAYLAND_DISPLAY=wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
xa=$(ls -t /run/user/1000/xauth_* 2>/dev/null | head -1 || true); [ -n "$xa" ] && export XAUTHORITY=$xa
export AC6NP_WIN_EXE="$exe"
cd "$I/bin"
# SteamOS logind has KillUserProcesses=yes: everything in an SSH login's session scope is killed
# when that SSH connection closes. Run the launcher in its own transient user scope instead, so
# the instance outlives the SSH call that started it (systemd-run --scope execs it: same PID).
scope=()
command -v systemd-run >/dev/null && scope=(systemd-run --user --scope --quiet --collect
  "--unit=ac6np-win-$INST-$stamp")
"${scope[@]}" setsid nohup /usr/bin/python3 "$WT/win-run.py" "$INST" "$@" < /dev/null > "$I/console.log" 2>&1 &
echo $! > "$I/launcher.pid"
sleep 3
kill -0 "$(cat "$I/launcher.pid")" 2>/dev/null || { tail -20 "$I/console.log"; echo "exited during startup" >&2; exit 1; }
echo "started $INST launcher $(cat "$I/launcher.pid") ($PROTON_NAME, $exe)"
