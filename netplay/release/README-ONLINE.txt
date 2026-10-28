AC6 Recomp Online - Windows x64, Beta 1 (2026-10-27)
====================================================

This is a recompiled PC build of Ace Combat 6: Fires of Liberation that can play Player Match
and Ranked Match online through a community lobby server. Nothing talks to Microsoft or Xbox
Live.

NO GAME FILES ARE INCLUDED (no disc image, XEX or assets). ac6recomp.exe contains code
recompiled from the game's executable, as upstream AC6 Recomp's releases do. You need your own
legally obtained Ace Combat 6 disc image (.iso), US release.


What is in this zip
-------------------

  ac6recomp.exe                   the game (Windows 10/11, 64-bit)
  ac6recomp-online-example.toml   example online settings
  PLAYING_ONLINE.md               the full online guide (settings, firewall, troubleshooting)
  LICENSE.txt                     license of the recomp runtime
  THIRD_PARTY_NOTICES.txt         licenses of the libraries built into the exe (FFmpeg,
                                  libmspack, SDL3 and others) and the LGPL source details
  licenses\                       full license texts
  README-ONLINE.txt               this file

No DLLs and no Visual C++ redistributable are needed: the runtime is built into the exe.


Requirements
------------

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 (Intel Haswell / AMD Zen or newer). Older CPUs crash at start.
- A GPU with current Vulkan drivers.
- Outgoing UDP to the internet (almost every home network allows it).


Setup
-----

1. Unzip into a folder of its own, for example C:\Games\AC6.
2. Put your AC6 disc image (.iso) in the same folder as ac6recomp.exe.
3. First start: rename ac6recomp-online-example.toml to ac6recomp.toml (beside the exe) and
   set your gamertag in it. It already points at the Acenet lobby (play.acenet.gg) with its
   key. If you already have an ac6recomp.toml, close the game and paste the [netplay] section
   from the example at the end of it instead.
4. Use forward slashes in paths. One bad line makes the game ignore the whole file.
5. Start the game. The online menus (Player Match, Ranked Match, leaderboards) now open.


Windows Firewall
----------------

The first start may show "Windows Defender Firewall has blocked some features of this app".
Tick the network type you use (new networks are often "Public") and click Allow access. In
fullscreen the prompt can be hidden behind the game: alt-tab to it. If you cancel, online play
should still work, but more matches go through the server's relay and ping is higher. No port
forwarding is needed.


Your online account
-------------------

Your account is a key file, not a password:

    %LOCALAPPDATA%\ac6recomp\netplay\identity.json

Back it up. If you delete it, the next start creates a new account and your ranked results
stay with the old one. Never share it: it is your account. Saves stay in
Documents\ac6recomp.


Proton / Wine (Linux, Steam Deck)
---------------------------------

On Linux and the Steam Deck the native Linux build runs better; https://acenet.gg says when it
is ready to download. If you run this Windows exe under Proton, set these Steam launch options,
or the game freezes after a few frames or minutes:

    PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1 %command%


Status and known issues
-----------------------

- Tested against the Linux build under Proton on a Steam Deck: lobby, mission start, players
  seeing and shooting each other, kills both ways, matching scoreboards, and a full ranked
  match with matching results. Not yet tested on a real Windows PC, and not yet between
  players on two different home networks.
- AC6 ends a match when the host's game freezes for more than 1 second (another player's for
  more than 2 seconds): "Connection with key player lost" or "Connection with host lost". Two test matches ended this way on
  an overloaded test machine; the network itself delivered every packet. Close heavy background
  programs while you play.
- No voice chat, no host migration (if the host leaves, the match ends), no invites.
- Both players need the same DLC stages and the same frame-rate setting (ac6_unlock_fps).

When you report a problem, include your gamertag, the time, and ac6recomp.log from the game
folder (the game replaces it at every start, so copy it right after the problem), plus any crash
file next to it.
Setup guide and troubleshooting: https://acenet.gg/games/ac6
Acenet rules, privacy and contact: https://acenet.gg
Source code: https://github.com/DogeXH/AC6_recomp/tree/netplay-online
