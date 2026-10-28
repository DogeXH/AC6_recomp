AC6 Recomp Online - Linux and Steam Deck (x86-64), Beta 1 (2026-10-27)
=====================================================================

This is a recompiled build of Ace Combat 6: Fires of Liberation that can play Player Match and
Ranked Match online through a community lobby server (Acenet). Nothing talks to Microsoft or
Xbox Live.

NO GAME FILES ARE INCLUDED (no disc image, XEX or assets). ac6recomp contains code recompiled
from the game's executable, as upstream AC6 Recomp's releases do. You need your own legally
obtained Ace Combat 6 disc image (.iso), US release.


What is in this folder
----------------------

  ac6recomp.sh                    start the game with this
  ac6recomp                       the game
  ld-linux-x86-64.so.2, lib/      glibc 2.44, bundled because most systems (SteamOS 3.8
                                  included) have an older one; the game uses it, not yours
  ac6recomp-online-example.toml   example online settings
  PLAYING_ONLINE.md               the full online guide
  LICENSE.txt, THIRD_PARTY_NOTICES.txt, NOTICES-LINUX-RUNTIME.txt, licenses/

Everything else comes from your system: a Vulkan driver (Mesa RADV on the Steam Deck), X11 or
XWayland, GTK 3, and PulseAudio or PipeWire.


Requirements
------------

- 64-bit x86 Linux with a CPU that has AVX2 (Intel Haswell / AMD Zen or newer). The Steam Deck
  qualifies.
- A GPU with a current Vulkan driver.
- Outgoing UDP to the internet (almost every home network allows it).


Setup
-----

1. Unpack the archive into a folder of its own and keep everything in it together:
       tar xzf AC6-Recomp-Online-beta.1-linux-x86_64.tar.gz
2. Put your AC6 disc image (.iso) in the same folder as ac6recomp.sh.
3. First start: rename ac6recomp-online-example.toml to ac6recomp.toml and set your gamertag in
   it. It already points at the Acenet lobby (play.acenet.gg) with its key. If you already have
   an ac6recomp.toml, close the game and paste the [netplay] section from the example at the
   end of it instead.
4. Start ./ac6recomp.sh. Open Online from the main menu: Player Match, Ranked Match and the
   leaderboards.

Steam Deck: in Desktop Mode, unpack the folder (for example to ~/Games/AC6-Online), then in
Steam use "Add a Non-Steam Game", browse to ac6recomp.sh and add it. It then starts from Game
Mode like any other game. Do not add Proton or a compatibility tool to it: this is the native
build.


Your online account
-------------------

Your account is a key file, not a password:

    ~/.local/share/ac6recomp/netplay/identity.json

Back it up. If you delete it, the next start creates a new account and your ranked results
stay with the old one. Never share it: it is your account. Saves are in
~/.local/share/ac6recomp as well.


Status and known issues
-----------------------

- Online play was developed on this Linux build: full Player and Ranked Matches against other
  Linux copies and against the Windows build, direct and through the relay. Every test so far
  ran on one site; matches between players on two different home networks have not been
  tested yet.
- AC6 ends a match when the host's game freezes for more than 1 second (another player's for
  more than 2 seconds): "Connection with key player lost" or "Connection with host lost". Close
  heavy background programs while you play.
- No voice chat, no host migration (if the host leaves, the match ends), no invites.
- Both players need the same DLC stages and the same frame-rate setting (ac6_unlock_fps).

When you report a problem, include your gamertag, the time, and ac6recomp.log from this folder
(the game replaces it at every start, so copy it right after the problem), plus any crash file
next to it.
Setup guide and troubleshooting: https://acenet.gg/games/ac6
Acenet rules, privacy and contact: https://acenet.gg (Discord @dogexandedare).
Source code: https://github.com/DogeXH/AC6_recomp/tree/netplay-online
