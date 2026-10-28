# Running an AC6 lobby server day to day

This guide covers monitoring, handling abuse, and logs and privacy. Installation is in
[HOSTING_A_SERVER.md](HOSTING_A_SERVER.md). The examples use the systemd install. With Docker,
replace `sudo ac6lobby` with `docker exec ac6lobby /ac6lobby`.

## Monitoring

**Is it up?**

- `ac6lobby healthcheck -udp <host>:47800` sends one probe packet that a live server answers
  (a "REHELLO"). It creates no account and no state. It works from any machine and exits
  non-zero when there is no answer, so it suits cron and monitoring agents.
- The Docker image runs the same check as its `HEALTHCHECK`, so `docker ps` shows `healthy`.
- If the status page is public, an HTTP monitor can watch `http://<host>:47801/status`.

**What is happening?** Every minute the server logs one `stats` line (change the interval with
`-stats-interval`):

```
level=INFO msg=stats online=12 sessions=3 accounts=240 bans=1 hello_per_s=0.2 rpc_per_s=3.1
  intro_per_s=0.1 relay_pkt_per_s=1840 relay_kbit_per_s=3900 relay_drop_per_s=0
  malformed_per_s=0 rehello_per_s=0 banned_refused=0 cpu_s=812.4 rss_mb=21 heap_mb=4 goroutines=13
```

With `-log-format json` this line is easy to graph with Loki, Vector or `jq`. Use
`sudo ac6lobby admin metrics` for the same figures (and the cumulative counters) on demand.

| Signal | Normal | Look into it when |
|---|---|---|
| `relay_drop_per_s` | 0 | It stays above 0: a client is over the relay limit (2,000 packets/s or 4 Mbit/s) or is sending for pairs the server never introduced. |
| `malformed_per_s` | near 0 | Spikes: scanning or junk traffic. It is harmless (dropped before any work), but see the DDoS section below. |
| `rehello_per_s` | near 0 | It stays high after a restart: clients with old state. It should settle within a minute. |
| `banned_refused` | 0 | It is high: a banned player keeps retrying (expected), or a ban caught the wrong account. |
| `cpu_s` | grows slowly | It grows by more than half a second per second (half a core): heavy relay load. See capacity below. |
| `rss_mb` | 15-40 | It keeps growing for days. Report it with the `metrics` output. |

The server also logs a warning at start-up if the kernel's UDP buffers are small. On Linux,
raise `net.core.rmem_max` as described in HOSTING_A_SERVER.md.

The server cannot count packets that the kernel drops before it reads them. To check for
those drops, watch `RcvbufErrors` in `grep Udp: /proc/net/snmp` (or "receive buffer errors"
in `netstat -su`). If it keeps growing while players are relayed, the buffers are too small
or the machine is too slow.

**Capacity.** One core relays more than 30,000 packets a second; a busy evening of AC6 is
far below that. Bandwidth usually runs out first (see the traffic figures in
HOSTING_A_SERVER.md). Before you advertise a server widely, run `cmd/loadtest` against a
test copy of it.

**Public status page.** `/` (HTML), `/status` and `/status/sessions` show counts only:

- players online and the number of sessions;
- lobby or in battle;
- sessions per mode (Siege, Battle Royale, Team, Co-op and their DLC variants), split into
  ranked and standard.

They never show names, XUIDs, session IDs or addresses.

## Handling abuse

Players report a gamertag. Look it up in the moderation panel (HOSTING_A_SERVER.md,
"Moderation panel"). Its Players and Sessions tabs show the same as:

```sh
sudo ac6lobby admin players            # peer ID, how long online, sessions hosted, relay use
sudo ac6lobby admin sessions           # which session the player is in, with whom
```

Then pick the lightest action that works. The panel has a button for each:

| Action | Command | Effect |
|---|---|---|
| Warn everyone | `admin broadcast "..."` | Logged by every client. It is not shown in the game. |
| Kick | `admin kick <who> [reason]` | Disconnects the player and deletes the sessions they host. They can reconnect at once. |
| Temporary ban | `admin ban -for 24h <who> [reason]` | Disconnects the player and refuses their sign-ins until the time is up. |
| Ban | `admin ban <who> [reason]` | The same, with no end. |
| Undo | `admin unban <who>` | Lifts the ban. |

Things to know:

- **A ban targets an account key.** The ban survives server restarts, and it holds even if
  the player changes their gamertag. A player can make a new key and so a new account, but
  the server allows at most 20 new accounts per IP address per hour (lower it with
  `-accounts-per-ip-hour`). That limit is shared by everyone behind the same CGNAT address.
- **Kicking does not end a match that is already running directly between players.** The
  server is not in that path. The kick ends the player's relay traffic, their hosted sessions
  and their sign-in.
- **IP blocks are a last resort.** `admin players -addr` shows the IP address of each online
  player. Block an address with your firewall only for a persistent attacker, and remove the
  block later. An address can be shared by many innocent players (CGNAT, universities), and
  it is personal data (see below).
- **Write down why.** The audit log keeps every kick, ban, unban, broadcast and delete with its
  reason: run `admin audit`, or open the panel's Audit log tab. Give each ban a reason you could
  explain in an appeal. The panel does not accept a ban without one.

Built-in protections against floods:

- sign-ins: 10 per second per IP and 500 per second in total;
- RPCs: 50 per second per client;
- introductions: 20 per second per client;
- relay: 2,000 packets per second and 4 Mbit/s per client, and only between players the
  server introduced;
- unauthenticated packets get no reply, except the 2-byte REHELLO (rate-limited).

**DDoS.** The server cannot protect a VPS from a volumetric attack. If one happens, use your
provider's DDoS protection, or move the server to a new IP address and update its DNS name.
The players' pinned key stays the same.

## Logs, retention and privacy

IP addresses, gamertags and XUIDs are personal data in many jurisdictions (for example under
the GDPR). The server keeps as little as it needs:

- **Files on disk** (`accounts.json`, `stats.json`, `bans.json`, `audit.jsonl`) contain no IP
  addresses.
  They hold public keys, peer IDs, XUIDs, virtual IPs (10.64.x.x, not real addresses),
  gamertags and leaderboard values.
- **Memory.** A player's real address is kept only while they are online, because the server
  needs it to reach them.
- **Logs at the default `info` level** contain peer IDs, gamertags, session IDs and admin
  actions. They contain no IP addresses.
  - The new-account rate-limit warning omits the address unless you start the server with
    `-log-ips`.
  - `-log-level debug` logs addresses. Use it only briefly, to debug a specific problem, and
    delete those logs afterwards.
- **Admin output.** `admin players -addr` and the panel's **Show** button show addresses on
  request only. Do not paste them in public channels.

**Set a retention period.** The server writes logs to stderr and your system keeps them:

- systemd/journald: set `MaxRetentionSec=14day` (and `SystemMaxUse=200M`) in
  `/etc/systemd/journald.conf`, then `sudo systemctl restart systemd-journald`. This applies
  to the whole machine.
- Docker: the bundled `docker-compose.yml` rotates logs at 10 MB × 3 files. With plain
  `docker run`, add `--log-opt max-size=10m --log-opt max-file=3`.
- Two to four weeks is enough to investigate abuse reports. Keep your private ban notes
  separately.

**Deletion requests.** `sudo ac6lobby admin forget <who>` (or **Delete** in the panel) deletes
the account (key, peer ID, XUID, gamertag) and its leaderboard rows, and disconnects the player.
The server writes the files at once. If the player's game connects again, it gets a new account.
See below for what the audit log keeps.

A ban on that key is kept, so a banned player cannot use a deletion request to return. To
forget the ban too, run `admin unban` first. Log lines from before the deletion still contain
the gamertag until your retention period removes them.

**Tell your players** what you keep and for how long. One paragraph on the page where you
publish the server key is enough. For example: "We keep server logs for 14 days. They
contain gamertags but no IP addresses. Players whose games connect to yours (normally the
players in your session) can see your IP address unless you set `privacy = true`."

### The audit log

`<data>/audit.jsonl` (mode 0600) gets one JSON line for every admin action: kick, ban, unban,
broadcast, forget, and each look at a player's IP address. Read it with `sudo ac6lobby admin audit`,
in the panel's Audit log tab, or with `jq`. A line holds:

- `time` (UTC), `action` and `ok`, plus `error` when the action failed;
- who did it:
  - `via` is `cli`, `panel` or `api`;
  - `operator` is the account that ran the command (`SUDO_USER`, else `USER`), CLI only;
  - `token` is the first 8 hex digits of the SHA-256 of the admin token. It tells entries made
    with an old token apart after you replace it.
- the target's `peer_id` and `gamertag`;
- `reason`, a ban's `duration` and `expires`, a broadcast's `message`, and a `detail` such as
  "was online; disconnected".

Clients report `via` and `operator` themselves, and anyone with the token can set them to
anything. The token is the real identity, so give it only to people you trust to moderate.

It never holds IP addresses. An IP lookup records whose address was looked at, not the address.

**Retention.** Entries older than 90 days are deleted when the server starts and whenever it
writes a new entry. Change this with `-audit-days N`; 0 means no age limit. The file never
keeps more than 5,000 entries, under 2 MB. Ninety days covers ban appeals.
A ban itself stays in `bans.json`, with its reason, for as long as it lasts.

**Deletion requests and the audit log.** `forget` also removes the account's gamertag from the
audit log, both from its own entry and from earlier entries about the player, such as a ban.
Those entries keep the peer ID, which leads nowhere once the account is gone. If you keep the
player's ban, `bans.json` keeps its gamertag as well. Unban first to remove that too.
