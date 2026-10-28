# Playing AC6 online

The recomp can play Ace Combat 6 online through a community lobby server (`ac6lobby`). The game
believes it is signed in to Xbox Live: Player Match and Ranked Match, matchmaking, lobbies and
leaderboards go to the lobby server instead. Nothing talks to Microsoft.

- **No port forwarding.** It is designed to work behind home routers, CGNAT and phone hotspots
  (not yet tested between two different home networks). Players
  connect directly when the network allows it, and through the server's relay when it does not.
- **Encrypted.** Traffic to the server and between players is authenticated and encrypted.
- **Your account is a key file.** There is no password. The first connection creates the
  account and stores its secret key on your machine.

## Status (Beta 1, 2026-10-27)

Online play is new. What has been tested:

- **Full matches work** on the Linux / Steam Deck build. Tested: Player Match and Ranked Match,
  the lobby, mission start, both players seeing and shooting each other, kills and scores
  agreeing on both screens, the results screen, stats saved to the server, and a rematch from
  the same lobby. Tested both with a direct connection and with all traffic through the
  server's relay.
- **macOS:** not a supported online platform for now. The code builds there, but Mac online
  play is not being tested.
- **Windows (2026-10-23):** the Windows build (`ac6recomp.exe`, no Visual C++ redistributable
  needed) has played Player and Ranked Matches against the Linux build, tested under Proton on
  the Steam Deck: lobby, mission start, both players seeing and shooting each other, kills both
  ways and matching scoreboards. It has not been run on a real Windows PC yet. Its online
  identity is kept in `%LOCALAPPDATA%\ac6recomp\netplay` (or `<user_data_root>\netplay` when
  `user_data_root` is set). The first launch may show a Windows Firewall prompt for UDP: allow
  it (online should still work if you cancel, with more relayed connections).
- **Windows build under Proton/Wine:** set the launch options
  `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1 %command%`, or the game freezes after
  a few frames or minutes. On Linux and the Steam Deck, use the Linux build instead when you can.
- **Networks:** a full match with 100 ms of added round-trip delay and 2% packet loss, and a full Player
  Match between a Windows and a Linux copy through the public Acenet server. Every test so far
  ran on one site; matches between players on two different home networks have not been tested
  yet.
- **Freezes end matches.** AC6 itself ends a match when the host's game stops for more than
  1 second, or another player's for more than 2 seconds; the host sees "Connection with key
  player lost". Two test matches ended this way, on an overloaded test machine running two
  copies under Proton; the network delivered every packet. Close heavy background programs,
  and if it happens to you, note the time and tell the server operator.

## What you need

- A recomp build with online support.
- The address of a lobby server (`host:port`) and its **public key** (64 hexadecimal
  characters). The server operator publishes both.
- Outgoing UDP must be allowed. Almost every home network allows it.

## Turn it on

Open `ac6recomp.toml` while the game is closed. On Windows and Linux the file is beside the
executable; on macOS it is in the game's per-user data folder. Steam Deck setups that start the
game through a bundled `ld-linux` loader read it from the **loader's** folder instead; the log's
`Loaded config from ...` line shows which file was used. Add a `[netplay]` section:

```toml
[netplay]
enabled = true
server = "lobby.example.net:47800"
server_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
gamertag = "Talisman"
```

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Turns online support on. When it is off, the game runs exactly as it does without it. |
| `server` | `"127.0.0.1:47800"` | The lobby server as `host:port`. A DNS name, IPv4 or `[IPv6]` works. |
| `server_key` | `""` | The server's public key. It must match exactly, or you will never sign in. |
| `gamertag` | `""` | The name others see, up to 15 letters, digits and spaces. If the name is taken, the server adds a number (`Talisman2`). |
| `force_relay` | `false` | Sends all player traffic through the server. Use it only if direct connections misbehave. |
| `privacy` | `false` | Keeps your IP address from other players. All traffic then goes through the relay, which may add latency. |
| `lan` | `true` | Offers your local network addresses, so players on the same network connect directly. |
| `port` | `0` | Local UDP port; `0` picks a free one. Set a fixed port only if your firewall needs it. |

Use forward slashes in any path in the TOML file. One bad line makes the game ignore the whole
file.

## What "signed in" looks like

- The game treats your profile as signed in to Xbox Live. The online menus open (Player Match,
  Ranked Match, leaderboards) and the lobby shows you under your gamertag.
- With `ac6_performance_mode = false` in `ac6recomp.toml` (performance mode, the default,
  keeps only errors in the log), the log shows `overlay: registered as <gamertag> vip
  10.64.x.x ...` and `overlay: state connecting -> online`. If the server never answers, it
  shows `overlay: state connecting -> offline` after 30 seconds; if the server name cannot be
  resolved, `... retrying in 5 s`.
- If the server is unreachable for more than 30 seconds, the game is told that you signed
  out. The connection comes back by itself when the server answers. If the online menus
  stay unavailable after that, go back to the main menu or restart the game.
- Your saves do not move. They stay under your offline profile.

## Your account

- Your identity is `netplay/identity.json` in the game's user data folder. It holds your secret
  key and your server account (XUID, gamertag, virtual address). On Windows it is in
  `%LOCALAPPDATA%\ac6recomp\netplay` instead (see [Windows](#windows)).
- **Back it up.** If you delete it, the next start creates a new account with a new XUID, and
  your ranked results stay with the old one.
- Each server keeps its own accounts. The same key file works on several servers.

## Network and firewall

- Nothing to forward and nothing to open for incoming traffic. The game needs only outgoing
  UDP to the server's port (usually 47800) and to other players.
- Some strict networks (some offices, schools, hotel Wi-Fi) block UDP. Online play cannot
  work there.
- Players connect directly when possible: on the same network, over IPv6, or through NAT hole
  punching. Otherwise they use the relay, so the match still works with slightly more
  latency.

## Windows

The Windows build has been tested only under Proton on the Steam Deck so far (see
[Status](#status-beta-1-2026-10-27)). This section is what differs from Linux.

**Files**

- `ac6recomp.toml` goes **beside `ac6recomp.exe`**. The game does not create it: on a first
  start, rename `ac6recomp-online-example.toml` from the zip to `ac6recomp.toml` and set your
  gamertag. If you already have one, close the game and add the `[netplay]` section from
  [Turn it on](#turn-it-on). Use forward slashes in paths (`C:/Games/AC6/ac6.iso`).
- The online identity is `%LOCALAPPDATA%\ac6recomp\netplay\identity.json` (paste
  `%LOCALAPPDATA%\ac6recomp` into Explorer's address bar). It is kept out of Documents
  because OneDrive often syncs Documents to every PC on your Microsoft account, and
  Controlled Folder Access can block an unknown exe from writing there. Saves stay in
  `Documents\ac6recomp`.
- With `user_data_root` set in the toml, saves and identity both go to that folder
  (`<user_data_root>\netplay`), as on Linux. Use this for a portable install, or to run two
  copies on one PC: without it, both copies share one identity and therefore one account.

**Windows Firewall**

- With `enabled = true` the game opens two UDP sockets when it starts, before the title
  screen: one IPv4 and one IPv6, on the same port. The port is random unless you set `port`.
  There is no TCP listener.
- The first start can show **"Windows Defender Firewall has blocked some features of this
  app"** for `ac6recomp.exe`. Tick the network type you actually use and click **Allow
  access** (needs an administrator). Windows 11 marks a new network **Public** by default; a
  rule for Private only does nothing there.
- In fullscreen the prompt can open behind the game. Alt-tab to it, or start windowed the
  first time.
- If you clicked **Cancel**, online play should still work through the lobby, the relay and
  most direct connections (not yet tested on a Windows PC), but more matches use the relay and ping is higher. To fix it: Windows
  Security > Firewall & network protection > Allow an app through firewall >
  `ac6recomp.exe`. Or, in an administrator PowerShell:

  ```powershell
  New-NetFirewallRule -DisplayName "AC6 Recomp netplay" -Direction Inbound -Program "C:\Games\AC6\ac6recomp.exe" -Protocol UDP -Action Allow -Profile Private,Public
  ```

- Firewall rules belong to the exe's path. Moving or renaming the game folder brings the
  prompt back. Third-party security suites (Norton, Bitdefender, Kaspersky, ESET) have their
  own prompts.
- Port forwarding is optional and helps only on strict NATs: set `port = 47820` (for
  example) and forward UDP 47820 to this PC.
- A fixed `port` can fail with "the port is reserved by Windows" when Hyper-V, WSL or Docker
  reserves its range (`netsh interface ipv4 show excludedportrange protocol=udp`), and
  online play then stays off. `port = 0` avoids this.

**Graphics, runtime and CPU**

- The online build uses **Vulkan** by default (`ac6_graphics_backend = "vulkan"`), which is
  the backend online play was tested with. `"d3d12"` is also built in but has not been tested
  with this build; try it only if your Vulkan driver misbehaves.
- No Visual C++ redistributable is needed: the exe has the runtime built in.
- The CPU must support AVX2 (Intel Haswell / AMD Zen or newer). Older CPUs crash at start.

**Running the Windows build under Proton or Wine**

- Set the launch options `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1 %command%`
  (with plain Wine, run it without esync, fsync or ntsync). Without them the game freezes after
  a few frames or minutes: Wine's fast sync modes do not perform the game's "signal and wait"
  as one step. Real Windows is not affected.
- There is no firewall prompt under Proton.
- On Linux and the Steam Deck, the Linux build is the better choice.

## Privacy

- The server never shows your IP address in the lobby, in search results or on its status
  page.
- To connect players directly, the server gives your addresses to other players' games that
  ask to connect to yours: normally the players who host or join your session. **Those
  players can see your IP address**, as in most peer-to-peer games, and a modified game could
  ask for any online player's address.
- `privacy = true` stops this. The server gives other players no addresses for you, and all
  your traffic goes through the relay. The server operator can still see your address,
  because the server needs it to reach you.
- The operator sees your gamertag, XUID and the games you join. Read the server's own policy
  for how long it keeps logs.

## Known limitations

- **No voice chat** yet.
- **No host migration.** AC6 has none. If the host leaves, the match ends for everyone.
- **No invites or friends list.** Find matches with the game's search.
- **Base-game rules.** The recomp runs the original release, so online play follows that
  version's rules. Fixes from the game's title updates are not applied.
- **Same DLC stages.** The server does not check DLC yet. Both players need the DLC stages a
  match uses, or the match will fail.
- **Same game settings.** Players only see each other's sessions if they run the same game
  build and the same frame-rate setting (`ac6_unlock_fps` on or off). Agree on one setting.
- **Ranked results are not cheat-proof.** The server stores the results that the players'
  games report.

## Troubleshooting

| Symptom | What to check |
|---|---|
| Never signs in | `enabled = true`; `server` has the right host and port; `server_key` is exactly the published key (a wrong key looks like a dead server). Ask the operator whether the server is up. Someone with the server tools can check reachability with `ac6lobby healthcheck -udp host:47800`. |
| Signed in, then signed out | The server stopped answering for 30 s: a network drop, a server restart, a kick or a ban. The connection comes back by itself (unless you are banned); restart the game if the menus stay offline. |
| No sessions found | The host must be online and have created a session of the same mode and type (Ranked or Player). Check that both of you have the same `ac6_unlock_fps` setting and game build. |
| Your gamertag has a number added | The name was taken on that server. Pick another `gamertag`. |
| Lag or rubber-banding | Remove `force_relay` and `privacy` if you set them. A relayed connection adds the trip through the server. |
| Two copies on one computer | Give each copy its own user data folder (so each has its own `identity.json`) and leave `port = 0`. |

When you report a problem to an operator, include your gamertag and the time. Do not post your
`identity.json`: it is your account.
