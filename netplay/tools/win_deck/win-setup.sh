#!/bin/bash
# Creates (or refreshes the config of) Windows netplay test instance W or W2 on the SD card:
#   <inst>/bin/ac6recomp.toml   from ac6recomp-win.toml.in (paths on Z:, own user data/logs)
#   <inst>/user-data            cloned once from the Linux test install's user-data, without its
#                               netplay identity (the instance registers its own); never Cole's
#   <inst>/repl_script.txt      "repl <inst>/repl" (the harness polls that queue)
#   <inst>/{pfx,cache,state,shots,repl,history}
# The lobby address and key come from the Linux test instance D's config.
#   win-setup.sh W|W2 [--backend vulkan|d3d12] [--force-relay]
set -euo pipefail
. /run/media/deck/SN256/ACE6-Netplay-Win/win-env.sh "${1:-W}"
[ $# -gt 0 ] && shift
backend=vulkan; relay=false
while [ $# -gt 0 ]; do
  case "$1" in
    --backend) backend="$2"; shift ;;
    --force-relay) relay=true ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
T=/home/deck/Games/ACE6-Netplay-Test
DTOML=$T/ac6recomp.toml
server=$(sed -n 's/^server = "\(.*\)"/\1/p' "$DTOML")
key=$(sed -n 's/^server_key = "\(.*\)"/\1/p' "$DTOML")
[ -n "$server" ] && [ -n "$key" ] || { echo "no server/server_key in $DTOML" >&2; exit 1; }
mkdir -p "$I/bin" "$I/pfx" "$I/cache" "$I/state" "$I/shots" "$I/repl" "$I/history"
# Shared read-only DLC copy (from the Linux test config's dlc_dir; copied once, never written).
if [ ! -d "$WT/dlc-raw-mac" ]; then
  src=$(sed -n 's/^dlc_dir = "\(.*\)"/\1/p' "$DTOML")
  cp -a "$src" "$WT/dlc-raw-mac.tmp" && mv "$WT/dlc-raw-mac.tmp" "$WT/dlc-raw-mac"
  chmod -R a-w "$WT/dlc-raw-mac"
fi
if [ ! -d "$I/user-data" ]; then
  cp -a "$T/user-data" "$I/user-data.tmp"
  rm -f "$I/user-data.tmp/netplay/identity.json"
  mv "$I/user-data.tmp" "$I/user-data"
fi
sed -e "s|@INST@|$INST|g" -e "s|@I@|$I|g" -e "s|@WT@|$WT|g" -e "s|@WX@|$WX|g" -e "s|@WY@|$WY|g" \
    -e "s|@TAG@|$TAG|g" -e "s|@BACKEND@|$backend|g" -e "s|@SERVER@|$server|g" -e "s|@KEY@|$key|g" \
    -e "s|^force_relay = .*|force_relay = $relay|" \
    "$WT/ac6recomp-win.toml.in" > "$I/bin/ac6recomp.toml.tmp"
# Guard: every path must live in this instance, the shared DLC copy or the read-only ISO.
bad=$(grep -n '"[A-Za-z]:/\|"/' "$I/bin/ac6recomp.toml.tmp" \
      | grep -v "\"Z:$I/\|\"Z:$I\"\|\"Z:$WT/dlc-raw-mac\"\|\"Z:/home/deck/Games/ACE6-Recomp/[^/]*\.iso\"" || true)
grep -q '@[A-Z]*@' "$I/bin/ac6recomp.toml.tmp" && bad="$bad unfilled placeholder"
if [ -n "$bad" ]; then echo "refusing config, foreign paths: $bad" >&2; rm -f "$I/bin/ac6recomp.toml.tmp"; exit 1; fi
mv "$I/bin/ac6recomp.toml.tmp" "$I/bin/ac6recomp.toml"
echo "repl Z:$I/repl" > "$I/repl_script.txt"
echo "$INST ready: $I (gamertag $TAG, backend $backend, force_relay $relay, $PROTON_NAME)"
