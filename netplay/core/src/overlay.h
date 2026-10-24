// ac6net - the overlay transport (PROTOCOL.md section 2): one UDP socket (plus a best-effort
// IPv6 socket on the same port), the encrypted server channel (HELLO, RPC, keepalive,
// introductions), authenticated peers with direct-path probing and relay fallback.
//
// Threading: one network thread owns the sockets and the timers. Every public method is
// thread-safe. Handlers run on the network thread and are never invoked while the overlay holds
// any of its locks, so they may call back into Send() (vnet does). Call() and ResolveVip() block
// and must not be called from the network thread (they fail fast if they are).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ac6net/config.h"
#include "ac6net/netplay.h"
#include "ac6net/types.h"
#include "crypto.h"
#include "link.h"

namespace ac6net {

// What the server told us in HELLO_ACK (or a cached copy of it).
struct OverlayRegistration {
  bool valid = false;
  PeerId peer_id = 0;
  Xuid xuid = 0;
  uint32_t vip = 0;  // host order
  std::string gamertag;
  std::string public_ip;  // server-observed reflexive address ("" if unknown)
  uint16_t public_port = 0;
  std::string motd;
};

struct OverlayCounters {
  uint64_t datagrams_sent = 0, datagrams_recv = 0;
  uint64_t bytes_sent = 0, bytes_recv = 0;
  uint64_t send_errors = 0;
  uint64_t malformed = 0;       // failed a bounds or format check
  uint64_t auth_failures = 0;   // AEAD tag mismatch
  uint64_t replays = 0;         // rejected by a replay window
  uint64_t hello_sent = 0, hello_acks = 0, rehello_recv = 0;
  uint64_t rpc_calls = 0, rpc_retransmits = 0, rpc_timeouts = 0;
  uint64_t intro_reqs = 0, intros_recv = 0, intro_fails = 0;
  uint64_t data_sent_direct = 0, data_sent_relay = 0;
  uint64_t data_recv_direct = 0, data_recv_relay = 0;
  uint64_t data_queued = 0, data_dropped = 0;  // Send() before introduction / dropped
  uint64_t unknown_buffered = 0, unknown_dropped = 0;
  uint64_t unknown_direct = 0;  // direct packets from unknown peers (not buffered; see .cpp)
  uint64_t probes_sent = 0, probe_acks_recv = 0;
  uint64_t path_switches = 0, relay_fallbacks = 0, peers_lost = 0;
  uint64_t peer_table_full = 0;  // INTROs ignored because 256 peers are known
  uint64_t test_dropped = 0;  // injected loss (SetTestLoss)
  // TEST-ONLY impairment from AC6NET_TEST_IMPAIR (overlay_impair.h); all zero when it is off.
  uint64_t impair_delayed = 0;    // outgoing datagrams that went through the delay queue
  uint64_t impair_dropped = 0;    // outgoing datagrams dropped by the configured loss
  uint64_t impair_overflow = 0;   // dropped because the delay queue was full (4096 / 8 MiB)
  uint64_t impair_queue_max = 0;  // delay queue high-water mark (datagrams)
  uint64_t impair_queued = 0;     // datagrams in the delay queue now
};

struct OverlayCandidateInfo {
  std::string addr;  // "ip:port"
  std::string type;  // lan, v6, srflx, prflx
  bool validated = false;
  uint32_t rtt_ms = 0;
};

struct OverlayPeerInfo {
  PeerId peer_id = 0;
  uint32_t vip = 0;
  Xuid xuid = 0;
  std::string gamertag;
  bool direct = false;    // DATA currently goes over a validated direct path
  std::string path;       // active direct address, or "relay"
  std::string path_type;  // candidate type of the active path, or "relay"
  uint32_t rtt_ms = 0;    // RTT of the active direct path (0 when relayed)
  uint64_t idle_ms = 0;   // time since the last authenticated packet from the peer
  std::vector<OverlayCandidateInfo> cands;
};

class Overlay final : public PeerLink {
 public:
  using DataHandler = std::function<void(uint32_t vip, Channel, const uint8_t*, size_t)>;
  using PeerLostHandler = std::function<void(uint32_t vip)>;
  using TickHandler = std::function<void(uint64_t now_ms)>;
  using StateHandler = std::function<void(NetState)>;
  using RegisteredHandler = std::function<void(const OverlayRegistration&)>;

  Overlay();
  ~Overlay() override;
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // Opens the sockets, starts the network thread and begins HELLO. `cached` (optional) is a
  // registration from identity.json for this server; it makes LocalVip() valid immediately.
  // Every Start() uses a fresh boot ID. Returns false on configuration or socket errors.
  bool Start(const Config& config, const crypto::SecretKey& secret_key,
             const OverlayRegistration* cached = nullptr);
  // Sends BYE to every peer and to the server, then stops the thread and closes the sockets.
  // Blocked Call()/ResolveVip() return with transport_ok = false / false.
  void Stop();
  bool running() const;

  NetState state() const;
  // Waits until a HELLO_ACK has been received during this Start() (true), or timeout (false).
  bool WaitOnline(uint32_t timeout_ms);
  OverlayRegistration registration() const;
  uint16_t local_port() const;  // bound UDP port (0 when stopped)
  bool has_ipv6() const;        // the IPv6 socket is open
  crypto::PublicKey public_key() const;

  // Blocking lobby RPC (PROTOCOL.md section 3). Fails fast with err "offline" unless the state
  // is kOnline; err "timeout" after `timeout_ms`.
  RpcResult Call(const std::string& method, const std::string& args_json,
                 uint32_t timeout_ms = 6000);

  // vIP -> peer_id. Non-blocking variant: only already-introduced peers.
  bool PeerIdForVip(uint32_t vip, PeerId* peer_id) const;
  // Blocking variant: asks the server for an introduction by vIP and waits up to `wait_ms`.
  bool ResolveVip(uint32_t vip, uint32_t wait_ms, PeerId* peer_id);

  // Smoothed server RTT in ms (0 if not yet measured).
  uint32_t ServerRttMs() const;
  std::vector<OverlayPeerInfo> Peers() const;
  OverlayCounters counters() const;
  // {"server":{...},"peers":[...],"counters":{...},"max_payload":N}
  std::string StatusJson() const;

  // --- PeerLink ---
  bool Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) override;
  uint32_t LocalVip() const override;
  size_t MaxPayload() const override;  // 1346: RELAY datagram <= 1400 bytes
  uint64_t NowMs() const override;

  // --- Dispatch (network thread; never called with overlay locks held) ---
  void SetDataHandler(DataHandler handler);
  void SetPeerLostHandler(PeerLostHandler handler);
  void SetTickHandler(TickHandler handler);  // every loop iteration (~5 ms)
  void SetStateHandler(StateHandler handler);
  void SetRegisteredHandler(RegisteredHandler handler);  // every HELLO_ACK

  // --- Test hooks ---
  // Also: the environment variable AC6NET_TEST_IMPAIR="delay=75,jitter=20,loss=0.02", read once
  // by Start(), delays and drops every outgoing datagram (TEST ONLY; see overlay_impair.h).
  // Drops the given fraction (0..1) of outgoing / incoming datagrams, chosen at random.
  void SetTestLoss(double send_fraction, double recv_fraction);
  // Drops every datagram sent to or received from a non-server address (direct peer paths),
  // to exercise relay fallback.
  void SetTestBlockDirect(bool block);
  // Drops incoming HELLO_ACK and S2C packets (RELAYED still arrives), e.g. to delay an INTRO.
  void SetTestDropServerControl(bool drop);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace ac6net
