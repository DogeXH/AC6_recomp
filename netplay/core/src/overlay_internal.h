// ac6net - overlay implementation details shared by overlay*.cpp. Not for use outside the
// overlay.
//
// Locking: Impl::mu guards every mutable field below unless a comment says otherwise. Methods
// named *Locked require mu to be held. Work that must reach a handler is collected into an
// Events object while mu is held and delivered by DispatchEvents() after it is released.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "crypto.h"
#include "overlay.h"
#include "overlay_impair.h"
#include "overlay_net.h"
#include "overlay_wire.h"

namespace ac6net {

using BootId = std::array<uint8_t, 8>;

// Timing and limits (PROTOCOL.md section 2 and the overlay lane brief).
namespace overlay_limits {
inline constexpr uint64_t kLoopMs = 5;
inline constexpr uint64_t kHelloFirstMs = 1000;
inline constexpr uint64_t kHelloMaxMs = 5000;
inline constexpr uint64_t kRehelloMinMs = 1000;
inline constexpr uint64_t kServerKeepaliveMs = 10000;
inline constexpr uint64_t kServerOfflineMs = 30000;
inline constexpr uint64_t kRpcRetransmitMs = 300;
inline constexpr uint64_t kIntroRetryMs = 1000;
inline constexpr uint64_t kIntroGiveUpMs = 5000;
inline constexpr size_t kMaxIntroRequests = 64;
inline constexpr uint64_t kIntroFailBackoffMs = 1000;  // no new request after a failure
inline constexpr size_t kMaxIntroBackoff = 256;
inline constexpr uint64_t kProbeFastMs = 200;
inline constexpr uint64_t kProbeFastPhaseMs = 10000;
inline constexpr uint64_t kProbeSlowMs = 5000;
inline constexpr uint64_t kProbeStopMs = 120000;
inline constexpr uint64_t kPeerKeepaliveMs = 2500;
inline constexpr uint64_t kDirectSilenceMs = 8000;
inline constexpr uint64_t kPeerLostMs = 15000;
inline constexpr size_t kMaxPeers = 256;
inline constexpr size_t kMaxCandidates = 16;
inline constexpr size_t kMaxPrflx = 4;
inline constexpr size_t kUnknownPerSource = 8;
inline constexpr size_t kUnknownTotal = 64;
inline constexpr uint64_t kUnknownMaxAgeMs = 2000;
inline constexpr size_t kPendingPerVip = 32;
inline constexpr size_t kPendingBytesPerVip = 64 * 1024;
inline constexpr uint64_t kPendingMaxAgeMs = 2000;
inline constexpr size_t kMaxPendingVips = 64;
inline constexpr uint32_t kAuthFailRefresh = 4;  // consecutive bad tags before re-intro
inline constexpr uint64_t kRefreshMinMs = 2000;
// A relayed peer silent this long makes us send a server KEEPALIVE early (at most this often), so
// a restarted server (which answers REHELLO) is noticed before the peer-lost timeout.
inline constexpr uint64_t kRelaySilenceCheckMs = 4000;
}  // namespace overlay_limits

enum class CandType : uint8_t { kLan = 0, kV6 = 1, kSrflx = 2, kPrflx = 3 };  // lower = better
const char* CandTypeName(CandType type);

struct Candidate {
  net::Addr addr;
  CandType type = CandType::kSrflx;
  bool validated = false;
  uint32_t rtt_ms = 0;       // smoothed probe RTT, rounded (srtt_x8 / 8)
  uint32_t srtt_x8 = 0;      // smoothed probe RTT in 1/8 ms
  uint64_t last_recv_ms = 0;  // last authenticated packet received from this address
};

struct Peer {
  PeerId peer_id = 0;
  uint32_t vip = 0;
  Xuid xuid = 0;
  std::string gamertag;
  crypto::PublicKey pk{};
  BootId boot{};
  crypto::Key send_key{};
  crypto::Key recv_key{};
  uint64_t send_ctr = 0;  // last ctr used; the next packet uses send_ctr + 1
  crypto::ReplayWindow recv_window;

  std::vector<Candidate> cands;
  int active = -1;  // index of the active direct path in cands, -1 = relay
  uint64_t path_since_ms = 0;  // when `active` last changed

  uint64_t last_recv_ms = 0;    // any authenticated packet, any path
  uint64_t last_sent_ms = 0;    // anything we sent to this peer
  uint64_t probe_phase_ms = 0;  // start of the current probing schedule
  uint64_t last_probe_ms = 0;
  uint32_t auth_failures = 0;   // consecutive tag failures
  uint64_t last_refresh_ms = 0;
};

// Key state kept for a peer after its record is removed, so a re-created record with the same
// keys (same boot IDs on both ends) never reuses a nonce and keeps rejecting replays.
struct PeerTombstone {
  BootId boot{};
  crypto::PublicKey pk{};
  uint64_t send_ctr = 0;
  uint64_t recv_highest = 0;
};

struct PendingPayload {
  Channel channel = Channel::kDatagram;
  std::vector<uint8_t> data;
  uint64_t t_ms = 0;
};

struct PendingQueue {
  std::deque<PendingPayload> items;
  size_t bytes = 0;
};

struct UnknownPacket {
  PeerId src = 0;
  std::vector<uint8_t> packet;  // the complete P2P packet
  bool relayed = false;
  net::Addr from;
  uint64_t t_ms = 0;
};

struct IntroRequest {
  uint8_t by = 0;      // wire::kIntroByVip / kIntroByPeerId
  uint64_t value = 0;  // vip or peer_id
  uint64_t first_ms = 0;
  uint64_t last_ms = 0;
};

struct RpcCall {
  uint32_t req_id = 0;
  std::vector<std::string> fragments;  // request fragments
  uint64_t deadline_ms = 0;
  uint64_t last_sent_ms = 0;
  bool sent_once = false;

  // Response reassembly.
  uint8_t resp_count = 0;
  std::vector<std::string> resp_parts;
  std::vector<bool> resp_have;
  size_t resp_received = 0;

  bool done = false;
  RpcResult result;
  std::condition_variable cv;
};

// Handler work collected under the lock and delivered after it is released.
struct Events {
  struct Data {
    uint32_t vip;
    Channel channel;
    std::vector<uint8_t> bytes;
  };
  std::vector<Data> data;
  std::vector<uint32_t> lost;
  std::vector<NetState> states;
  std::vector<OverlayRegistration> registrations;

  bool empty() const {
    return data.empty() && lost.empty() && states.empty() && registrations.empty();
  }
};

enum class Route { kBest, kDirect, kRelay };

struct Overlay::Impl {
  // --- Immutable while running ---
  Config cfg;
  crypto::SecretKey sk{};
  crypto::PublicKey pk{};
  crypto::PublicKey server_pk{};
  crypto::Key server_dh{};
  crypto::Key c2s_key{};
  BootId boot{};
  bool direct_allowed = true;  // !force_relay && !privacy
  net::UdpSocket sock4;
  net::UdpSocket sock6;
  std::thread thread;
  std::thread::id thread_id;
  // Windows: interface scans run on this helper (GetAdaptersAddresses can take tens of ms, and
  // much longer with stale VPN/WWAN adapters), never on the network thread or under mu.
  // Started and joined by the network thread only.
  std::thread scan_thread;
  std::atomic<bool> scan_busy{false};
  uint64_t next_scan_ms = 0;  // network thread only

  std::atomic<bool> stop_requested{false};
  std::atomic<uint32_t> local_vip{0};         // lock-free copy of reg.vip
  std::atomic<uint32_t> send_loss_ppm{0};     // test hook
  std::atomic<uint32_t> recv_loss_ppm{0};     // test hook
  std::atomic<bool> block_direct{false};      // test hook
  std::atomic<bool> drop_server_control{false};  // test hook
  // TEST-ONLY impairment (AC6NET_TEST_IMPAIR, overlay_impair.h). impair_on is written by Start()
  // before the network thread exists and is read-only while running, so the send path checks it
  // without the lock; `impair` itself is guarded by mu like the sockets' send side.
  bool impair_on = false;

  // --- Guarded by mu ---
  mutable std::mutex mu;
  bool running = false;
  NetState state = NetState::kDisabled;
  std::condition_variable state_cv;  // state / registration changes
  OverlayRegistration reg;
  OverlayCounters counters;

  // Server channel.
  bool server_resolved = false;
  net::Addr server_addr;
  uint64_t next_resolve_ms = 0;
  uint64_t c2s_ctr = 0;
  bool have_s_boot = false;
  BootId s_boot{};
  crypto::Key s2c_key{};
  crypto::ReplayWindow s2c_window;
  bool session_ok = false;  // HELLO_ACK received for the current server boot
  bool need_hello = true;
  bool online_this_start = false;
  uint64_t start_ms = 0;
  uint64_t last_hello_ms = 0;
  uint64_t next_hello_ms = 0;
  uint64_t hello_interval_ms = overlay_limits::kHelloFirstMs;
  std::vector<net::Addr> local_cands;  // cached LocalCandidatesLocked() result
  uint64_t local_cands_ms = 0;
  bool local_cands_valid = false;
  std::vector<net::Addr> local_scan;  // last background interface scan (Windows)
  bool local_scan_valid = false;
  uint64_t last_s2c_ms = 0;
  uint64_t last_keepalive_ms = 0;
  uint64_t keepalive_sent_ms = 0;  // 0 = no keepalive awaiting its ACK
  uint32_t server_rtt_ms = 0;
  bool check_server_soon = false;  // a relayed peer went quiet: confirm our session early

  // RPC.
  uint32_t next_req_id = 1;
  std::map<uint32_t, std::shared_ptr<RpcCall>> rpc_calls;

  // Introductions.
  uint32_t next_intro_id = 1;
  std::map<uint32_t, IntroRequest> intro_reqs;
  std::map<std::pair<uint8_t, uint64_t>, uint64_t> intro_backoff;  // (by, value) -> until_ms
  std::condition_variable intro_cv;

  // TEST-ONLY impairment: the delay queue (filled by SendRawLocked on any thread, drained by
  // the network thread in ThreadMain) and the periodic impair_stats trace.
  Impairment impair;
  uint64_t impair_trace_ms = 0;
  std::string impair_last_trace;

  // Peers.
  std::unordered_map<PeerId, std::unique_ptr<Peer>> peers;
  std::unordered_map<uint32_t, PeerId> vip_to_peer;
  std::unordered_map<PeerId, PeerTombstone> tombstones;
  std::unordered_map<uint32_t, PendingQueue> pending;  // by vip, before introduction
  std::deque<UnknownPacket> unknown;                    // P2P from not-yet-introduced peers

  // --- Handlers: guarded by handlers_mu; the network thread keeps a snapshot ---
  std::mutex handlers_mu;
  std::atomic<uint64_t> handlers_gen{0};
  DataHandler on_data;
  PeerLostHandler on_lost;
  TickHandler on_tick;
  StateHandler on_state;
  RegisteredHandler on_registered;

  // Network-thread-only snapshot of the handlers.
  struct HandlerSnapshot {
    uint64_t gen = ~uint64_t{0};
    DataHandler on_data;
    PeerLostHandler on_lost;
    TickHandler on_tick;
    StateHandler on_state;
    RegisteredHandler on_registered;
  } snap;

  // --- overlay.cpp ---
  void ThreadMain();
  void ReceiveFrom(net::UdpSocket& sock);
  void MaybeStartLocalScan(uint64_t now);  // network thread, without mu
  void JoinLocalScan();                     // network thread, without mu
  void HandleDatagram(const uint8_t* p, size_t n, const net::Addr& from, Events* ev);
  void DispatchEvents(Events* ev);
  void RefreshHandlerSnapshot();
  bool SendRawLocked(const net::Addr& to, const uint8_t* data, size_t len);
  bool SendNowLocked(const net::Addr& to, const uint8_t* data, size_t len);  // to the socket
  // TEST-ONLY impairment: sends the delayed datagrams that are due (all of them if `flush`),
  // and the impair_stats trace (every 5 s while tracing and changed, or `force`).
  void ImpairReleaseLocked(uint64_t now_us, bool flush);
  void ImpairTraceLocked(uint64_t now_ms, bool force);
  bool ShouldDrop(const std::atomic<uint32_t>& ppm);
  void SetStateLocked(NetState s, Events* ev);
  bool OnNetworkThread() const { return std::this_thread::get_id() == thread_id; }

  // --- overlay_server.cpp ---
  void ResolveServer(uint64_t now);  // called without mu
  void ServerTimersLocked(uint64_t now, Events* ev);
  // This socket's local candidates for HELLO/KEEPALIVE (cached for 30 s), ports filled in.
  const std::vector<net::Addr>& LocalCandidatesLocked(uint64_t now);
  void ScheduleHelloLocked(uint64_t now, uint64_t interval_ms);
  void UpdateServerRttLocked(uint64_t sample_ms);
  void SendHelloLocked(uint64_t now);
  bool SendC2SLocked(uint8_t kind, const uint8_t* body, size_t len);
  void SendKeepaliveLocked(uint64_t now);
  void HandleHelloAckLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev);
  void HandleS2CLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev);
  void HandleRehelloLocked(uint64_t now);
  void HandleRpcRespLocked(wire::Reader& r);
  void HandleKeepaliveAckLocked(const uint8_t* p, size_t n, uint64_t now);
  void HandleNoticeLocked(const uint8_t* p, size_t n, uint64_t now);
  void SendRpcFragmentsLocked(RpcCall& call, uint64_t now);
  void FinishCallLocked(RpcCall& call, RpcResult result);
  // Starts (or joins) an introduction request. Returns false if too many are in flight.
  bool RequestIntroLocked(uint8_t by, uint64_t value, uint64_t now);
  void SendIntroReqLocked(uint32_t req_id, const IntroRequest& req);
  bool IntroInFlightLocked(uint8_t by, uint64_t value) const;
  void IntroTimersLocked(uint64_t now);
  void FailIntroLocked(const IntroRequest& req, uint64_t now);
  void HandleIntroLocked(wire::Reader& r, uint64_t now, Events* ev);
  void HandleIntroFailLocked(wire::Reader& r, uint64_t now, Events* ev);

  // --- overlay_peers.cpp ---
  Peer* FindPeerLocked(PeerId id);
  Peer* FindPeerByVipLocked(uint32_t vip);
  Peer* UpsertPeerLocked(PeerId id, uint32_t vip, Xuid xuid, const std::string& gamertag,
                         const crypto::PublicKey& peer_pk, const BootId& peer_boot,
                         uint64_t now, Events* ev);
  int FindCandidateLocked(const Peer& peer, const net::Addr& addr) const;
  void MergeCandidateLocked(Peer& peer, const net::Addr& addr, CandType type);
  void RemovePeerLocked(PeerId id);
  bool SendP2PLocked(Peer& peer, uint8_t kind, const uint8_t* body, size_t len, Route route,
                     const net::Addr* to, uint64_t now);
  bool SendDataLocked(Peer& peer, Channel channel, const uint8_t* data, size_t len, uint64_t now);
  void HandleP2PLocked(const uint8_t* p, size_t n, bool relayed, const net::Addr& from,
                       uint64_t now, Events* ev);
  void HandleRelayedLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev);
  void ChooseActivePathLocked(Peer& peer, uint64_t now);
  void ProbeAllLocked(Peer& peer, uint64_t now);
  void SendProbeLocked(Peer& peer, const net::Addr& to, uint64_t now);
  void PeerTimersLocked(uint64_t now, Events* ev);
  void BufferUnknownLocked(PeerId src, const uint8_t* p, size_t n, bool relayed,
                           const net::Addr& from, uint64_t now);
  void ReplayUnknownLocked(PeerId src, uint64_t now, Events* ev);
  void FlushPendingLocked(Peer& peer, uint64_t now);
  void ExpireQueuesLocked(uint64_t now);
  void SendByeToPeersLocked(uint64_t now);
};

// Steady-clock helpers shared by all overlay instances (one epoch per process).
uint64_t OverlayNowMs();
uint64_t OverlayNowUs();

// Hex helpers for the u64 identifiers used in JSON (16 lower-case hex digits, big-endian text).
std::string U64ToHex(uint64_t v);
bool U64FromHex(const std::string& s, uint64_t* out);

}  // namespace ac6net
