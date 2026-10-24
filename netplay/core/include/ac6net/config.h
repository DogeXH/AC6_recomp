// ac6net - runtime configuration. The SDK glue fills this from netplay_* cvars.
#pragma once

#include <cstdint>
#include <string>

namespace ac6net {

struct Config {
  bool enabled = false;

  // Lobby server "host:port" (IPv4 literal, [IPv6] literal, or DNS name) and its pinned
  // X25519 public key (64 hex chars). Required when enabled.
  std::string server = "127.0.0.1:47800";
  std::string server_key_hex;

  // Requested gamertag (<= 15 chars). The server may suffix it to keep it unique; the final
  // value is reported in Identity::gamertag.
  std::string gamertag;

  // Directory for identity.json (secret key + cached server identity). Created if missing.
  std::string data_dir;

  uint16_t port = 0;         // local UDP port; 0 = random
  bool force_relay = false;  // never use direct peer paths
  bool privacy = false;      // ask the server never to reveal our addresses to peers
  bool lan = true;           // offer LAN/loopback candidates

  std::string trace_path;  // JSONL trace output; empty = off

  // Identification sent to the server.
  std::string title_id = "4E4D07D1";
  std::string compat_key;  // 32 hex chars; see netplay.h ComputeCompatKey()
  std::string client_version = "ac6recomp-netplay/0.1";

  // How long WaitRegistered() may block on first start (no cached identity).
  uint32_t first_connect_wait_ms = 5000;
};

}  // namespace ac6net
