# Hosting an AC6 lobby server

`ac6lobby` is the server that recomp players sign in to for online play. It handles accounts,
matchmaking, lobbies, leaderboards, connection set-up between players, and a relay for players
who cannot connect directly. It is one small Go program with no database. It needs one UDP
port.

Players need two things from you: the server address (`host:port`) and its public key. See
[Publishing the key](#the-server-key).

## What you need

- A Linux VPS or any always-on machine with a public IPv4 address. IPv6 is optional.
- **Size:** the smallest plan of most providers is enough (1 vCPU, 512 MB to 1 GB of RAM),
  typically US$4 to 6 a month. The server uses about 20 MB of memory.
- **Traffic:** sign-in and matchmaking traffic is tiny. The relay is what uses bandwidth, and
  only for players who cannot connect directly. One relayed pair of players uses about
  150 kbit/s each way (about 70 MB an hour in and the same out). A 16-player match where
  everyone is relayed uses about 18 Mbit/s each way. Check your plan's monthly transfer.
- One UDP port open to the Internet, 47800 by default.

These figures were measured with the [load test](#load-testing). The test ran in a Linux
container limited to 4 CPUs, on a Mac that other work kept very busy, so they are
conservative.

| Load | Relay traffic | Server CPU | Memory | Loss, default Linux buffers | Loss, 4 MiB buffers |
|---|---|---|---|---|---|
| 64 players in 32 relayed pairs, 20-60 packets/s each way | 2,400 packets/s, 5 Mbit/s | 19% of one core | 14-16 MB | 0.6% | 0 |
| 64 players in four 16-player matches, everyone relayed | 29,000-32,000 packets/s, 61-67 Mbit/s | 41-43% of one core | 15-16 MB | 29% | 0 at the server |

All the losses with default buffers happened in the server's receive buffer (the kernel's
`RcvbufErrors` counter). With the 4 MiB buffer limit, the server forwarded every packet.
In the second row, 0.17% was lost after the server, at the overloaded load generator. Raise the
limit on any server that relays traffic (see [systemd](#c-systemd-on-a-plain-vps)).

## Ports

| Port | Protocol | Open to the Internet? | Purpose |
|---|---|---|---|
| 47800 | UDP | **yes** | The game protocol: sign-in, matchmaking, introductions and the relay |
| 47801 | TCP | optional | Status page with counts only (`/`, `/status`, `/status/sessions`). Loopback by default. |
| 47802 | TCP | **never** | Admin interface. It accepts loopback addresses only. |

Players need no open ports.

## Install

Choose one of these three methods.

### A. Docker Compose

From a checkout of the repository:

```sh
cd netplay/deploy
docker compose up -d --build
docker compose logs ac6lobby | grep "server public key"
```

The state is in the named volume `ac6lobby-data`. The container runs as a non-root user on a
read-only root filesystem, limited to one CPU and 256 MB. To make the status page public,
follow the comments in `docker-compose.yml`.

### B. Docker without Compose

```sh
docker build -f netplay/deploy/Dockerfile -t ac6lobby netplay/server
docker run -d --name ac6lobby --restart unless-stopped \
  -p 47800:47800/udp -v ac6lobby-data:/data ac6lobby
docker logs ac6lobby | grep "server public key"
```

The image is about 5 MB (compressed). Extra arguments replace the default `-http 127.0.0.1:47801`.
For example, `... ac6lobby -http :47801 -motd Welcome` together with `-p 47801:47801` makes the
status page public. You can build for another architecture from any machine:
`docker build --platform linux/amd64 ...`.

### C. systemd on a plain VPS

Build a static binary (on any machine with Go 1.26; the dependencies are vendored):

```sh
cd netplay/server
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -o ac6lobby .   # arm64 VPS: GOARCH=arm64
```

Copy `ac6lobby` and the `netplay/deploy` directory to the server. Then run:

```sh
sudo ./install.sh --binary ./ac6lobby --dry-run   # shows exactly what it will do
sudo ./install.sh --binary ./ac6lobby
```

The script shows its plan and asks before it changes anything. It installs
`/usr/local/bin/ac6lobby`, `/etc/systemd/system/ac6lobby.service` and `/etc/default/ac6lobby`,
then enables and starts the service.

It never overwrites your data or your edited settings:

- an older binary is kept as `ac6lobby.bak.<time>`;
- a changed unit file is left in place, and the new one is written beside it as `.new`.

It does not touch firewall rules, and changes a kernel setting only if you pass `--tune-udp`.
It prints the firewall commands for you to run, for example `sudo ufw allow 47800/udp`. Also
open the port in your provider's cloud firewall.

Settings are in `/etc/default/ac6lobby`. State is in `/var/lib/ac6lobby`, which systemd keeps
in `/var/lib/private/ac6lobby` because the unit uses `DynamicUser`. Logs: `journalctl -u ac6lobby`.

**UDP buffers.** Linux limits UDP socket buffers to 208 KiB by default, which is too small
for relay bursts (see the table above). The server warns about it at start-up. To raise the
limit to 4 MiB, use one of these:

- `sudo ./install.sh --binary ./ac6lobby --tune-udp` writes `/etc/sysctl.d/60-ac6lobby.conf`
  and applies it. The script announces this system-wide change in its plan, and it never
  makes it without the flag.
- Or by hand:

  ```sh
  sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=4194304
  ```

  Make the setting permanent in `/etc/sysctl.d/`.

Restart the server afterwards. The limit is a host setting, so for Docker, set it on the
Docker host.

### Any other way

`./ac6lobby -data /path/to/state` runs the server in the foreground. Run `./ac6lobby -h` for the
flags.

| Flag | Default | Meaning |
|---|---|---|
| `-udp` | `:47800` | game protocol address |
| `-http` | `127.0.0.1:47801` | status page; `:47801` makes it public; empty turns it off |
| `-admin` | off | admin interface, loopback only (the deploy files use `127.0.0.1:47802`) |
| `-admin-token-file` | `<data>/admin.token` | admin token, created on first start |
| `-data` | `data` | state directory |
| `-motd` | empty | message of the day sent at sign-in |
| `-log-level`, `-log-format` | `info`, `text` | `json` suits log collectors |
| `-stats-interval` | `1m` | how often the `stats` log line is written |
| `-log-ips` | off | include IP addresses in rate-limit warnings |
| `-hello-per-ip`, `-hello-burst`, `-accounts-per-ip-hour` | 10, 20, 20 | per-IP limits on sign-ins and new accounts |
| `-audit-days` | `90` | how long the admin audit log keeps entries; 0 = no age limit |

## The server key

The first start creates `server.key` (secret, mode 0600) and `server.pub` in the data directory.
Players pin the public key: their game refuses any server that does not hold the matching
secret key. Nobody can impersonate your server, even if they take over its DNS name.

**Print the public key:**

```sh
ac6lobby key show -data /path/to/state                  # plain install
sudo ac6lobby key show                                  # systemd install
docker exec ac6lobby /ac6lobby key show                 # Docker
```

**Publish it** together with the address, somewhere players trust: your website over HTTPS, or a
pinned message in your community channel. Give players a ready-made block to paste:

```toml
[netplay]
enabled = true
server = "lobby.example.net:47800"
server_key = "<the 64 hex characters>"
gamertag = "YourName"
```

**Keep `server.key` secret and backed up.** Anyone with the file can run a server that your
players' games accept. If you lose it, every player has to change their `server_key`.

**Rotating the key.** Rotate only if the secret key may have leaked, or if you move to a new
key on purpose. Rotation changes the pin. A grace period keeps the old key working while
players update their `server_key`:

1. Stop the server.
2. Run `ac6lobby key rotate -data <dir>`. It prints the new public key. The old secret key is
   kept twice: as a backup, `server.key.<time>.old`, and as `server.prev.key`, which the
   server keeps accepting.
3. Start the server, and publish the new key. The log confirms `also accepting the previous
   server key`.
4. Watch `online_old_key` in `ac6lobby admin metrics`, or the `OLD_KEY` column of
   `admin players`. When few players remain on the old key, run `ac6lobby key retire` and
   restart. Players still on the old key then cannot sign in until they update.

If the old key leaked, skip the grace period: `ac6lobby key rotate -no-grace`. Whoever holds
a leaked key can impersonate the server for as long as clients accept it.

Accounts, gamertags and leaderboards survive a rotation, because accounts belong to the
players' keys and not to the server key. To undo a rotation, stop the server, copy the `.old`
file back to `server.key`, delete `server.pub` and `server.prev.key`, and start the server
again.

## Backups

All state is in the data directory:

| File | Contents | If you lose it |
|---|---|---|
| `server.key` | the server's secret key | every player must enter a new `server_key` |
| `server.prev.key` | the previous secret key, during a rotation grace period | players on the old key must update now |
| `accounts.json` | player key → peer ID, XUID, virtual IP, gamertag | everyone gets a new XUID, and their leaderboard entries are orphaned |
| `stats.json` | leaderboards | the rankings start again |
| `bans.json` | bans | banned players can return |
| `admin.token` | admin token | nothing; a new one is created |
| `audit.jsonl` | the admin audit log | the record of past kicks, bans and deletions |
| `server.pub` | public key | nothing; it is rewritten at start |

Every file is replaced atomically (written to a temporary file, synced, then renamed), so copying
the directory while the server runs gives a usable backup. Sessions and online players are
kept in memory only.

```sh
# systemd install
sudo tar czf ac6lobby-$(date +%F).tgz -C /var/lib/private/ac6lobby .
# Docker volume (the image has no shell, so use a helper container)
docker run --rm -v ac6lobby-data:/data:ro -v "$PWD":/backup alpine \
  tar czf /backup/ac6lobby-$(date +%F).tgz -C /data .
```

Keep backups private: they contain the server's secret key. To restore one, stop the server,
unpack the archive into the data directory (owned by the server's user, mode 0700), and start
the server.

## Admin commands

The admin interface needs the token in `<data>/admin.token`. `ac6lobby admin` finds the token
in `/var/lib/ac6lobby`, `/data` or `./data`, or uses `-data DIR` or `-token-file FILE`.

```sh
sudo ac6lobby admin players                  # who is online (add -addr to show IP addresses)
sudo ac6lobby admin sessions                 # host, mode, ranked or standard, state, members
sudo ac6lobby admin kick Pixy "spamming"     # disconnect; they may reconnect
sudo ac6lobby admin ban -for 7d Pixy "griefing"  # temporary ban; without -for it is permanent
sudo ac6lobby admin unban Pixy
sudo ac6lobby admin bans
sudo ac6lobby admin broadcast "Restarting in 5 minutes"
sudo ac6lobby admin forget Pixy              # delete the account and its leaderboard rows
sudo ac6lobby admin metrics                  # counters, CPU and memory, as JSON
sudo ac6lobby admin audit                    # the last 50 admin actions (-n 200 for more)
```

For Docker, replace `sudo ac6lobby` with `docker exec ac6lobby /ac6lobby`.

- **Naming a player.** `<who>` is a gamertag or a 16-hex-digit peer ID. Use `tag:<name>` for a
  gamertag that looks like a peer ID.
- **Bans.** A ban applies to the player's account key. It survives restarts and disconnects
  the player at once.
- **Broadcast.** Players' games log the message, but do not show it.
- **Audit log.** Every kick, ban, unban, broadcast, delete and IP-address lookup is written to
  `<data>/audit.jsonl`, whether it came from these commands, the panel or the HTTP API. See
  OPERATIONS.md, "The audit log".
- **Scripts.** Add `-json` to any command for machine-readable output.
- **HTTP API.** It is the same as the commands: `GET /admin/v1/players|sessions|bans|metrics|audit`
  and `POST /admin/v1/kick|ban|unban|broadcast|forget` with `Authorization: Bearer <token>`.

### Moderation panel

The admin interface also serves a moderation panel at `/admin/ui/`. It shows online players,
sessions, bans, the audit log and metrics, and has buttons for kick, ban, unban, broadcast and
delete. It runs on the same loopback-only listener as the API, so it opens no port to the
internet. Reach it from your own computer through an SSH tunnel:

```sh
ssh -N -L 47802:127.0.0.1:47802 you@your-server
```

Leave that running, open <http://127.0.0.1:47802/admin/ui/> in your browser and paste the
token. To print the token:

```sh
ssh you@your-server sudo cat /var/lib/ac6lobby/admin.token
```

- **Another local port.** If 47802 is taken on your computer, use
  `-L 9000:127.0.0.1:47802` and open <http://localhost:9000/admin/ui/>.
- **The token.** The panel keeps it in that browser tab and forgets it when the tab closes,
  unless you tick "Remember in this browser". **Lock** removes it. To revoke the token, delete
  `admin.token` and restart the server; it creates a new one.
- **Other websites cannot use the tunnel.** The server answers only requests addressed to
  `127.0.0.1`, `[::1]` or `localhost`, and refuses cross-origin POSTs from browsers. A page on
  another site that is open in the same browser therefore cannot act through your tunnel. Open
  the panel by one of those addresses, not by a hosts-file name.
- **Safeguards.** A ban needs a reason and a length: 24 hours, 7 days, 30 days or permanent.
  Delete asks you to type the gamertag first. IP addresses are shown one player at a time, and
  only when you click **Show**. Each lookup goes into the audit log.
- **Docker.** The admin interface listens on the container's own loopback, which a tunnel to the
  host cannot reach. Use the commands with `docker exec`, or run the container with host
  networking (`--network host`, or `network_mode: host` in Compose). The container then shares
  the host's loopback and the tunnel works as above.

## Upgrades and restarts

Sessions live in memory, so a restart ends every lobby and every match that uses the relay.
Players' games sign in again by themselves within a few seconds.

To upgrade:

1. Restart when `admin sessions` shows nothing important, or announce it first with
   `admin broadcast`.
2. Install the new binary: run `install.sh` again, or `docker compose up -d --build`.

On SIGTERM the server sends every client a shutdown notice, writes its files and exits within
a second.

## Load testing

`netplay/server/cmd/loadtest` simulates players. They register, host, search, join, get
introduced and send relay traffic at AC6-like rates (20-60 packets/s each way, 100-300 bytes).

```sh
cd netplay/server && go build -o loadtest ./cmd/loadtest
# against a test server started with relaxed per-IP limits (all simulated players share one IP):
./ac6lobby -data /tmp/lt -udp 127.0.0.1:47900 -admin 127.0.0.1:47902 \
  -hello-per-ip 1000 -hello-burst 1000 -accounts-per-ip-hour 100000
./loadtest -server 127.0.0.1:47900 -key-file /tmp/lt/server.pub \
  -admin http://127.0.0.1:47902 -admin-token-file /tmp/lt/admin.token        # 64 players in pairs
./loadtest ... -clients 64 -group 16                                            # four full matches
```

`-clients 2 -duration 3s` against your public server checks a fresh deployment from end to
end. It exits with status 1 on any failure. Add `-identities ids.json` to reuse the same test
accounts on every run.

## More

- [OPERATIONS.md](OPERATIONS.md): monitoring, handling abuse, logs and privacy.
- [PLAYING_ONLINE.md](PLAYING_ONLINE.md): the player guide to link to.
- [PROTOCOL.md](PROTOCOL.md): the wire protocol.
