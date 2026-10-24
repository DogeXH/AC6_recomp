# Sourced by win-launch.sh / win-stop.sh / win-setup.sh: maps a Windows test instance name
# (W, W2) to its files, and picks the Proton build and the Steam Linux Runtime it needs.
# Source of truth: netplay/tools/win_deck/ in the netplay/windows worktree (win_deploy.sh --scripts).
WT=/run/media/deck/SN256/ACE6-Netplay-Win
INST="${1:-W}"
case "$INST" in
  W)  I=$WT/W;  WX=640; WY=60;  TAG=Talisman ;;
  W2) I=$WT/W2; WX=640; WY=440; TAG=Strigon ;;
  *) echo "unknown instance '$INST' (W or W2)" >&2; exit 2 ;;
esac
STEAM=/home/deck/.local/share/Steam
# Proton 11.0 (pinned stable, 11.0-2e, ntsync): a fixed build that does not update under us
# mid-test like Experimental. Proton 10.0 (fsync) was the first choice but the 19:00 exe stalls
# after its first frame there every time (vulkan, vulkan without nr2, d3d12); 11.0 and
# Experimental run (2026-10-23, see TESTING_WINDOWS.md). AC6NP_PROTON="Proton 10.0" to compare.
PROTON_NAME="${AC6NP_PROTON:-Proton 11.0}"
PROTON="$STEAM/steamapps/common/$PROTON_NAME"
# The container Steam runs that Proton in (toolmanifest.vdf require_tool_appid).
case "$(sed -n 's/.*"require_tool_appid" *"\([0-9]*\)".*/\1/p' "$PROTON/toolmanifest.vdf" 2>/dev/null)" in
  1628350) SLR="$STEAM/steamapps/common/SteamLinuxRuntime_sniper" ;;
  4183110) SLR="$STEAM/steamapps/common/SteamLinuxRuntime_4" ;;
  *) SLR="" ;;
esac
# Wine-side sync (2026-10-23 integration): Proton's fast sync paths (ntsync in 11.0, fsync and esync in
# 10.0) do NtSignalAndWaitForSingleObject as "signal, then wait", not atomically like the NT
# kernel (and the Deck tree's POSIX backend, threading_posix.cpp SignalAndWait). AC6's render
# thread relies on that atomicity: a peer woken by the signal pulses the wait object before the
# signaller blocks, the pulse is lost and the game freezes (UI thread keeps painting the last
# frame). Wineserver sync keeps it atomic, so the rig turns both fast paths off by default.
# AC6NP_FAST_SYNC=1 keeps Proton's default (to reproduce the freeze).
if [ "${AC6NP_FAST_SYNC:-0}" != 1 ]; then
  export PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1
fi
export WT I INST WX WY TAG STEAM PROTON PROTON_NAME SLR
