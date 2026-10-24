// ac6net - overlay lifecycle, network thread, datagram demux, dispatch, PeerLink and status.
#include "overlay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "ac6net/log.h"
#include "nlohmann/json.hpp"
#include "overlay_internal.h"
#include "platform.h"

namespace ac6net {

using json = nlohmann::json;
namespace lim = overlay_limits;

// --- Shared helpers ---

namespace {
const std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();
}

uint64_t OverlayNowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - g_epoch)
                                   .count());
}

uint64_t OverlayNowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - g_epoch)
                                   .count());
}

std::string U64ToHex(uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
  return HexEncode(b, sizeof(b));
}

bool U64FromHex(const std::string& s, uint64_t* out) {
  uint8_t b[8];
  if (!HexDecode(s, b, sizeof(b))) return false;
  uint64_t v = 0;
  for (uint8_t x : b) v = (v << 8) | x;
  *out = v;
  return true;
}

const char* CandTypeName(CandType type) {
  switch (type) {
    case CandType::kLan: return "lan";
    case CandType::kV6: return "v6";
    case CandType::kSrflx: return "srflx";
    case CandType::kPrflx: return "prflx";
  }
  return "?";
}

namespace {

const char* StateName(NetState s) {
  switch (s) {
    case NetState::kDisabled: return "disabled";
    case NetState::kConnecting: return "connecting";
    case NetState::kOnline: return "online";
    case NetState::kOffline: return "offline";
  }
  return "?";
}

// Per-thread xorshift for the loss-injection test hook (not security relevant).
uint32_t TestRandom() {
  thread_local uint64_t s = crypto::RandomU64() | 1;
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return static_cast<uint32_t>(s >> 32);
}

}  // namespace

// --- Overlay ---

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}

Overlay::~Overlay() { Stop(); }

bool Overlay::Start(const Config& config, const crypto::SecretKey& secret_key,
                    const OverlayRegistration* cached) {
  Impl& m = *impl_;
  if (m.thread.joinable()) {
    LogF(LogLevel::kWarn, "overlay: Start called while running");
    return false;
  }
  crypto::PublicKey server_pk;
  if (!HexDecode(config.server_key_hex, server_pk.data(), server_pk.size())) {
    LogF(LogLevel::kError, "overlay: server_key must be 64 hex characters");
    return false;
  }
  crypto::Key server_dh;
  if (!crypto::Dh(secret_key, server_pk, &server_dh)) {
    LogF(LogLevel::kError, "overlay: server_key is not a usable X25519 public key");
    return false;
  }

  std::string error;
  if (!m.sock4.Open(AF_INET, config.port, &error)) {
    LogF(LogLevel::kError, "overlay: cannot open UDP port %u: %s", config.port, error.c_str());
    return false;
  }
  // Best effort: an IPv6 socket on the same port number for v6 candidates and v6 servers.
  if (!m.sock6.Open(AF_INET6, m.sock4.port(), &error)) {
    LogF(LogLevel::kInfo, "overlay: no IPv6 socket on port %u (%s); IPv4 only", m.sock4.port(),
         error.c_str());
  }

  // TEST-ONLY network impairment, read once per Start (overlay_impair.h).
  ImpairConfig impair_cfg;
  if (const char* env = std::getenv(kImpairEnvVar); env && *env) {
    std::string impair_err;
    if (!ParseImpairSpec(env, &impair_cfg, &impair_err)) {
      LogF(LogLevel::kError, "overlay: ignoring %s=\"%.120s\": %s; network impairment stays OFF",
           kImpairEnvVar, env, impair_err.c_str());
      impair_cfg = ImpairConfig{};
    } else if (!impair_cfg.active()) {
      LogF(LogLevel::kInfo, "overlay: %s sets no delay, jitter or loss; impairment off",
           kImpairEnvVar);
    }
  }

  const uint64_t now = OverlayNowMs();
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.cfg = config;
    m.sk = secret_key;
    m.pk = crypto::PublicKeyFromSecret(secret_key);
    m.server_pk = server_pk;
    m.server_dh = server_dh;
    crypto::RandomBytes(m.boot.data(), m.boot.size());
    m.c2s_key = crypto::Kdf(m.server_dh, "AC6NET-C2S-1", {m.pk, m.server_pk, m.boot});
    m.direct_allowed = !config.force_relay && !config.privacy;

    m.running = true;
    m.state = NetState::kConnecting;
    m.reg = cached && cached->valid ? *cached : OverlayRegistration{};
    m.local_vip.store(m.reg.valid ? m.reg.vip : 0);
    m.counters = OverlayCounters{};
    m.server_resolved = false;
    m.next_resolve_ms = 0;
    m.c2s_ctr = 0;
    m.have_s_boot = false;
    m.s2c_window.Reset();
    m.session_ok = false;
    m.need_hello = true;
    m.online_this_start = false;
    m.start_ms = now;
    m.last_hello_ms = 0;
    m.hello_interval_ms = lim::kHelloFirstMs;
    m.last_s2c_ms = now;
    m.last_keepalive_ms = now;
    m.keepalive_sent_ms = 0;
    m.server_rtt_ms = 0;
    m.next_req_id = (crypto::RandomU32() & 0x3FFFFFFFu) | 1u;
    m.rpc_calls.clear();
    m.next_intro_id = (crypto::RandomU32() & 0x3FFFFFFFu) | 1u;
    m.intro_reqs.clear();
    m.intro_backoff.clear();
    m.peers.clear();
    m.vip_to_peer.clear();
    m.tombstones.clear();  // new boot ID: every peer key changes
    m.pending.clear();
    m.unknown.clear();
    m.impair_on = impair_cfg.active();
    m.impair.Configure(impair_cfg, crypto::RandomU64());
    m.impair_trace_ms = now;
    m.impair_last_trace.clear();
    m.local_scan.clear();
    m.local_scan_valid = false;
    m.next_scan_ms = 0;
  }
  m.stop_requested = false;
  {
    // The thread's first action is to take mu, so it cannot observe thread_id unset.
    std::lock_guard<std::mutex> lock(m.mu);
    m.thread = std::thread([&m] { m.ThreadMain(); });
    m.thread_id = m.thread.get_id();
  }
  LogF(LogLevel::kInfo, "overlay: started on UDP %u%s, server %s%s", m.sock4.port(),
       m.sock6.valid() ? " (+IPv6)" : "", config.server.c_str(),
       config.force_relay ? ", force_relay" : "");
  Trace("overlay_start", "\"port\":" + std::to_string(m.sock4.port()) + ",\"v6\":" +
                             (m.sock6.valid() ? "true" : "false"));
  if (m.impair_on) {
    LogF(LogLevel::kWarn,
         "overlay: *** TEST NETWORK IMPAIRMENT ACTIVE (%s): %s on every outgoing datagram. "
         "Test only; unset %s for real play. ***",
         kImpairEnvVar, impair_cfg.Describe().c_str(), kImpairEnvVar);
    char fields[160];
    std::snprintf(fields, sizeof(fields),
                  "\"delay_ms\":%u,\"jitter_ms\":%u,\"loss\":%.6g,\"reorder\":%s",
                  impair_cfg.delay_ms, impair_cfg.jitter_ms, impair_cfg.loss,
                  impair_cfg.reorder ? "true" : "false");
    Trace("impair_start", fields);
  }
  return true;
}

void Overlay::Stop() {
  Impl& m = *impl_;
  if (!m.thread.joinable()) return;
  m.stop_requested = true;
  m.thread.join();
  {
    std::lock_guard<std::mutex> lock(m.mu);
    if (m.impair_on) m.ImpairTraceLocked(OverlayNowMs(), /*force=*/true);
    m.running = false;
    m.state = NetState::kDisabled;
    m.session_ok = false;
    for (auto& [id, call] : m.rpc_calls) {
      RpcResult r;
      r.err = "offline";
      r.msg = "overlay stopped";
      m.FinishCallLocked(*call, std::move(r));
    }
    m.peers.clear();
    m.vip_to_peer.clear();
    m.pending.clear();
    m.unknown.clear();
    m.intro_reqs.clear();
    crypto::Wipe(m.sk.data(), m.sk.size());
    crypto::Wipe(m.server_dh.data(), m.server_dh.size());
    crypto::Wipe(m.c2s_key.data(), m.c2s_key.size());
  }
  m.state_cv.notify_all();
  m.intro_cv.notify_all();
  m.sock4.Close();
  m.sock6.Close();
  Trace("overlay_stop");
}

bool Overlay::running() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->running;
}

NetState Overlay::state() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->state;
}

bool Overlay::WaitOnline(uint32_t timeout_ms) {
  Impl& m = *impl_;
  std::unique_lock<std::mutex> lock(m.mu);
  m.state_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&m] { return !m.running || m.online_this_start; });
  return m.running && m.online_this_start;
}

OverlayRegistration Overlay::registration() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->reg;
}

uint16_t Overlay::local_port() const { return impl_->sock4.port(); }

bool Overlay::has_ipv6() const { return impl_->sock6.valid(); }

crypto::PublicKey Overlay::public_key() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->pk;
}

uint32_t Overlay::ServerRttMs() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->server_rtt_ms;
}

OverlayCounters Overlay::counters() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  OverlayCounters c = impl_->counters;
  const ImpairCounters& ic = impl_->impair.counters();
  c.impair_delayed = ic.delayed;
  c.impair_dropped = ic.dropped;
  c.impair_overflow = ic.overflow;
  c.impair_queue_max = ic.queue_max;
  c.impair_queued = impl_->impair.queued();
  return c;
}

bool Overlay::PeerIdForVip(uint32_t vip, PeerId* peer_id) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto it = impl_->vip_to_peer.find(vip);
  if (it == impl_->vip_to_peer.end()) return false;
  if (peer_id) *peer_id = it->second;
  return true;
}

bool Overlay::ResolveVip(uint32_t vip, uint32_t wait_ms, PeerId* peer_id) {
  Impl& m = *impl_;
  std::unique_lock<std::mutex> lock(m.mu);
  auto known = [&]() -> bool {
    auto it = m.vip_to_peer.find(vip);
    if (it == m.vip_to_peer.end()) return false;
    if (peer_id) *peer_id = it->second;
    return true;
  };
  if (known()) return true;
  if (!m.running || !IsVip(vip) || vip == m.reg.vip || m.OnNetworkThread() || wait_ms == 0) {
    return false;
  }
  if (!m.RequestIntroLocked(wire::kIntroByVip, vip, OverlayNowMs())) return false;
  m.intro_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
    return !m.running || m.vip_to_peer.count(vip) ||
           !m.IntroInFlightLocked(wire::kIntroByVip, vip);
  });
  return known();
}

std::vector<OverlayPeerInfo> Overlay::Peers() const {
  const Impl& m = *impl_;
  std::lock_guard<std::mutex> lock(m.mu);
  const uint64_t now = OverlayNowMs();
  std::vector<OverlayPeerInfo> out;
  out.reserve(m.peers.size());
  for (const auto& [id, peer] : m.peers) {
    OverlayPeerInfo info;
    info.peer_id = peer->peer_id;
    info.vip = peer->vip;
    info.xuid = peer->xuid;
    info.gamertag = peer->gamertag;
    info.direct = peer->active >= 0;
    if (info.direct) {
      const Candidate& c = peer->cands[static_cast<size_t>(peer->active)];
      info.path = c.addr.ToString();
      info.path_type = CandTypeName(c.type);
      info.rtt_ms = c.rtt_ms;
    } else {
      info.path = "relay";
      info.path_type = "relay";
    }
    info.idle_ms = now - std::min(now, peer->last_recv_ms);
    for (const Candidate& c : peer->cands) {
      info.cands.push_back({c.addr.ToString(), CandTypeName(c.type), c.validated, c.rtt_ms});
    }
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(),
            [](const OverlayPeerInfo& a, const OverlayPeerInfo& b) { return a.vip < b.vip; });
  return out;
}

std::string Overlay::StatusJson() const {
  json doc = json::object();
  {
    const Impl& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mu);
    const uint64_t now = OverlayNowMs();
    json server = {
        {"addr", m.server_resolved ? m.server_addr.ToString() : std::string()},
        {"state", StateName(m.state)},
        {"session_ok", m.session_ok},
        {"rtt_ms", m.server_rtt_ms},
        {"last_s2c_age_ms", now - std::min(now, m.last_s2c_ms)},
        {"port", m.sock4.port()},
        {"ipv6", m.sock6.valid()},
        {"force_relay", m.cfg.force_relay},
        {"privacy", m.cfg.privacy},
    };
    if (m.reg.valid) {
      server["vip"] = Ipv4ToString(m.reg.vip);
      server["peer_id"] = U64ToHex(m.reg.peer_id);
      server["public"] = m.reg.public_ip.empty()
                             ? std::string()
                             : m.reg.public_ip + ":" + std::to_string(m.reg.public_port);
    }
    doc["server"] = std::move(server);
    const auto& c = m.counters;
    doc["counters"] = {
        {"datagrams_sent", c.datagrams_sent}, {"datagrams_recv", c.datagrams_recv},
        {"bytes_sent", c.bytes_sent}, {"bytes_recv", c.bytes_recv},
        {"send_errors", c.send_errors}, {"malformed", c.malformed},
        {"auth_failures", c.auth_failures}, {"replays", c.replays},
        {"hello_sent", c.hello_sent}, {"hello_acks", c.hello_acks},
        {"rehello_recv", c.rehello_recv}, {"rpc_calls", c.rpc_calls},
        {"rpc_retransmits", c.rpc_retransmits}, {"rpc_timeouts", c.rpc_timeouts},
        {"intro_reqs", c.intro_reqs}, {"intros_recv", c.intros_recv},
        {"intro_fails", c.intro_fails}, {"data_sent_direct", c.data_sent_direct},
        {"data_sent_relay", c.data_sent_relay}, {"data_recv_direct", c.data_recv_direct},
        {"data_recv_relay", c.data_recv_relay}, {"data_queued", c.data_queued},
        {"data_dropped", c.data_dropped}, {"unknown_buffered", c.unknown_buffered},
        {"unknown_dropped", c.unknown_dropped}, {"unknown_direct", c.unknown_direct},
        {"probes_sent", c.probes_sent},
        {"probe_acks_recv", c.probe_acks_recv}, {"path_switches", c.path_switches},
        {"relay_fallbacks", c.relay_fallbacks}, {"peers_lost", c.peers_lost},
        {"peer_table_full", c.peer_table_full},
        {"test_dropped", c.test_dropped},
    };
    // TEST-ONLY impairment (AC6NET_TEST_IMPAIR).
    const ImpairCounters& ic = m.impair.counters();
    json impair = {{"active", m.impair_on}};
    if (m.impair_on) {
      const ImpairConfig& icfg = m.impair.config();
      impair["delay_ms"] = icfg.delay_ms;
      impair["jitter_ms"] = icfg.jitter_ms;
      impair["loss"] = icfg.loss;
      impair["reorder"] = icfg.reorder;
    }
    impair["delayed"] = ic.delayed;
    impair["dropped"] = ic.dropped;
    impair["overflow"] = ic.overflow;
    impair["queue_max"] = ic.queue_max;
    impair["queued"] = m.impair.queued();
    doc["impair"] = std::move(impair);
  }
  json peers = json::array();
  for (const OverlayPeerInfo& p : Peers()) {
    json cands = json::array();
    for (const auto& c : p.cands) {
      cands.push_back({{"addr", c.addr}, {"type", c.type}, {"ok", c.validated},
                       {"rtt_ms", c.rtt_ms}});
    }
    peers.push_back({{"vip", Ipv4ToString(p.vip)}, {"peer_id", U64ToHex(p.peer_id)},
                     {"gamertag", p.gamertag}, {"path", p.path}, {"path_type", p.path_type},
                     {"rtt_ms", p.rtt_ms}, {"idle_ms", p.idle_ms}, {"cands", cands}});
  }
  doc["peers"] = std::move(peers);
  doc["max_payload"] = wire::kMaxDataPayload;
  return doc.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool Overlay::Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) {
  if (len > wire::kMaxDataPayload || !IsVip(vip)) return false;
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lock(m.mu);
  if (!m.running || vip == m.reg.vip) return false;
  const uint64_t now = OverlayNowMs();
  if (Peer* peer = m.FindPeerByVipLocked(vip); peer && m.reg.valid) {
    return m.SendDataLocked(*peer, channel, data, len, now);
  }
  // Unknown vIP: queue the payload and ask the server for an introduction.
  auto it = m.pending.find(vip);
  if (it == m.pending.end()) {
    if (m.pending.size() >= lim::kMaxPendingVips) {
      ++m.counters.data_dropped;
      return false;
    }
    it = m.pending.emplace(vip, PendingQueue{}).first;
  }
  PendingQueue& q = it->second;
  if (q.items.size() >= lim::kPendingPerVip || q.bytes + len > lim::kPendingBytesPerVip) {
    ++m.counters.data_dropped;
    return false;
  }
  if (!m.RequestIntroLocked(wire::kIntroByVip, vip, now)) {
    if (q.items.empty()) m.pending.erase(it);
    ++m.counters.data_dropped;
    return false;
  }
  q.items.push_back({channel, std::vector<uint8_t>(data, data + len), now});
  q.bytes += len;
  ++m.counters.data_queued;
  return true;
}

uint32_t Overlay::LocalVip() const { return impl_->local_vip.load(std::memory_order_relaxed); }

size_t Overlay::MaxPayload() const { return wire::kMaxDataPayload; }

uint64_t Overlay::NowMs() const { return OverlayNowMs(); }

void Overlay::SetDataHandler(DataHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mu);
  impl_->on_data = std::move(handler);
  impl_->handlers_gen.fetch_add(1);
}

void Overlay::SetPeerLostHandler(PeerLostHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mu);
  impl_->on_lost = std::move(handler);
  impl_->handlers_gen.fetch_add(1);
}

void Overlay::SetTickHandler(TickHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mu);
  impl_->on_tick = std::move(handler);
  impl_->handlers_gen.fetch_add(1);
}

void Overlay::SetStateHandler(StateHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mu);
  impl_->on_state = std::move(handler);
  impl_->handlers_gen.fetch_add(1);
}

void Overlay::SetRegisteredHandler(RegisteredHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mu);
  impl_->on_registered = std::move(handler);
  impl_->handlers_gen.fetch_add(1);
}

void Overlay::SetTestLoss(double send_fraction, double recv_fraction) {
  auto ppm = [](double f) {
    return static_cast<uint32_t>(std::clamp(f, 0.0, 1.0) * 1000000.0);
  };
  impl_->send_loss_ppm = ppm(send_fraction);
  impl_->recv_loss_ppm = ppm(recv_fraction);
}

void Overlay::SetTestBlockDirect(bool block) { impl_->block_direct = block; }

void Overlay::SetTestDropServerControl(bool drop) { impl_->drop_server_control = drop; }

// --- Impl: network thread ---

void Overlay::Impl::ThreadMain() {
  platform::SetCurrentThreadName("ac6net-overlay");
  const net::ScopedTimerResolution timer_resolution;  // Windows only: 1 ms timer for the loop
  while (!stop_requested.load()) {
    uint64_t now = OverlayNowMs();
    bool resolve = false;
    {
      std::lock_guard<std::mutex> lock(mu);
      resolve = !server_resolved && now >= next_resolve_ms;
    }
    if (resolve) ResolveServer(now);  // may block on DNS; never under mu
    MaybeStartLocalScan(now);         // Windows: interface scan on a helper thread

    int timeout_ms = static_cast<int>(lim::kLoopMs);
    if (impair_on) {
      // Wake up for the next delayed datagram if it is due before the regular tick.
      std::lock_guard<std::mutex> lock(mu);
      const uint64_t due = impair.NextDueUs();
      if (due != std::numeric_limits<uint64_t>::max()) {
        const uint64_t now_us = OverlayNowUs();
        const uint64_t wait_ms = due <= now_us ? 0 : (due - now_us + 999) / 1000;
        timeout_ms = static_cast<int>(std::min<uint64_t>(wait_ms, lim::kLoopMs));
      }
    }
    net::UdpSocket* socks[2] = {&sock4, &sock6};
    bool readable[2] = {false, false};
    net::PollReadable(socks, 2, timeout_ms, readable);
    if (readable[0]) ReceiveFrom(sock4);
    if (readable[1]) ReceiveFrom(sock6);

    now = OverlayNowMs();
    Events ev;
    {
      std::lock_guard<std::mutex> lock(mu);
      ServerTimersLocked(now, &ev);
      IntroTimersLocked(now);
      PeerTimersLocked(now, &ev);
      ExpireQueuesLocked(now);
      if (impair_on) {
        ImpairReleaseLocked(OverlayNowUs(), /*flush=*/false);
        ImpairTraceLocked(now, /*force=*/false);
      }
    }
    DispatchEvents(&ev);
    RefreshHandlerSnapshot();
    if (snap.on_tick) snap.on_tick(now);
  }
  JoinLocalScan();  // it takes mu when it finishes
  // Shutdown: say goodbye while the sockets are still open.
  std::lock_guard<std::mutex> lock(mu);
  const uint64_t now = OverlayNowMs();
  SendByeToPeersLocked(now);
  if (session_ok) SendC2SLocked(wire::kBye, nullptr, 0);
  // Impairment: whatever is still delayed (BYE included) leaves now, while the sockets are open.
  if (impair_on) ImpairReleaseLocked(OverlayNowUs(), /*flush=*/true);
}

void Overlay::Impl::ReceiveFrom(net::UdpSocket& sock) {
  uint8_t buf[2048];
  // Bounded per wakeup so timers keep running under a flood.
  for (int i = 0; i < 128; ++i) {
    net::Addr from;
    const int n = sock.RecvFrom(buf, sizeof(buf), &from);
    if (n == 0) break;
    if (n < 0) continue;
    if (ShouldDrop(recv_loss_ppm)) {
      std::lock_guard<std::mutex> lock(mu);
      ++counters.test_dropped;
      continue;
    }
    Events ev;
    HandleDatagram(buf, static_cast<size_t>(n), from, &ev);
    DispatchEvents(&ev);
  }
}

void Overlay::Impl::HandleDatagram(const uint8_t* p, size_t n, const net::Addr& from,
                                   Events* ev) {
  std::lock_guard<std::mutex> lock(mu);
  ++counters.datagrams_recv;
  counters.bytes_recv += n;
  if (n < 2 || p[0] != wire::kMagic || n > wire::kMaxDatagram + 64) {
    ++counters.malformed;
    return;
  }
  const uint64_t now = OverlayNowMs();
  const bool from_server = server_resolved && from == server_addr;
  if ((!from_server && block_direct.load(std::memory_order_relaxed)) ||
      (from_server && (p[1] == wire::kHelloAck || p[1] == wire::kS2C) &&
       drop_server_control.load(std::memory_order_relaxed))) {
    ++counters.test_dropped;
    return;
  }
  try {
    switch (p[1]) {
      case wire::kHelloAck:
        if (from_server) HandleHelloAckLocked(p, n, now, ev);
        else ++counters.malformed;
        break;
      case wire::kS2C:
        if (from_server) HandleS2CLocked(p, n, now, ev);
        else ++counters.malformed;
        break;
      case wire::kRehello:
        if (from_server) HandleRehelloLocked(now);
        else ++counters.malformed;
        break;
      case wire::kRelayed:
        if (from_server) HandleRelayedLocked(p, n, now, ev);
        else ++counters.malformed;
        break;
      case wire::kP2P:
        HandleP2PLocked(p, n, /*relayed=*/false, from, now, ev);
        break;
      default:
        ++counters.malformed;
        break;
    }
  } catch (const std::exception& e) {
    // JSON handling is written not to throw; this is a last line of defence for the thread.
    ++counters.malformed;
    LogF(LogLevel::kWarn, "overlay: dropped a packet that raised: %s", e.what());
  }
}

void Overlay::Impl::RefreshHandlerSnapshot() {
  const uint64_t gen = handlers_gen.load();
  if (gen == snap.gen) return;
  std::lock_guard<std::mutex> lock(handlers_mu);
  snap.gen = handlers_gen.load();
  snap.on_data = on_data;
  snap.on_lost = on_lost;
  snap.on_tick = on_tick;
  snap.on_state = on_state;
  snap.on_registered = on_registered;
}

void Overlay::Impl::DispatchEvents(Events* ev) {
  if (ev->empty()) return;
  RefreshHandlerSnapshot();
  for (const auto& r : ev->registrations)
    if (snap.on_registered) snap.on_registered(r);
  for (NetState s : ev->states)
    if (snap.on_state) snap.on_state(s);
  for (const auto& d : ev->data)
    if (snap.on_data) snap.on_data(d.vip, d.channel, d.bytes.data(), d.bytes.size());
  for (uint32_t vip : ev->lost)
    if (snap.on_lost) snap.on_lost(vip);
  *ev = Events{};
}

bool Overlay::Impl::ShouldDrop(const std::atomic<uint32_t>& ppm) {
  const uint32_t v = ppm.load(std::memory_order_relaxed);
  if (v == 0) return false;
  return (TestRandom() % 1000000u) < v;
}

bool Overlay::Impl::SendRawLocked(const net::Addr& to, const uint8_t* data, size_t len) {
  if (ShouldDrop(send_loss_ppm) || (block_direct.load(std::memory_order_relaxed) &&
                                     !(server_resolved && to == server_addr))) {
    ++counters.test_dropped;
    return true;  // as far as the caller can tell, the datagram left
  }
  if (impair_on) {
    // TEST-ONLY impairment: a dropped or delayed datagram "left" as far as the caller knows.
    if (impair.Submit(to, data, len, OverlayNowUs()) != Impairment::Verdict::kSendNow) {
      return true;
    }
  }
  return SendNowLocked(to, data, len);
}

bool Overlay::Impl::SendNowLocked(const net::Addr& to, const uint8_t* data, size_t len) {
  net::UdpSocket& sock = to.IsV6() ? sock6 : sock4;
  if (!sock.SendTo(to, data, len)) {
    ++counters.send_errors;
    return false;
  }
  ++counters.datagrams_sent;
  counters.bytes_sent += len;
  return true;
}

void Overlay::Impl::ImpairReleaseLocked(uint64_t now_us, bool flush) {
  Impairment::Packet pkt;
  while (flush ? impair.PopAny(&pkt) : impair.PopDue(now_us, &pkt)) {
    SendNowLocked(pkt.to, pkt.data.data(), pkt.data.size());
  }
}

void Overlay::Impl::ImpairTraceLocked(uint64_t now_ms, bool force) {
  if (!TraceEnabled()) return;
  if (!force && now_ms - std::min(now_ms, impair_trace_ms) < 5000) return;
  impair_trace_ms = now_ms;
  const ImpairCounters& c = impair.counters();
  const std::string fields =
      "\"delayed\":" + std::to_string(c.delayed) + ",\"dropped\":" + std::to_string(c.dropped) +
      ",\"overflow\":" + std::to_string(c.overflow) +
      ",\"queue_max\":" + std::to_string(c.queue_max) +
      ",\"queue_max_bytes\":" + std::to_string(c.queue_max_bytes) +
      ",\"queued\":" + std::to_string(impair.queued());
  if (!force && fields == impair_last_trace) return;
  impair_last_trace = fields;
  Trace("impair_stats", fields + (force ? ",\"final\":true" : ""));
}

void Overlay::Impl::SetStateLocked(NetState s, Events* ev) {
  if (state == s) return;
  LogF(LogLevel::kInfo, "overlay: state %s -> %s", StateName(state), StateName(s));
  Trace("net_state", std::string("\"from\":\"") + StateName(state) + "\",\"to\":\"" +
                         StateName(s) + "\"");
  state = s;
  ev->states.push_back(s);
  state_cv.notify_all();
}

}  // namespace ac6net
