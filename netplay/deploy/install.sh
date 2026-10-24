#!/usr/bin/env bash
# Install ac6lobby as a systemd service on a Linux VPS.
#
#   sudo ./install.sh --binary ./ac6lobby            # shows the plan, asks, then installs
#   sudo ./install.sh --binary ./ac6lobby --dry-run  # only shows the plan
#
# What it does (each step is printed first, and nothing happens before you confirm):
#   - copies the binary to /usr/local/bin/ac6lobby (an existing different binary is kept as
#     /usr/local/bin/ac6lobby.bak.<time>);
#   - installs /etc/systemd/system/ac6lobby.service (an existing different unit is left alone;
#     the new one is written next to it as ac6lobby.service.new);
#   - creates /etc/default/ac6lobby with default settings if it does not exist (never replaced);
#   - runs systemctl daemon-reload, and systemctl enable --now ac6lobby unless --no-start.
#
# What it never does: delete or overwrite server data (/var/lib/ac6lobby: key, accounts,
# stats, bans), create users (the unit uses DynamicUser), change firewall rules, or change
# sysctl settings unless you pass --tune-udp. It prints the firewall commands for you to run.
set -euo pipefail

BIN_DST=/usr/local/bin/ac6lobby
UNIT_DST=/etc/systemd/system/ac6lobby.service
DEFAULTS_DST=/etc/default/ac6lobby
DATA_DIR=/var/lib/ac6lobby
SYSCTL_DST=/etc/sysctl.d/60-ac6lobby.conf
UDP_BUF=4194304
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_SRC="$HERE/systemd/ac6lobby.service"
DEFAULTS_SRC="$HERE/systemd/ac6lobby.default"

orig_args=("$@")
binary=""
dry_run=0
assume_yes=0
start=1
tune_udp=0

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  cat <<'EOF'
Options:
  --binary PATH   the ac6lobby binary to install (default: ./ac6lobby, then ../server/ac6lobby)
  --dry-run       print the plan and exit
  --yes           do not ask for confirmation
  --no-start      install the files only; do not enable or start the service
  --tune-udp      also raise the kernel's UDP buffer limits (net.core.rmem_max and wmem_max)
                  to 4 MiB through /etc/sysctl.d/60-ac6lobby.conf. This changes a system-wide
                  setting, so it is off by default; it helps servers that relay a lot.
  -h, --help      this text

Build a Linux binary first, for example on any machine with Go 1.26:
  cd netplay/server && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -o ac6lobby .
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) binary="${2:?--binary needs a path}"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --yes|-y) assume_yes=1; shift ;;
    --no-start) start=0; shift ;;
    --tune-udp) tune_udp=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

# --- checks (read-only) ---
[[ "$(uname -s)" == Linux ]] || die "this script is for Linux with systemd (found $(uname -s))"
[[ -d /run/systemd/system ]] || die "systemd is not running here; run the binary another way (see HOSTING_A_SERVER.md)"
[[ -f "$UNIT_SRC" && -f "$DEFAULTS_SRC" ]] || die "missing $UNIT_SRC or $DEFAULTS_SRC (run the script from its directory in the repo)"

if [[ -z "$binary" ]]; then
  for cand in "$PWD/ac6lobby" "$HERE/ac6lobby" "$HERE/../server/ac6lobby"; do
    if [[ -f "$cand" ]]; then binary="$cand"; break; fi
  done
fi
[[ -n "$binary" && -f "$binary" ]] || die "no binary found; pass --binary PATH (see --help for how to build one)"
[[ -x "$binary" ]] || die "$binary is not executable (chmod +x it)"
bin_version="$("$binary" version 2>/dev/null)" || die "$binary does not run on this machine (wrong OS or architecture?)"

if [[ $dry_run -eq 0 && $EUID -ne 0 ]]; then
  die "run as root (sudo $0 ${orig_args[*]:-}), or use --dry-run to see the plan"
fi

# --- plan ---
plan=()
backup=""
if [[ -e "$BIN_DST" ]]; then
  if cmp -s "$binary" "$BIN_DST"; then
    plan+=("keep $BIN_DST (identical)")
  else
    backup="$BIN_DST.bak.$(date -u +%Y%m%dT%H%M%SZ)"
    plan+=("copy $BIN_DST to $backup, then install $binary as $BIN_DST ($bin_version)")
  fi
else
  plan+=("install $binary as $BIN_DST ($bin_version)")
fi

unit_new=0
if [[ -e "$UNIT_DST" ]]; then
  if cmp -s "$UNIT_SRC" "$UNIT_DST"; then
    plan+=("keep $UNIT_DST (identical)")
  else
    unit_new=1
    plan+=("leave the existing $UNIT_DST unchanged; write the new unit to $UNIT_DST.new for you to compare")
  fi
else
  plan+=("install the systemd unit as $UNIT_DST")
fi

if [[ -e "$DEFAULTS_DST" ]]; then
  plan+=("keep your settings in $DEFAULTS_DST")
else
  plan+=("create $DEFAULTS_DST with the default settings (UDP :47800, status and admin on loopback)")
fi

rmem="$(cat /proc/sys/net/core/rmem_max 2>/dev/null || echo 0)"
wmem="$(cat /proc/sys/net/core/wmem_max 2>/dev/null || echo 0)"
tune_apply=0
if [[ $tune_udp -eq 1 ]]; then
  if [[ -e "$SYSCTL_DST" ]]; then
    plan+=("keep the existing $SYSCTL_DST (not changed)")
  elif [[ "$rmem" -ge $UDP_BUF && "$wmem" -ge $UDP_BUF ]]; then
    plan+=("UDP buffer limits are already at least 4 MiB (rmem_max $rmem, wmem_max $wmem): nothing to tune")
  else
    tune_apply=1
    plan+=("SYSTEM SETTING: write $SYSCTL_DST raising net.core.rmem_max and net.core.wmem_max to $UDP_BUF (now $rmem and $wmem), and apply it with sysctl")
  fi
fi

plan+=("run: systemctl daemon-reload")
if [[ $start -eq 1 ]]; then
  if systemctl is-active --quiet ac6lobby 2>/dev/null; then
    plan+=("run: systemctl enable ac6lobby, then systemctl restart ac6lobby (clients get a shutdown notice and reconnect)")
  else
    plan+=("run: systemctl enable --now ac6lobby (starts at boot from now on)")
  fi
fi

if [[ -e "$DATA_DIR/server.key" || -e "/var/lib/private/ac6lobby/server.key" ]]; then
  data_note="existing server data in $DATA_DIR is kept as it is (same server key, accounts, stats, bans)"
else
  data_note="the first start creates $DATA_DIR with a new server key"
fi

say "ac6lobby install plan:"
for p in "${plan[@]}"; do say "  - $p"; done
say "  - $data_note"
if [[ $tune_apply -eq 1 ]]; then
  say "Not changed: firewall, users, existing data."
else
  say "Not changed: firewall, sysctl, users, existing data."
fi

if [[ $dry_run -eq 1 ]]; then
  say "(dry run: nothing was changed)"
  exit 0
fi
if [[ $assume_yes -eq 0 ]]; then
  read -r -p "Proceed? [y/N] " answer
  [[ "$answer" == y || "$answer" == Y || "$answer" == yes ]] || { say "Nothing changed."; exit 1; }
fi

# --- apply ---
if [[ -n "$backup" ]]; then
  cp -p "$BIN_DST" "$backup"
  say "kept the old binary as $backup"
fi
if ! cmp -s "$binary" "$BIN_DST" 2>/dev/null; then
  install -m 0755 "$binary" "$BIN_DST.tmp.$$"
  mv -f "$BIN_DST.tmp.$$" "$BIN_DST"
  say "installed $BIN_DST"
fi
if [[ $unit_new -eq 1 ]]; then
  install -m 0644 "$UNIT_SRC" "$UNIT_DST.new"
  say "wrote $UNIT_DST.new (your $UNIT_DST is unchanged; diff them and merge by hand)"
elif [[ ! -e "$UNIT_DST" ]]; then
  install -m 0644 "$UNIT_SRC" "$UNIT_DST"
  say "installed $UNIT_DST"
fi
if [[ ! -e "$DEFAULTS_DST" ]]; then
  install -m 0644 "$DEFAULTS_SRC" "$DEFAULTS_DST"
  say "created $DEFAULTS_DST"
fi
if [[ $tune_apply -eq 1 ]]; then
  mkdir -p "$(dirname "$SYSCTL_DST")"
  {
    echo "# Written by netplay/deploy/install.sh --tune-udp for ac6lobby: larger UDP socket buffers"
    echo "# absorb relay bursts. Delete this file and reboot (or sysctl -w) to undo."
    echo "net.core.rmem_max = $UDP_BUF"
    echo "net.core.wmem_max = $UDP_BUF"
  } > "$SYSCTL_DST"
  chmod 0644 "$SYSCTL_DST"
  say "wrote $SYSCTL_DST (previous values: rmem_max $rmem, wmem_max $wmem)"
  if command -v sysctl >/dev/null 2>&1 && sysctl -p "$SYSCTL_DST" >/dev/null 2>&1; then
    say "applied: net.core.rmem_max and net.core.wmem_max are now $UDP_BUF"
    rmem=$UDP_BUF
  else
    say "warning: could not apply $SYSCTL_DST now (it applies at the next boot); try: sudo sysctl -p $SYSCTL_DST"
  fi
fi
systemctl daemon-reload
if [[ $start -eq 1 ]]; then
  if systemctl is-active --quiet ac6lobby; then
    systemctl enable ac6lobby
    systemctl restart ac6lobby
  else
    systemctl enable --now ac6lobby
  fi
  pub=""
  for _ in $(seq 1 20); do
    pub="$(cat "$DATA_DIR/server.pub" 2>/dev/null || true)"
    [[ -n "$pub" ]] && break
    sleep 0.5
  done
  if [[ -n "$pub" ]]; then
    say ""
    say "ac6lobby is running. Server public key (publish it; players put it in server_key):"
    say "  $pub"
  else
    say "the service did not write $DATA_DIR/server.pub yet; check: journalctl -u ac6lobby -n 50"
  fi
fi

# --- advice only ---
say ""
say "Firewall: allow UDP 47800 in. This script did not change any firewall rule. For example:"
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
  say "  sudo ufw allow 47800/udp"
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  say "  sudo firewall-cmd --permanent --add-port=47800/udp && sudo firewall-cmd --reload"
else
  say "  ufw:       sudo ufw allow 47800/udp"
  say "  firewalld: sudo firewall-cmd --permanent --add-port=47800/udp && sudo firewall-cmd --reload"
  say "  also open UDP 47800 in your provider's cloud firewall, if it has one"
fi
if [[ "$rmem" -gt 0 && "$rmem" -lt $UDP_BUF ]]; then
  say "Optional, for relay-heavy servers: the kernel caps UDP buffers at $rmem bytes, so bursts of"
  say "relay traffic can be dropped. This script did not change it. Re-run with --tune-udp, or:"
  say "  sudo sysctl -w net.core.rmem_max=$UDP_BUF net.core.wmem_max=$UDP_BUF   (and add them to /etc/sysctl.d/)"
  say "then sudo systemctl restart ac6lobby."
fi
say ""
say "Next: sudo ac6lobby admin players | sudo ac6lobby key show | journalctl -u ac6lobby -f"
