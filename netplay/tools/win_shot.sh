#!/bin/bash
# Harness screenshots of one or more Deck test instances, fetched to the Mac as PNG.
#   win_shot.sh NAME W [D ...]     -> $RUN/instances/<inst>/shots/repl/<inst>_NAME.png each, and
#                                     with 2+ instances also $RUN/instances/pair_NAME.png side by side
#   win_shot.sh --desktop NAME     -> the Deck's whole desktop (spectacle), for crash dialogs or a
#                                     game that never got as far as the harness: $RUN/instances/desk_NAME.png
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="${AC6NP_RUN_ROOT:-/Users/cole/ACE6-Recomp/ac6-online-20261019/run}"
R="$RUN/instances"
if [ "${1:-}" = "--desktop" ]; then
  name="${2:?name}"
  read -r rhost _ < "$R/D/remote"
  ssh -o BatchMode=yes -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/ac6np-%C" -o ControlPersist=20m \
    -o "UserKnownHostsFile=$R/D/known_hosts" "$rhost" \
    'export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
     f=/run/user/1000/ac6np-desk.png; rm -f $f; kscreen-doctor --dpms on >/dev/null 2>&1; timeout 20 spectacle -b -n -f -o $f >/dev/null 2>&1; cat $f; rm -f $f' \
    > "$R/desk_$name.png"
  [ -s "$R/desk_$name.png" ] && echo "$R/desk_$name.png" || { echo "no desktop screenshot" >&2; exit 1; }
  exit 0
fi
[ "$#" -ge 2 ] || { echo "usage: win_shot.sh NAME <inst>... | --desktop NAME" >&2; exit 2; }
name="$1"; shift
pids=(); pngs=()
for i in "$@"; do
  "$HERE/repl_cmd.sh" "$R/$i" --timeout 120 --shot "${i}_$name" -- > /dev/null &
  pids+=($!); pngs+=("$R/$i/shots/repl/${i}_$name.png")
done
rc=0; for p in "${pids[@]}"; do wait "$p" || rc=1; done
for p in "${pngs[@]}"; do [ -s "$p" ] && echo "$p" || { echo "missing $p" >&2; rc=1; }; done
if [ "$#" -ge 2 ] && [ "$rc" = 0 ]; then
  python3 -I - "$R/pair_$name.png" "${pngs[@]}" <<'PY'
import sys
from PIL import Image
out, ims = sys.argv[1], [Image.open(p).convert("RGB") for p in sys.argv[2:]]
w, h = ims[0].width // 2, ims[0].height // 2
img = Image.new("RGB", (len(ims) * (w + 8) - 8, h), (255, 255, 255))
for n, im in enumerate(ims):
    img.paste(im.resize((w, h)), (n * (w + 8), 0))
img.save(out); print(out)
PY
fi
exit $rc
