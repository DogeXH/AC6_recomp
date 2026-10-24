// ac6net - virtual BSD sockets (ARCHITECTURE.md 5.4/5.5; wire format PROTOCOL.md 2.8).
//
// Locking: one mutex (Impl::mu) guards all state and one condition variable (Impl::cv) is
// notified after any state change; every blocking call and Select waits on it. PeerLink::Send is
// called with the mutex held, which the overlay contract allows (it never calls back into vnet
// while holding its own locks).
//
// Datagrams (channel 0x01): a datagram larger than MaxPayload - 9 is split into at most 8
// fragments that share a per-socket msg_id; the receiver reassembles by (source vip, source
// port, msg_id) and drops incomplete datagrams after 2 s.
//
// Streams (channels 0x02/0x03): one KCP control block per connection in message mode, so each
// guest send() is delivered as one whole message and recv() serves bytes from complete messages
// only. That gives the game its framing guarantee: once select() reports a stream readable, the
// whole frame that starts there is readable.
//
// Loopback (own vip or 127.0.0.1): datagrams are delivered directly. Stream packets go through
// an in-process queue (loop_q) that is drained after the current operation, so a KCP control
// block is never re-entered from inside its own output callback.
#include "ac6net/vnet.h"

#include "ac6net/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <random>
#include <vector>

#include "ac6net/netplay.h"
#include "kcp/ikcp.h"
#include "link.h"
#include "vnet_wire.h"

namespace ac6net::vnet {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSockTNoSupport = 10044;  // WSAESOCKTNOSUPPORT (not listed in types.h)
constexpr int kMsgOob = 0x1;
constexpr int kMsgPeek = 0x2;

constexpr SocketId kMaxSocketId = 4095;
constexpr uint32_t kEphemeralFirst = 49152;
constexpr uint32_t kEphemeralCount = 65536 - kEphemeralFirst;
constexpr int kDefaultBufSize = 65536;
constexpr int kMaxBacklog = 64;
constexpr int64_t kMaxSelectTimeoutUs = int64_t(1) << 40;  // ~12 days; avoids clock overflow

constexpr size_t kDgramQueueLimit = 256;
constexpr uint64_t kReassemblyTimeoutMs = 2000;
constexpr size_t kMaxReassemblies = 128;

constexpr uint64_t kSynIntervalMs = 250;
constexpr uint64_t kConnectTimeoutMs = 5000;
constexpr uint64_t kFinIntervalMs = 250;
constexpr uint64_t kTimeWaitMs = 2000;
constexpr uint64_t kOrphanLingerMs = 20000;
constexpr uint64_t kTombstoneMs = 10000;
constexpr size_t kMaxSendCall = 32 * 1024;
constexpr size_t kSendWindowBytes = 256 * 1024;
constexpr size_t kRecvQueueBytes = 512 * 1024;

constexpr int kKcpWindow = 256;
constexpr int kKcpInterval = 10;

enum class StreamState { kIdle, kListening, kConnecting, kConnected, kFailed, kReset };

// A connection key names one end of a connection. Remote connections use kDirRemote: the
// (peer vip, conn_id) pair is unique because conn_id carries the "initiator is the lower vip"
// bit. Both ends of a loopback connection live in this process with the same ip and conn_id, so
// the key also records which end it is (and loopback packets carry the end they are for).
enum Dir : uint8_t { kDirRemote = 0, kDirInitiator = 1, kDirAcceptor = 2 };

uint8_t ReplyDir(uint8_t dir) {
  if (dir == kDirInitiator) return kDirAcceptor;
  if (dir == kDirAcceptor) return kDirInitiator;
  return kDirRemote;
}

struct ConnKey {
  uint32_t ip = 0;
  uint32_t conn_id = 0;
  uint8_t dir = kDirRemote;
  auto operator<=>(const ConnKey&) const = default;
};

struct ReasmKey {
  uint32_t ip = 0;
  uint16_t src_port = 0;
  uint16_t msg_id = 0;
  auto operator<=>(const ReasmKey&) const = default;
};

// Milliseconds from `since` to `now`, 0 if the clock reads earlier (callers mix Tick's now with
// fresh NowMs() reads).
uint64_t Elapsed(uint64_t now, uint64_t since) { return now > since ? now - since : 0; }

// Sets the Winsock error and returns the -1 failure result.
int Fail(int* err, int code) {
  *err = code;
  return -1;
}

}  // namespace

struct VNet::Impl {
  struct Conn;

  // Rate-limited JSONL trace (log.h). At most kTraceBudget events per key, so a misbehaving
  // stream cannot flood the trace file.
  static constexpr int kTraceBudget = 40;
  std::map<std::string, int> trace_counts;
  void Tr(const std::string& key, const char* ev, const std::string& fields) {
    if (!TraceEnabled()) return;
    int& n = trace_counts[key];
    if (n >= kTraceBudget) return;
    if (++n == kTraceBudget) {
      Trace(ev, fields + ",\"rate_limited\":true");
    } else {
      Trace(ev, fields);
    }
  }
  // AC6's engine packets start with a 20-byte header: u32 CRC, u16 message id, ... (re/transport_map.md
  // section 5). Streams carry them framed as [u16 LE length][header...]; UDP datagrams carry them bare.
  static void CountId(std::map<uint32_t, uint32_t>& m, const uint8_t* p, size_t len, size_t off) {
    if (len < off + 6) return;
    const uint32_t id = (uint32_t(p[off + 4]) << 8) | p[off + 5];
    if (m.size() < 64 || m.count(id)) m[id]++;
  }
  // One "vnet.msg" line per game-channel message (not VDP), with the first bytes in hex: shows
  // what the engine exchanges while a mission starts. Bounded per process.
  static constexpr int kMsgTraceBudget = 40000;
  int msg_traces = 0;
  void TraceMsg(const char* dir, uint64_t sock_id, uint16_t local_port, const std::string& peer,
                const uint8_t* p, size_t len, size_t off) {
    if (!TraceEnabled() || msg_traces >= kMsgTraceBudget) return;
    ++msg_traces;
    char hex[2 * 40 + 1];
    const size_t m = std::min(len, size_t(40));
    for (size_t i = 0; i < m; ++i) std::snprintf(hex + 2 * i, 3, "%02x", p[i]);
    hex[2 * m] = 0;
    const int id = len >= off + 6 ? int((uint32_t(p[off + 4]) << 8) | p[off + 5]) : -1;
    Trace("vnet.msg", std::string("\"d\":\"") + dir + "\",\"s\":" + std::to_string(sock_id) +
                          ",\"lp\":" + std::to_string(local_port) + ",\"peer\":" + peer +
                          ",\"n\":" + std::to_string(len) + ",\"id\":" + std::to_string(id) +
                          ",\"h\":\"" + hex + "\"");
  }
  static std::string Hist(const std::map<uint32_t, uint32_t>& m) {
    std::string out;
    for (const auto& [id, n] : m) {
      if (!out.empty()) out += ' ';
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%04x:%u", id, n);
      out += buf;
    }
    return "\"" + out + "\"";
  }
  static std::string Ep(Endpoint e) {
    return "\"" + Ipv4ToString(e.ip) + ":" + std::to_string(e.port) + "\"";
  }

  struct Datagram {
    Endpoint from;
    std::vector<uint8_t> data;
  };

  struct Sock {
    SocketId id = 0;   // 0 while waiting in a listener's accept queue
    uint64_t gen = 0;  // distinguishes a reused id across a blocking wait
    int type = 0;
    int protocol = 0;
    bool non_blocking = false;
    bool bound = false;
    bool owns_port = false;  // holds the port-table entry for local.port
    Endpoint local;
    bool has_peer = false;  // stream: connected peer; datagram: connect() default destination
    Endpoint peer;
    bool shut_rd = false;
    bool shut_wr = false;
    // Options.
    int so_error = 0;
    int sndbuf = kDefaultBufSize;
    int rcvbuf = kDefaultBufSize;
    uint32_t sndtimeo_ms = 0;
    uint32_t rcvtimeo_ms = 0;
    bool reuse_addr = false;
    bool keepalive = false;
    bool broadcast = false;
    bool nodelay = false;
    uint16_t linger_onoff = 0;
    uint16_t linger_s = 0;
    // Datagram state.
    std::deque<Datagram> dgrams;
    size_t dgram_bytes = 0;
    uint16_t next_msg_id = 0;
    std::set<uint64_t> seen_from;  // datagram sources already traced (vnet.first_dgram)
    uint64_t tx_n = 0, tx_b = 0, rx_n = 0, rx_b = 0;  // datagram counters (vnet.stats)
    // Diagnostics: AC6 game-engine message ids (u16 at header +4) seen per direction.
    std::map<uint32_t, uint32_t> tx_ids, rx_ids;
    // Stream state. Invariant: conn != nullptr exactly in kConnecting and kConnected.
    StreamState st = StreamState::kIdle;
    Conn* conn = nullptr;
    int backlog = 0;
    std::deque<std::unique_ptr<Sock>> accept_q;
    Sock* listener = nullptr;  // set while this socket waits in `listener`'s accept queue

    bool IsStream() const { return type == kSockStream; }
  };

  struct Conn {
    Impl* owner = nullptr;
    ConnKey key;
    uint32_t peer_ip = 0;  // vip to send to (the loopback address used, for loopback)
    uint16_t local_port = 0;
    uint16_t peer_port = 0;
    bool loopback = false;
    bool initiator = false;
    bool established = false;
    ikcpcb* kcp = nullptr;
    Sock* sock = nullptr;  // nullptr once the guest closed the socket (orphan)
    uint64_t connect_deadline_ms = 0;
    uint64_t next_syn_ms = 0;
    uint64_t next_update_ms = 0;
    // Send side: one entry per message not yet fully acknowledged.
    struct InFlight {
      uint32_t last_sn;
      uint32_t bytes;
    };
    std::deque<InFlight> inflight;
    size_t unacked_bytes = 0;
    uint32_t msgs_sent = 0;
    uint64_t tx_bytes = 0;  // total bytes queued by the guest (vnet.stats)
    uint64_t rx_total = 0;  // total bytes received (vnet.stats)
    bool fin_sent = false;
    bool fin_acked = false;
    uint64_t next_fin_ms = 0;
    // Receive side: whole messages; rx_off indexes into the front one.
    uint32_t msgs_recv = 0;
    bool fin_recv = false;
    bool finack_sent = false;
    uint32_t fin_count = 0;
    std::deque<std::vector<uint8_t>> rx;
    size_t rx_off = 0;
    size_t rx_bytes = 0;  // unread bytes in rx
    // Lifetime after the guest closed the socket.
    uint64_t orphan_since_ms = 0;
    uint64_t time_wait_since_ms = 0;

    ~Conn() {
      if (kcp) ikcp_release(kcp);
    }
    // The peer's FIN arrived and every message it announced has been received.
    bool PeerFinComplete() const { return fin_recv && msgs_recv >= fin_count; }
    bool Eof() const { return PeerFinComplete() && rx_bytes == 0; }
  };

  struct Reasm {
    uint16_t dst_port = 0;
    uint8_t frag_cnt = 0;
    uint8_t got = 0;
    uint64_t first_ms = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<bool> have;
  };

  struct LoopPacket {
    Channel channel;
    uint32_t ip;  // the loopback address the connection uses
    uint8_t dir;  // the end the packet is for
    std::vector<uint8_t> data;
  };

  // Locks the state for one public call; on exit drains the loopback queue and wakes waiters.
  struct Guard {
    explicit Guard(Impl& i) : impl(i), lk(i.mu) {}
    ~Guard() {
      impl.DrainLoop();
      impl.Publish();
    }
    Impl& impl;
    std::unique_lock<std::mutex> lk;
  };

  explicit Impl(PeerLink* l) : link(l) {
    std::random_device rd;
    next_conn_counter = rd() & 0x7FFFFFFF;
    next_dgram_eph = kEphemeralFirst + rd() % kEphemeralCount;
    next_stream_eph = kEphemeralFirst + rd() % kEphemeralCount;
  }

  PeerLink* link;
  mutable std::mutex mu;
  std::condition_variable cv;
  bool changed = false;
  std::map<SocketId, std::unique_ptr<Sock>> socks;
  SocketId next_id = 1;
  uint64_t next_gen = 1;
  std::map<uint16_t, Sock*> dgram_ports;
  std::map<uint16_t, Sock*> stream_ports;
  uint32_t next_dgram_eph = kEphemeralFirst;
  uint32_t next_stream_eph = kEphemeralFirst;
  std::map<ConnKey, std::unique_ptr<Conn>> conns;
  // Recently destroyed connections. After a graceful close late packets are dropped silently
  // (like TIME_WAIT); after an abortive one they are answered with RST, in case ours was lost.
  struct Tombstone {
    uint64_t since_ms = 0;
    bool graceful = false;
  };
  std::map<ConnKey, Tombstone> tombstones;
  uint32_t next_conn_counter = 0;
  std::map<ReasmKey, Reasm> reasm;
  std::deque<LoopPacket> loop_q;
  bool draining = false;
  Stats stats;

  // ---------------------------------------------------------------- helpers

  uint64_t Now() const { return link->NowMs(); }
  uint32_t OwnVip() const { return link->LocalVip(); }
  bool IsLocalIp(uint32_t ip) const {
    if (ip == kLoopback) return true;
    const uint32_t own = OwnVip();
    return own != 0 && ip == own;
  }
  size_t FragPayload() const {
    const size_t mp = link->MaxPayload();
    return mp > wire::kDgramHeader ? mp - wire::kDgramHeader : 0;
  }

  void Changed() { changed = true; }
  void Publish() {
    if (changed) {
      changed = false;
      cv.notify_all();
    }
  }

  // Waits for any state change (spurious wake-ups are fine: callers re-check). Returns false
  // once the deadline has passed. If draining the loopback queue (or the caller) changed state,
  // it returns at once so the caller re-checks before sleeping.
  bool WaitChange(std::unique_lock<std::mutex>& lk, const std::optional<Clock::time_point>& dl) {
    DrainLoop();
    if (changed) {
      Publish();
      return !dl || Clock::now() < *dl;
    }
    if (!dl) {
      cv.wait(lk);
      return true;
    }
    return cv.wait_until(lk, *dl) != std::cv_status::timeout;
  }

  static std::optional<Clock::time_point> DeadlineMs(uint32_t timeout_ms) {
    if (timeout_ms == 0) return std::nullopt;  // Winsock: 0 = wait forever
    return Clock::now() + std::chrono::milliseconds(timeout_ms);
  }

  Sock* Find(SocketId s) {
    auto it = socks.find(s);
    return it == socks.end() ? nullptr : it->second.get();
  }
  Sock* FindGen(SocketId s, uint64_t gen) {
    Sock* k = Find(s);
    return (k && k->gen == gen) ? k : nullptr;
  }
  Sock* Lookup(SocketId s, int* err) {
    Sock* k = Find(s);
    if (!k) *err = wsa::kNotSock;
    return k;
  }

  SocketId AllocId() {
    for (SocketId i = 0; i < kMaxSocketId; ++i) {
      const SocketId id = next_id;
      next_id = next_id % kMaxSocketId + 1;
      if (!socks.count(id)) return id;
    }
    return 0;
  }

  std::map<uint16_t, Sock*>& PortsFor(const Sock& k) {
    return k.IsStream() ? stream_ports : dgram_ports;
  }

  static bool AllocEphemeral(const std::map<uint16_t, Sock*>& ports, uint32_t& cursor,
                             uint16_t* out) {
    for (uint32_t i = 0; i < kEphemeralCount; ++i) {
      const uint32_t p = kEphemeralFirst + (cursor - kEphemeralFirst + i) % kEphemeralCount;
      if (!ports.count(uint16_t(p))) {
        cursor = kEphemeralFirst + (p - kEphemeralFirst + 1) % kEphemeralCount;
        *out = uint16_t(p);
        return true;
      }
    }
    return false;
  }

  // Ports are exclusive per kind (one datagram and one stream port space) regardless of the
  // bound address. SO_REUSEADDR never lets two live sockets share a port: AC6's per-client UDP
  // bind loop (20000+i) depends on kAddrInUse.
  int BindLocked(Sock* k, Endpoint local, int* err) {
    if (local.ip != kInaddrAny && !IsLocalIp(local.ip)) return Fail(err, wsa::kAddrNotAvail);
    auto& ports = PortsFor(*k);
    uint16_t port = local.port;
    if (port == 0) {
      uint32_t& cursor = k->IsStream() ? next_stream_eph : next_dgram_eph;
      if (!AllocEphemeral(ports, cursor, &port)) return Fail(err, wsa::kNoBufs);
    } else if (ports.count(port))
      return Fail(err, wsa::kAddrInUse);
    ports[port] = k;
    k->bound = true;
    k->owns_port = true;
    k->local = {local.ip, port};
    Tr("bind" + std::to_string(port), "vnet.bind",
       "\"sock\":" + std::to_string(k->id) + ",\"type\":" + std::to_string(k->type) +
           ",\"proto\":" + std::to_string(k->protocol) + ",\"local\":" + Ep(k->local));
    *err = wsa::kOk;
    return 0;
  }

  void ReleasePort(Sock* k) {
    if (!k->owns_port) return;
    auto& ports = PortsFor(*k);
    auto it = ports.find(k->local.port);
    if (it != ports.end() && it->second == k) ports.erase(it);
    k->owns_port = false;
  }

  // ---------------------------------------------------------------- loopback / sending

  void SendCtlTo(uint32_t ip, uint8_t incoming_dir, const wire::CtlMsg& m) {
    uint8_t buf[wire::kCtlMaxSize];
    const size_t n = wire::EncodeCtl(m, buf);
    if (incoming_dir == kDirRemote) {
      link->Send(ip, Channel::kStreamCtl, buf, n);
    } else {
      loop_q.push_back(
          {Channel::kStreamCtl, ip, ReplyDir(incoming_dir), std::vector<uint8_t>(buf, buf + n)});
    }
  }

  void SendRaw(Channel channel, Conn* c, const uint8_t* data, size_t len) {
    if (c->loopback) {
      loop_q.push_back({channel, c->peer_ip,
                        c->initiator ? uint8_t(kDirAcceptor) : uint8_t(kDirInitiator),
                        std::vector<uint8_t>(data, data + len)});
    } else {
      link->Send(c->peer_ip, channel, data, len);
    }
  }

  void SendCtl(Conn* c, wire::Ctl type) {
    wire::CtlMsg m;
    m.type = type;
    m.conn_id = c->key.conn_id;
    m.src_port = c->local_port;
    m.dst_port = c->peer_port;
    m.msg_count = c->msgs_sent;
    uint8_t buf[wire::kCtlMaxSize];
    const size_t n = wire::EncodeCtl(m, buf);
    SendRaw(Channel::kStreamCtl, c, buf, n);
  }

  static int KcpOutput(const char* buf, int len, ikcpcb*, void* user) {
    auto* c = static_cast<Conn*>(user);
    if (len > 0) {
      c->owner->SendRaw(Channel::kStream, c, reinterpret_cast<const uint8_t*>(buf), size_t(len));
    }
    return 0;
  }

  void DrainLoop() {
    if (draining) return;
    draining = true;
    while (!loop_q.empty()) {
      LoopPacket p = std::move(loop_q.front());
      loop_q.pop_front();
      if (p.channel == Channel::kStream) {
        OnStreamPacket(p.ip, p.dir, p.data.data(), p.data.size());
      } else if (p.channel == Channel::kStreamCtl) {
        OnCtlPacket(p.ip, p.dir, p.data.data(), p.data.size());
      }
    }
    draining = false;
  }

  // ---------------------------------------------------------------- connections

  Conn* NewConn(const ConnKey& key, uint32_t peer_ip, uint16_t local_port, uint16_t peer_port,
                bool loopback, bool initiator) {
    auto c = std::make_unique<Conn>();
    c->owner = this;
    c->key = key;
    c->peer_ip = peer_ip;
    c->local_port = local_port;
    c->peer_port = peer_port;
    c->loopback = loopback;
    c->initiator = initiator;
    c->kcp = ikcp_create(key.conn_id, c.get());
    if (!c->kcp) return nullptr;
    c->kcp->output = &Impl::KcpOutput;
    if (ikcp_setmtu(c->kcp, int(link->MaxPayload())) < 0) return nullptr;
    ikcp_wndsize(c->kcp, kKcpWindow, kKcpWindow);
    ikcp_nodelay(c->kcp, 1, kKcpInterval, 2, 1);
    const uint64_t now = Now();
    ikcp_update(c->kcp, uint32_t(now));  // KCP flushes nothing until updated once
    c->next_update_ms = now + kKcpInterval;
    Conn* raw = c.get();
    tombstones.erase(key);
    conns[key] = std::move(c);
    return raw;
  }

  void DestroyConn(Conn* c, bool graceful = false) {
    if (c->sock) c->sock->conn = nullptr;
    const ConnKey key = c->key;
    tombstones[key] = {Now(), graceful};
    conns.erase(key);  // releases c
    Changed();
  }

  bool GracefulTombstone(const ConnKey& key) const {
    auto it = tombstones.find(key);
    return it != tombstones.end() && it->second.graceful;
  }

  void AbortConn(Conn* c, bool send_rst) {
    if (send_rst) SendCtl(c, wire::Ctl::kRst);
    DestroyConn(c);
  }

  void Establish(Conn* c) {
    c->established = true;
    Tr("est", "vnet.established",
       "\"peer\":\"" + Ipv4ToString(c->key.ip) + "\",\"conn\":" + std::to_string(c->key.conn_id) +
           ",\"sock\":" + std::to_string(c->sock ? c->sock->id : 0));
    if (c->sock && c->sock->st == StreamState::kConnecting) c->sock->st = StreamState::kConnected;
    Changed();
  }

  // The connection died (RST, peer lost, connect timeout). A connecting socket fails with
  // `connect_err` (except set, SO_ERROR); a connected one reads/writes kConnReset; a socket still
  // waiting in an accept queue is dropped from the queue.
  void ResetConn(Conn* c, int connect_err) {
    Sock* k = c->sock;
    Tr("reset", "vnet.reset",
       "\"peer\":\"" + Ipv4ToString(c->key.ip) + "\",\"conn\":" + std::to_string(c->key.conn_id) +
           ",\"sock\":" + std::to_string(k ? k->id : 0) + ",\"established\":" +
           (c->established ? "true" : "false") + ",\"err\":" + std::to_string(connect_err));
    if (k && k->listener) {
      Sock* l = k->listener;
      DestroyConn(c);
      auto& q = l->accept_q;
      q.erase(std::remove_if(q.begin(), q.end(), [k](const auto& p) { return p.get() == k; }),
              q.end());
      return;
    }
    if (k) {
      if (k->st == StreamState::kConnecting) {
        k->st = StreamState::kFailed;
        k->so_error = connect_err;
      } else {
        k->st = StreamState::kReset;
        k->so_error = wsa::kConnReset;
      }
    }
    DestroyConn(c);
  }

  void SendFin(Conn* c) {
    c->fin_sent = true;
    c->next_fin_ms = Now() + kFinIntervalMs;
    SendCtl(c, wire::Ctl::kFin);
  }

  // Drops acknowledged messages from the in-flight list. Returns true if window space opened.
  bool UpdateAcked(Conn* c) {
    bool freed = false;
    while (!c->inflight.empty() && int32_t(c->inflight.front().last_sn - c->kcp->snd_una) < 0) {
      c->unacked_bytes -= c->inflight.front().bytes;
      c->inflight.pop_front();
      freed = true;
    }
    return freed;
  }

  // Moves complete messages from KCP into the receive queue (bounded, so a guest that stops
  // reading closes the KCP window). Returns false if `c` was destroyed.
  bool Pump(Conn* c) {
    bool got = false;
    while (c->rx_bytes < kRecvQueueBytes) {
      const int size = ikcp_peeksize(c->kcp);
      if (size < 0) break;
      std::vector<uint8_t> msg(size_t(size) + 1);  // +1: never hand KCP an empty buffer
      const int n = ikcp_recv(c->kcp, reinterpret_cast<char*>(msg.data()), int(msg.size()));
      if (n < 0) break;
      msg.resize(size_t(n));
      c->msgs_recv++;
      if (!c->sock) {
        // Data for a connection the guest already closed: reset it, as TCP does.
        AbortConn(c, true);
        return false;
      }
      if (n > 0) {
        c->rx_bytes += size_t(n);
        c->rx_total += uint64_t(n);
        CountId(c->sock->rx_ids, msg.data(), size_t(n), 2);
        TraceMsg("srx", c->sock->id, c->sock->local.port, Ep({c->peer_ip, c->peer_port}),
                 msg.data(), size_t(n), 2);
        stats.stream_bytes_recv += uint64_t(n);
        c->rx.push_back(std::move(msg));
      }
      got = true;
    }
    if (got) Changed();
    if (c->PeerFinComplete() && !c->finack_sent) {
      SendCtl(c, wire::Ctl::kFinAck);
      c->finack_sent = true;
      Changed();
    }
    return true;
  }

  // Copies up to `len` unread bytes, possibly across message boundaries.
  static size_t CopyOut(Conn* c, uint8_t* out, size_t len, bool peek) {
    size_t copied = 0;
    size_t off = c->rx_off;
    auto it = c->rx.begin();
    while (copied < len && it != c->rx.end()) {
      const size_t n = std::min(it->size() - off, len - copied);
      std::memcpy(out + copied, it->data() + off, n);
      copied += n;
      off += n;
      if (off == it->size()) {
        ++it;
        off = 0;
      }
    }
    if (!peek) {
      c->rx.erase(c->rx.begin(), it);
      c->rx_off = off;
      c->rx_bytes -= copied;
    }
    return copied;
  }

  // ---------------------------------------------------------------- inbound packets

  void DeliverDatagram(Endpoint from, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
                       size_t len) {
    auto it = dgram_ports.find(dst_port);
    if (it == dgram_ports.end()) {
      stats.dgrams_dropped++;
      Tr("drop_noport" + std::to_string(dst_port), "vnet.drop",
         "\"reason\":\"no_socket\",\"dst_port\":" + std::to_string(dst_port) +
             ",\"from\":" + Ep(from) + ",\"len\":" + std::to_string(len));
      return;
    }
    Sock* k = it->second;
    const bool addr_mismatch = k->local.ip != kInaddrAny && k->local.ip != dst_ip;
    const bool peer_mismatch = k->has_peer && (k->peer.ip != from.ip || k->peer.port != from.port);
    if (addr_mismatch || peer_mismatch || k->shut_rd || k->dgrams.size() >= kDgramQueueLimit) {
      stats.dgrams_dropped++;
      const char* why = addr_mismatch   ? "addr_mismatch"
                        : peer_mismatch ? "peer_mismatch"
                        : k->shut_rd    ? "shut_rd"
                                        : "queue_full";
      Tr(std::string("drop_") + why + std::to_string(dst_port), "vnet.drop",
         std::string("\"reason\":\"") + why + "\",\"dst_port\":" + std::to_string(dst_port) +
             ",\"from\":" + Ep(from) + ",\"queued\":" + std::to_string(k->dgrams.size()));
      return;
    }
    if (k->seen_from.insert((uint64_t(from.ip) << 16) | from.port).second) {
      Tr("first" + std::to_string(dst_port), "vnet.first_dgram",
         "\"sock\":" + std::to_string(k->id) + ",\"local\":" + Ep(k->local) +
             ",\"from\":" + Ep(from) + ",\"len\":" + std::to_string(len));
    }
    k->dgrams.push_back({from, std::vector<uint8_t>(data, data + len)});
    k->dgram_bytes += len;
    k->rx_n++;
    k->rx_b += len;
    if (k->protocol != kProtoVdp) {
      CountId(k->rx_ids, data, len, 0);
      TraceMsg("drx", k->id, k->local.port, Ep(from), data, len, 0);
    }
    stats.dgrams_recv++;
    Changed();
  }

  void OnDgramPacket(uint32_t from, const uint8_t* data, size_t len) {
    wire::DgramHeader h;
    if (!wire::DecodeDgramHeader(data, len, &h)) {
      stats.dgrams_dropped++;
      return;
    }
    const uint8_t* payload = data + wire::kDgramHeader;
    const size_t plen = len - wire::kDgramHeader;
    const uint32_t dst_ip = OwnVip();
    if (h.frag_cnt == 1) {
      DeliverDatagram({from, h.src_port}, dst_ip, h.dst_port, payload, plen);
      return;
    }
    const ReasmKey key{from, h.src_port, h.msg_id};
    auto it = reasm.find(key);
    if (it == reasm.end()) {
      if (reasm.size() >= kMaxReassemblies) {
        auto oldest = std::min_element(
            reasm.begin(), reasm.end(),
            [](const auto& a, const auto& b) { return a.second.first_ms < b.second.first_ms; });
        reasm.erase(oldest);
        stats.dgrams_dropped++;
      }
      Reasm r;
      r.dst_port = h.dst_port;
      r.frag_cnt = h.frag_cnt;
      r.first_ms = Now();
      r.parts.resize(h.frag_cnt);
      r.have.assign(h.frag_cnt, false);
      it = reasm.emplace(key, std::move(r)).first;
    }
    Reasm& r = it->second;
    if (r.frag_cnt != h.frag_cnt || r.dst_port != h.dst_port) {
      reasm.erase(it);
      stats.dgrams_dropped++;
      return;
    }
    if (r.have[h.frag_idx]) return;  // duplicate fragment
    r.have[h.frag_idx] = true;
    r.parts[h.frag_idx].assign(payload, payload + plen);
    if (++r.got < r.frag_cnt) return;
    std::vector<uint8_t> whole;
    for (const auto& part : r.parts) whole.insert(whole.end(), part.begin(), part.end());
    const uint16_t dst_port = r.dst_port;
    reasm.erase(it);
    DeliverDatagram({from, h.src_port}, dst_ip, dst_port, whole.data(), whole.size());
  }

  void OnStreamPacket(uint32_t ip, uint8_t dir, const uint8_t* data, size_t len) {
    if (!data || len < wire::kKcpOverhead) return;
    const ConnKey key{ip, wire::GetLe32(data), dir};
    auto it = conns.find(key);
    if (it == conns.end()) {
      // Unknown connection: data deserves an RST (the peer's state is stale), ACKs do not.
      if (!GracefulTombstone(key) && wire::KcpPacketHasPush(data, len)) {
        wire::CtlMsg m;
        m.type = wire::Ctl::kRst;
        m.conn_id = key.conn_id;
        SendCtlTo(ip, dir, m);
      }
      return;
    }
    Conn* c = it->second.get();
    if (!wire::KcpPacketSane(data, len, key.conn_id, c->kcp->current)) return;
    if (c->initiator && !c->established) Establish(c);  // data proves the peer accepted
    if (ikcp_input(c->kcp, reinterpret_cast<const char*>(data), long(len)) < 0) return;
    if (UpdateAcked(c)) Changed();
    Pump(c);
  }

  void OnSyn(uint32_t ip, uint8_t dir, const wire::CtlMsg& m, Conn* existing) {
    if (dir == kDirInitiator) return;
    if (existing) {
      if (!existing->initiator) SendCtl(existing, wire::Ctl::kSynAck);  // SYNACK was lost
      return;
    }
    const ConnKey key{ip, m.conn_id, dir};
    if (GracefulTombstone(key)) return;  // late duplicate SYN of a finished connection
    const uint32_t dst_ip = dir == kDirRemote ? OwnVip() : ip;
    auto pit = stream_ports.find(m.dst_port);
    Sock* l = pit == stream_ports.end() ? nullptr : pit->second;
    if (!l || l->st != StreamState::kListening || tombstones.count(key) ||
        (l->local.ip != kInaddrAny && l->local.ip != dst_ip)) {
      wire::CtlMsg rst;
      rst.type = wire::Ctl::kRst;
      rst.conn_id = m.conn_id;
      rst.src_port = m.dst_port;
      rst.dst_port = m.src_port;
      SendCtlTo(ip, dir, rst);
      return;
    }
    // Backlog full: stay silent; the client retransmits its SYN for up to 5 s.
    if (int(l->accept_q.size()) >= l->backlog) return;

    auto child = std::make_unique<Sock>();
    child->type = kSockStream;
    child->protocol = l->protocol;
    child->non_blocking = l->non_blocking;
    child->sndbuf = l->sndbuf;
    child->rcvbuf = l->rcvbuf;
    child->sndtimeo_ms = l->sndtimeo_ms;
    child->rcvtimeo_ms = l->rcvtimeo_ms;
    child->reuse_addr = l->reuse_addr;
    child->keepalive = l->keepalive;
    child->nodelay = l->nodelay;
    child->linger_onoff = l->linger_onoff;
    child->linger_s = l->linger_s;
    child->bound = true;
    child->local = {dst_ip, l->local.port};
    child->has_peer = true;
    child->peer = {ip, m.src_port};
    child->st = StreamState::kConnected;
    child->listener = l;
    Conn* c = NewConn(key, ip, l->local.port, m.src_port, dir != kDirRemote, false);
    if (!c) return;
    c->established = true;
    c->sock = child.get();
    child->conn = c;
    l->accept_q.push_back(std::move(child));
    SendCtl(c, wire::Ctl::kSynAck);
    Changed();
  }

  void OnCtlPacket(uint32_t ip, uint8_t dir, const uint8_t* data, size_t len) {
    wire::CtlMsg m;
    if (!wire::DecodeCtl(data, len, &m)) return;
    const ConnKey key{ip, m.conn_id, dir};
    auto it = conns.find(key);
    Conn* c = it == conns.end() ? nullptr : it->second.get();
    switch (m.type) {
      case wire::Ctl::kSyn:
        OnSyn(ip, dir, m, c);
        break;
      case wire::Ctl::kSynAck:
        if (c && c->initiator && !c->established) Establish(c);
        break;
      case wire::Ctl::kRst:
        if (c) ResetConn(c, wsa::kConnRefused);
        break;
      case wire::Ctl::kFin: {
        if (!c) {
          // A retransmitted FIN after we finished gets its FINACK; anything else is stale.
          wire::CtlMsg reply;
          reply.type = GracefulTombstone(key) ? wire::Ctl::kFinAck : wire::Ctl::kRst;
          reply.conn_id = m.conn_id;
          reply.src_port = m.dst_port;
          reply.dst_port = m.src_port;
          SendCtlTo(ip, dir, reply);
          break;
        }
        if (c->initiator && !c->established) Establish(c);
        if (!c->fin_recv) {
          c->fin_recv = true;
          c->fin_count = m.msg_count;
          Changed();
        }
        if (c->PeerFinComplete()) {
          SendCtl(c, wire::Ctl::kFinAck);
          c->finack_sent = true;
          Changed();
        }
        break;
      }
      case wire::Ctl::kFinAck:
        if (c && c->fin_sent && !c->fin_acked) {
          c->fin_acked = true;
          Changed();
        }
        break;
    }
  }

  // ---------------------------------------------------------------- readiness

  bool Readable(const Sock& k) const {
    if (!k.IsStream()) return !k.dgrams.empty();
    switch (k.st) {
      case StreamState::kListening:
        return !k.accept_q.empty();
      case StreamState::kConnected:
        return k.conn->rx_bytes > 0 || k.conn->Eof();
      case StreamState::kReset:
        return true;  // recv fails at once with kConnReset
      default:
        return false;
    }
  }

  bool Writable(const Sock& k) const {
    if (!k.IsStream()) return true;
    if (k.st == StreamState::kReset) return true;  // send fails at once with kConnReset
    return k.st == StreamState::kConnected && !k.shut_wr &&
           k.conn->unacked_bytes < kSendWindowBytes;
  }

  static bool Except(const Sock& k) { return k.IsStream() && k.st == StreamState::kFailed; }

  // ---------------------------------------------------------------- guest operations

  SocketId SocketLocked(int type, int protocol, int* err) {
    if (type == kSockStream) {
      if (protocol == kProtoUdp || protocol == kProtoVdp) return Fail(err, wsa::kProtoType);
      if (protocol != 0 && protocol != kProtoTcp) return Fail(err, wsa::kProtoNoSupport);
    } else if (type == kSockDgram) {
      if (protocol == kProtoTcp) return Fail(err, wsa::kProtoType);
      if (protocol != 0 && protocol != kProtoUdp && protocol != kProtoVdp)
        return Fail(err, wsa::kProtoNoSupport);
    } else {
      return Fail(err, kSockTNoSupport);
    }
    const SocketId id = AllocId();
    if (!id) return Fail(err, wsa::kNoBufs);
    auto k = std::make_unique<Sock>();
    k->id = id;
    k->gen = next_gen++;
    k->type = type;
    k->protocol = protocol ? protocol : (type == kSockStream ? kProtoTcp : kProtoUdp);
    socks[id] = std::move(k);
    *err = wsa::kOk;
    return id;
  }

  int CloseLocked(SocketId s, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    ReleasePort(k);
    if (k->IsStream()) {
      switch (k->st) {
        case StreamState::kListening:
          for (auto& child : k->accept_q) {
            if (child->conn) AbortConn(child->conn, true);
          }
          k->accept_q.clear();
          break;
        case StreamState::kConnecting:
          AbortConn(k->conn, true);
          break;
        case StreamState::kConnected: {
          Conn* c = k->conn;
          const bool unread = c->rx_bytes > 0 || ikcp_peeksize(c->kcp) >= 0;
          if (unread) {
            AbortConn(c, true);  // Winsock/TCP: closing with unread data resets
          } else {
            if (!c->fin_sent) SendFin(c);
            c->sock = nullptr;  // orphan: keeps delivering queued data and the FIN
            c->orphan_since_ms = Now();
            k->conn = nullptr;
          }
          break;
        }
        default:
          if (k->conn) DestroyConn(k->conn);
          break;
      }
    }
    socks.erase(s);
    Changed();
    *err = wsa::kOk;
    return 0;
  }

  int BindGuest(SocketId s, Endpoint local, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (k->bound || (k->IsStream() && k->st != StreamState::kIdle)) return Fail(err, wsa::kInval);
    return BindLocked(k, local, err);
  }

  int ConnectLocked(std::unique_lock<std::mutex>& lk, SocketId s, Endpoint remote, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (!k->IsStream()) {
      if (remote.ip == kInaddrAny && remote.port == 0) {  // dissolve the default destination
        k->has_peer = false;
        *err = wsa::kOk;
        return 0;
      }
      if (remote.ip == kInaddrAny || remote.port == 0) return Fail(err, wsa::kAddrNotAvail);
      if (!k->bound && BindLocked(k, {kInaddrAny, 0}, err) != 0) return -1;
      k->has_peer = true;
      k->peer = remote;
      *err = wsa::kOk;
      return 0;
    }
    switch (k->st) {
      case StreamState::kIdle:
        break;
      case StreamState::kConnecting:
        return Fail(err, wsa::kAlready);
      case StreamState::kConnected:
        return Fail(err, wsa::kIsConn);
      default:
        return Fail(err, wsa::kInval);
    }
    if (remote.ip == kInaddrAny || remote.ip == kInaddrBroadcast || remote.port == 0)
      return Fail(err, wsa::kAddrNotAvail);
    if (!k->bound && BindLocked(k, {kInaddrAny, 0}, err) != 0) return -1;

    const bool loop = IsLocalIp(remote.ip);
    const uint32_t bit = (!loop && OwnVip() < remote.ip) ? 0x80000000u : 0u;
    ConnKey key;
    do {
      key = {remote.ip, bit | (next_conn_counter++ & 0x7FFFFFFFu),
             loop ? uint8_t(kDirInitiator) : uint8_t(kDirRemote)};
    } while (conns.count(key) || tombstones.count(key));
    Conn* c = NewConn(key, remote.ip, k->local.port, remote.port, loop, true);
    if (!c) return Fail(err, wsa::kNoBufs);
    const uint64_t now = Now();
    c->connect_deadline_ms = now + kConnectTimeoutMs;
    c->next_syn_ms = now + kSynIntervalMs;
    c->sock = k;
    k->conn = c;
    k->st = StreamState::kConnecting;
    k->has_peer = true;
    k->peer = remote;
    k->so_error = 0;
    SendCtl(c, wire::Ctl::kSyn);
    Changed();
    // Non-blocking: WOULDBLOCK even when loopback completed at once (Winsock does the same).
    if (k->non_blocking) return Fail(err, wsa::kWouldBlock);
    const uint64_t gen = k->gen;
    for (;;) {
      Sock* cur = FindGen(s, gen);
      if (!cur) return Fail(err, wsa::kInterrupted);
      if (cur->st == StreamState::kConnected) {
        *err = wsa::kOk;
        return 0;
      }
      if (cur->st != StreamState::kConnecting)
        return Fail(err, cur->so_error ? cur->so_error : wsa::kConnRefused);
      WaitChange(lk, std::nullopt);
    }
  }

  int ListenLocked(SocketId s, int backlog, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (!k->IsStream()) return Fail(err, wsa::kOpNotSupp);
    if (k->st == StreamState::kConnecting || k->st == StreamState::kConnected)
      return Fail(err, wsa::kIsConn);
    if (!k->bound || (k->st != StreamState::kIdle && k->st != StreamState::kListening))
      return Fail(err, wsa::kInval);
    k->st = StreamState::kListening;
    k->backlog = std::clamp(backlog, 1, kMaxBacklog);
    Changed();
    *err = wsa::kOk;
    return 0;
  }

  SocketId AcceptLocked(std::unique_lock<std::mutex>& lk, SocketId s, Endpoint* remote, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (!k->IsStream()) return Fail(err, wsa::kOpNotSupp);
    if (k->st != StreamState::kListening) return Fail(err, wsa::kInval);
    const uint64_t gen = k->gen;
    const auto deadline = DeadlineMs(k->rcvtimeo_ms);
    bool timed_out = false;
    for (;;) {
      k = FindGen(s, gen);
      if (!k || k->st != StreamState::kListening) return Fail(err, wsa::kInterrupted);
      if (!k->accept_q.empty()) {
        const SocketId id = AllocId();
        if (!id) return Fail(err, wsa::kNoBufs);
        std::unique_ptr<Sock> child = std::move(k->accept_q.front());
        k->accept_q.pop_front();
        child->id = id;
        child->gen = next_gen++;
        child->listener = nullptr;
        if (remote) *remote = child->peer;
        socks[id] = std::move(child);
        Changed();
        *err = wsa::kOk;
        return id;
      }
      if (k->non_blocking) return Fail(err, wsa::kWouldBlock);
      if (timed_out) return Fail(err, wsa::kTimedOut);
      timed_out = !WaitChange(lk, deadline);
    }
  }

  // Queues one guest send() as one KCP message.
  bool QueueMessage(Conn* c, const void* data, size_t n) {
    ikcpcb* kcp = c->kcp;
    const uint32_t first_sn = kcp->snd_nxt + kcp->nsnd_que;  // queued segments get sn in order
    const uint32_t before = kcp->nsnd_que;
    if (ikcp_send(kcp, static_cast<const char*>(data), int(n)) < 0) return false;
    const uint32_t segs = kcp->nsnd_que - before;
    c->inflight.push_back({first_sn + segs - 1, uint32_t(n)});
    c->unacked_bytes += n;
    c->msgs_sent++;
    c->tx_bytes += n;
    if (c->sock) {
      CountId(c->sock->tx_ids, static_cast<const uint8_t*>(data), n, 2);
      TraceMsg("stx", c->sock->id, c->sock->local.port, Ep({c->peer_ip, c->peer_port}),
               static_cast<const uint8_t*>(data), n, 2);
    }
    stats.stream_bytes_sent += n;
    ikcp_flush(kcp);
    return true;
  }

  int SendStream(std::unique_lock<std::mutex>& lk, SocketId s, const void* data, size_t len,
                 int flags, int* err) {
    if (flags & kMsgOob) return Fail(err, wsa::kOpNotSupp);
    if (len > 0 && !data) return Fail(err, wsa::kFault);
    Sock* k = Find(s);
    const uint64_t gen = k->gen;
    const auto deadline = DeadlineMs(k->sndtimeo_ms);
    bool timed_out = false;
    for (;;) {
      k = FindGen(s, gen);
      if (!k) return Fail(err, wsa::kInterrupted);
      switch (k->st) {
        case StreamState::kConnecting:  // would block until connected (see lane notes)
        case StreamState::kConnected:
          break;
        case StreamState::kReset:
          return Fail(err, wsa::kConnReset);
        default:
          return Fail(err, wsa::kNotConn);
      }
      if (k->shut_wr) return Fail(err, wsa::kShutdown);
      if (k->st == StreamState::kConnected) {
        if (len == 0) {
          *err = wsa::kOk;
          return 0;
        }
        Conn* c = k->conn;
        if (c->unacked_bytes < kSendWindowBytes) {
          const size_t n = std::min(len, kMaxSendCall);
          if (!QueueMessage(c, data, n)) return Fail(err, wsa::kNoBufs);
          *err = wsa::kOk;
          return int(n);
        }
      }
      if (k->non_blocking) return Fail(err, wsa::kWouldBlock);
      if (timed_out) return Fail(err, wsa::kTimedOut);
      timed_out = !WaitChange(lk, deadline);
    }
  }

  int RecvStream(std::unique_lock<std::mutex>& lk, SocketId s, void* data, size_t len, int flags,
                 Endpoint* from, int* err) {
    if (flags & ~kMsgPeek) return Fail(err, wsa::kOpNotSupp);
    if (len > 0 && !data) return Fail(err, wsa::kFault);
    Sock* k = Find(s);
    const uint64_t gen = k->gen;
    const auto deadline = DeadlineMs(k->rcvtimeo_ms);
    bool timed_out = false;
    for (;;) {
      k = FindGen(s, gen);
      if (!k) return Fail(err, wsa::kInterrupted);
      if (k->st == StreamState::kReset) return Fail(err, wsa::kConnReset);
      if (k->st != StreamState::kConnected) return Fail(err, wsa::kNotConn);
      if (k->shut_rd) return Fail(err, wsa::kShutdown);
      Conn* c = k->conn;
      if (len == 0) {
        *err = wsa::kOk;
        return 0;
      }
      if (c->rx_bytes > 0) {
        const bool peek = flags & kMsgPeek;
        const size_t n = CopyOut(c, static_cast<uint8_t*>(data), len, peek);
        if (from) *from = k->peer;
        if (!peek) Pump(c);  // room freed: pull more messages out of KCP
        *err = wsa::kOk;
        return int(n);
      }
      if (c->Eof()) {
        if (from) *from = k->peer;
        *err = wsa::kOk;
        return 0;
      }
      if (k->non_blocking) return Fail(err, wsa::kWouldBlock);
      if (timed_out) return Fail(err, wsa::kTimedOut);
      timed_out = !WaitChange(lk, deadline);
    }
  }

  int SendDgram(Sock* k, const void* data, size_t len, int flags, Endpoint to, int* err) {
    if (flags & kMsgOob) return Fail(err, wsa::kOpNotSupp);
    if (len > 0 && !data) return Fail(err, wsa::kFault);
    if (k->shut_wr) return Fail(err, wsa::kShutdown);
    if (to.ip == kInaddrBroadcast) {
      // Only AC6's LAN debug harness broadcasts; it is accepted and dropped (counted).
      if (!k->broadcast) return Fail(err, wsa::kAccess);
      if (!k->bound && BindLocked(k, {kInaddrAny, 0}, err) != 0) return -1;
      stats.dgrams_dropped++;
      *err = wsa::kOk;
      return int(len);
    }
    if (to.ip == kInaddrAny || to.port == 0) return Fail(err, wsa::kAddrNotAvail);
    const size_t per = FragPayload();
    if (per == 0) return Fail(err, wsa::kNetDown);
    if (len > per * wire::kMaxFragments) return Fail(err, wsa::kMsgSize);
    if (!k->bound && BindLocked(k, {kInaddrAny, 0}, err) != 0) return -1;
    const auto* p = static_cast<const uint8_t*>(data);
    const uint16_t msg_id = k->next_msg_id++;
    stats.dgrams_sent++;
    k->tx_n++;
    k->tx_b += len;
    if (k->protocol != kProtoVdp) {
      CountId(k->tx_ids, p, len, 0);
      TraceMsg("dtx", k->id, k->local.port, Ep(to), p, len, 0);
    }
    if (IsLocalIp(to.ip)) {
      // The receiver sees the destination address the sender used as the source.
      DeliverDatagram({to.ip, k->local.port}, to.ip, to.port, p, len);
      *err = wsa::kOk;
      return int(len);
    }
    const size_t cnt = len == 0 ? 1 : (len + per - 1) / per;
    std::vector<uint8_t> buf(wire::kDgramHeader + std::min(len, per));
    bool ok = true;
    for (size_t i = 0; i < cnt; ++i) {
      const size_t off = i * per;
      const size_t n = std::min(per, len - off);
      wire::DgramHeader h;
      h.proto = k->protocol == kProtoVdp ? wire::kProtoVdp : wire::kProtoUdp;
      h.src_port = k->local.port;
      h.dst_port = to.port;
      h.msg_id = msg_id;
      h.frag_idx = uint8_t(i);
      h.frag_cnt = uint8_t(cnt);
      wire::EncodeDgramHeader(h, buf.data());
      if (n) std::memcpy(buf.data() + wire::kDgramHeader, p + off, n);
      ok = link->Send(to.ip, Channel::kDatagram, buf.data(), wire::kDgramHeader + n) && ok;
    }
    if (!ok) {
      stats.dgrams_dropped++;
      Tr("send_drop" + std::to_string(to.port), "vnet.drop",
         "\"reason\":\"link_send\",\"to\":" + Ep(to) + ",\"len\":" + std::to_string(len));
    }
    *err = wsa::kOk;
    return int(len);
  }

  int RecvDgram(std::unique_lock<std::mutex>& lk, SocketId s, void* data, size_t len, int flags,
                Endpoint* from, int* err) {
    if (flags & ~kMsgPeek) return Fail(err, wsa::kOpNotSupp);
    Sock* k = Find(s);
    if (!k->bound) return Fail(err, wsa::kInval);
    const uint64_t gen = k->gen;
    const auto deadline = DeadlineMs(k->rcvtimeo_ms);
    bool timed_out = false;
    for (;;) {
      k = FindGen(s, gen);
      if (!k) return Fail(err, wsa::kInterrupted);
      if (k->shut_rd) return Fail(err, wsa::kShutdown);
      if (!k->dgrams.empty()) {
        Datagram& d = k->dgrams.front();
        const size_t full = d.data.size();
        const size_t n = std::min(len, full);
        if (n > 0) {
          if (!data) return Fail(err, wsa::kFault);
          std::memcpy(data, d.data.data(), n);
        }
        if (from) *from = d.from;
        if (!(flags & kMsgPeek)) {
          k->dgram_bytes -= full;
          k->dgrams.pop_front();
        }
        // Winsock: a short buffer gets the head of the datagram; the rest is discarded.
        if (full > len) return Fail(err, wsa::kMsgSize);
        *err = wsa::kOk;
        return int(n);
      }
      if (k->non_blocking) return Fail(err, wsa::kWouldBlock);
      if (timed_out) return Fail(err, wsa::kTimedOut);
      timed_out = !WaitChange(lk, deadline);
    }
  }

  int ShutdownLocked(SocketId s, int how, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (how != kSdReceive && how != kSdSend && how != kSdBoth) return Fail(err, wsa::kInval);
    if (k->IsStream()) {
      if (k->st != StreamState::kConnected) return Fail(err, wsa::kNotConn);
      if (how != kSdReceive && !k->shut_wr) {
        k->shut_wr = true;
        if (!k->conn->fin_sent) SendFin(k->conn);
      }
    } else if (how != kSdReceive) {
      k->shut_wr = true;
    }
    if (how != kSdSend) k->shut_rd = true;
    Changed();
    *err = wsa::kOk;
    return 0;
  }

  // The glue hands integer option values over host-endian in the guest's optlen: 1, 2 or 8
  // bytes are read at that width, 4..7 bytes as an int32; anything shorter is a fault.
  static bool ReadOptionInt(const void* value, size_t len, int64_t* out) {
    if (!value) return false;
    if (len == 1) {
      uint8_t v;
      std::memcpy(&v, value, 1);
      *out = v;
    } else if (len == 2) {
      uint16_t v;
      std::memcpy(&v, value, 2);
      *out = v;
    } else if (len == 8) {
      int64_t v;
      std::memcpy(&v, value, 8);
      *out = v;
    } else if (len >= 4) {
      int32_t v;
      std::memcpy(&v, value, 4);
      *out = v;
    } else {
      return false;
    }
    return true;
  }

  int SetOptionLocked(SocketId s, int level, int option, const void* value, size_t len, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    int64_t v = 0;
    const bool is_linger = level == kSolSocket && option == kSoLinger;
    if (is_linger ? (!value || len < 2 * sizeof(uint16_t)) : !ReadOptionInt(value, len, &v))
      return Fail(err, wsa::kFault);
    if (level == kSolSocket) {
      switch (option) {
        case kSoReuseAddr:  // stored only: ports stay exclusive (see BindLocked)
          k->reuse_addr = v != 0;
          break;
        case kSoKeepAlive:  // stored only: dead peers are detected by the overlay (15 s)
          k->keepalive = v != 0;
          break;
        case kSoBroadcast:
          k->broadcast = v != 0;
          break;
        case kSoLinger: {  // stored only; struct linger { u16 l_onoff; u16 l_linger; }
          uint16_t f[2];
          std::memcpy(f, value, sizeof(f));
          k->linger_onoff = f[0];
          k->linger_s = f[1];
          break;
        }
        case kSoSndBuf:
        case kSoRcvBuf:  // stored and reported; vnet's own limits apply
          if (v < 0 || v > INT32_MAX) return Fail(err, wsa::kInval);
          (option == kSoSndBuf ? k->sndbuf : k->rcvbuf) = int(v);
          break;
        case kSoSndTimeo:
        case kSoRcvTimeo:  // DWORD milliseconds; 0 = no timeout
          (option == kSoSndTimeo ? k->sndtimeo_ms : k->rcvtimeo_ms) = uint32_t(v);
          break;
        default:  // includes the read-only SO_TYPE and SO_ERROR
          return Fail(err, wsa::kNoProtoOpt);
      }
    } else if (level == kIpprotoTcp) {
      if (!k->IsStream() || option != kTcpNoDelay) return Fail(err, wsa::kNoProtoOpt);
      k->nodelay = v != 0;  // KCP already runs in no-delay mode
    } else {
      return Fail(err, wsa::kInval);
    }
    *err = wsa::kOk;
    return 0;
  }

  int GetOptionLocked(SocketId s, int level, int option, void* value, size_t* len, int* err) {
    Sock* k = Lookup(s, err);
    if (!k) return -1;
    if (!value || !len || *len < sizeof(int32_t)) return Fail(err, wsa::kFault);
    int32_t v = 0;
    if (level == kSolSocket) {
      switch (option) {
        case kSoType:
          v = k->type;
          break;
        case kSoError:  // read and clear (Winsock)
          v = k->so_error;
          k->so_error = 0;
          break;
        case kSoReuseAddr:
          v = k->reuse_addr;
          break;
        case kSoKeepAlive:
          v = k->keepalive;
          break;
        case kSoBroadcast:
          v = k->broadcast;
          break;
        case kSoLinger: {
          const uint16_t f[2] = {k->linger_onoff, k->linger_s};
          std::memcpy(value, f, sizeof(f));
          *len = sizeof(f);
          *err = wsa::kOk;
          return 0;
        }
        case kSoSndBuf:
          v = k->sndbuf;
          break;
        case kSoRcvBuf:
          v = k->rcvbuf;
          break;
        case kSoSndTimeo:
          v = int32_t(k->sndtimeo_ms);
          break;
        case kSoRcvTimeo:
          v = int32_t(k->rcvtimeo_ms);
          break;
        default:
          return Fail(err, wsa::kNoProtoOpt);
      }
    } else if (level == kIpprotoTcp) {
      if (!k->IsStream() || option != kTcpNoDelay) return Fail(err, wsa::kNoProtoOpt);
      v = k->nodelay;
    } else {
      return Fail(err, wsa::kInval);
    }
    std::memcpy(value, &v, sizeof(v));
    *len = sizeof(v);
    *err = wsa::kOk;
    return 0;
  }

  int SelectLocked(std::unique_lock<std::mutex>& lk, SelectSets* sets, int64_t timeout_us,
                   int* err) {
    if (!sets) return Fail(err, wsa::kFault);
    // Winsock rejects a select with no sockets at all.
    if (sets->read.empty() && sets->write.empty() && sets->except.empty()) {
      return Fail(err, wsa::kInval);
    }
    std::optional<Clock::time_point> deadline;
    if (timeout_us >= 0) {
      deadline =
          Clock::now() + std::chrono::microseconds(std::min(timeout_us, kMaxSelectTimeoutUs));
    }
    bool timed_out = false;
    for (;;) {
      for (const auto* set : {&sets->read, &sets->write, &sets->except}) {
        for (SocketId id : *set) {
          if (!Find(id)) return Fail(err, wsa::kNotSock);
        }
      }
      SelectSets ready;
      auto collect = [this](const std::vector<SocketId>& in, std::vector<SocketId>& out,
                            auto&& pred) {
        for (SocketId id : in) {
          if (pred(*Find(id)) && std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(id);
          }
        }
      };
      collect(sets->read, ready.read, [this](const Sock& k) { return Readable(k); });
      collect(sets->write, ready.write, [this](const Sock& k) { return Writable(k); });
      collect(sets->except, ready.except, [](const Sock& k) { return Except(k); });
      const int n = int(ready.read.size() + ready.write.size() + ready.except.size());
      if (n > 0 || timeout_us == 0 || timed_out) {
        *sets = std::move(ready);
        *err = wsa::kOk;
        return n;
      }
      timed_out = !WaitChange(lk, deadline);
    }
  }

  // ---------------------------------------------------------------- network thread

  void TickConn(Conn* c, uint64_t now) {
    const uint32_t now32 = uint32_t(now);
    if (c->initiator && !c->established) {
      if (now >= c->connect_deadline_ms) {
        SendCtl(c, wire::Ctl::kRst);  // in case the SYN arrived and only the SYNACKs were lost
        ResetConn(c, wsa::kTimedOut);
        return;
      }
      if (now >= c->next_syn_ms) {
        SendCtl(c, wire::Ctl::kSyn);
        c->next_syn_ms = now + kSynIntervalMs;
      }
      return;
    }
    if (now >= c->next_update_ms) {
      ikcp_update(c->kcp, now32);
      c->next_update_ms = now + uint32_t(ikcp_check(c->kcp, now32) - now32);
    }
    if (c->fin_sent && !c->fin_acked && now >= c->next_fin_ms) {
      SendCtl(c, wire::Ctl::kFin);
      c->next_fin_ms = now + kFinIntervalMs;
    }
    if (!c->sock) {
      // Orphan. FINACK means the peer received every message we sent; once its own FIN is
      // complete too, linger briefly to ACK retransmissions, then go.
      if (c->fin_acked && c->PeerFinComplete()) {
        if (!c->time_wait_since_ms) {
          c->time_wait_since_ms = now;
        } else if (Elapsed(now, c->time_wait_since_ms) >= kTimeWaitMs) {
          DestroyConn(c, /*graceful=*/true);
        }
      } else if (Elapsed(now, c->orphan_since_ms) >= kOrphanLingerMs) {
        // Give up. If our data was all delivered, go quietly (TCP's FIN_WAIT_2 timeout; later
        // data from the peer draws an RST); otherwise tell the peer now.
        AbortConn(c, /*send_rst=*/!c->fin_acked);
      }
    }
  }

  uint64_t last_stats_ms = 0;
  std::string last_stats;

  // Every 5 s while tracing: one "vnet.stats" line with per-socket traffic totals, written only
  // when something changed.
  void TraceStats(uint64_t now) {
    if (!TraceEnabled() || Elapsed(now, last_stats_ms) < 5000) return;
    last_stats_ms = now;
    std::string socks_json;
    for (const auto& [id, k] : socks) {
      if (!socks_json.empty()) socks_json += ',';
      socks_json += "{\"id\":" + std::to_string(id) + ",\"t\":" + std::to_string(k->type) +
                    ",\"port\":" + std::to_string(k->local.port);
      if (k->IsStream()) {
        if (k->conn) {
          const Conn* c = k->conn;
          socks_json += ",\"peer\":\"" + Ipv4ToString(c->key.ip) +
                        "\",\"msgs_tx\":" + std::to_string(c->msgs_sent) +
                        ",\"bytes_tx\":" + std::to_string(c->tx_bytes) +
                        ",\"msgs_rx\":" + std::to_string(c->msgs_recv) +
                        ",\"bytes_rx\":" + std::to_string(c->rx_total) +
                        ",\"unread\":" + std::to_string(c->rx_bytes) +
                        ",\"unacked\":" + std::to_string(c->unacked_bytes) +
                        ",\"ids_tx\":" + Hist(k->tx_ids) + ",\"ids_rx\":" + Hist(k->rx_ids);
        } else {
          socks_json += ",\"state\":" + std::to_string(int(k->st)) +
                        ",\"accept_q\":" + std::to_string(k->accept_q.size());
        }
      } else {
        socks_json += ",\"tx\":" + std::to_string(k->tx_n) + ",\"tx_b\":" +
                      std::to_string(k->tx_b) + ",\"rx\":" + std::to_string(k->rx_n) +
                      ",\"rx_b\":" + std::to_string(k->rx_b) + ",\"queued\":" +
                      std::to_string(k->dgrams.size()) + ",\"ids_tx\":" + Hist(k->tx_ids) +
                      ",\"ids_rx\":" + Hist(k->rx_ids);
      }
      socks_json += '}';
    }
    if (socks_json == last_stats) return;
    last_stats = socks_json;
    Trace("vnet.stats", "\"socks\":[" + socks_json + "],\"dropped\":" +
                            std::to_string(stats.dgrams_dropped));
  }

  void TickLocked(uint64_t now) {
    TraceStats(now);
    for (auto it = conns.begin(); it != conns.end();) {
      Conn* c = it->second.get();
      ++it;  // TickConn may destroy c (and only c)
      TickConn(c, now);
    }
    for (auto it = reasm.begin(); it != reasm.end();) {
      if (Elapsed(now, it->second.first_ms) >= kReassemblyTimeoutMs) {
        stats.dgrams_dropped++;
        it = reasm.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = tombstones.begin(); it != tombstones.end();) {
      it = Elapsed(now, it->second.since_ms) >= kTombstoneMs ? tombstones.erase(it) : std::next(it);
    }
  }

  void PeerLostLocked(uint32_t vip) {
    for (auto it = conns.begin(); it != conns.end();) {
      Conn* c = it->second.get();
      ++it;
      if (c->key.dir == kDirRemote && c->key.ip == vip) ResetConn(c, wsa::kTimedOut);
    }
    for (auto it = reasm.begin(); it != reasm.end();) {
      it = it->first.ip == vip ? reasm.erase(it) : std::next(it);
    }
  }

  void CloseAllLocked() {
    while (!conns.empty()) AbortConn(conns.begin()->second.get(), true);
    socks.clear();
    dgram_ports.clear();
    stream_ports.clear();
    reasm.clear();
    Changed();
  }
};

// -------------------------------------------------------------------- public API

VNet::VNet(PeerLink* link) : impl_(std::make_unique<Impl>(link)) {}

// No packets are sent from the destructor (the link may already be shutting down); call
// CloseAll() first to reset peers. Guest threads must not be blocked inside vnet.
VNet::~VNet() = default;

SocketId VNet::Socket(int type, int protocol, int* err) {
  Impl::Guard g(*impl_);
  return impl_->SocketLocked(type, protocol, err);
}

int VNet::Close(SocketId s, int* err) {
  Impl::Guard g(*impl_);
  return impl_->CloseLocked(s, err);
}

int VNet::Bind(SocketId s, Endpoint local, int* err) {
  Impl::Guard g(*impl_);
  return impl_->BindGuest(s, local, err);
}

int VNet::Connect(SocketId s, Endpoint remote, int* err) {
  Impl::Guard g(*impl_);
  return impl_->ConnectLocked(g.lk, s, remote, err);
}

int VNet::Listen(SocketId s, int backlog, int* err) {
  Impl::Guard g(*impl_);
  return impl_->ListenLocked(s, backlog, err);
}

SocketId VNet::Accept(SocketId s, Endpoint* remote, int* err) {
  Impl::Guard g(*impl_);
  return impl_->AcceptLocked(g.lk, s, remote, err);
}

int VNet::Send(SocketId s, const void* data, size_t len, int flags, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (k->IsStream()) return impl_->SendStream(g.lk, s, data, len, flags, err);
  if (!k->has_peer) return Fail(err, wsa::kNotConn);
  return impl_->SendDgram(k, data, len, flags, k->peer, err);
}

int VNet::Recv(SocketId s, void* data, size_t len, int flags, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (k->IsStream()) return impl_->RecvStream(g.lk, s, data, len, flags, nullptr, err);
  return impl_->RecvDgram(g.lk, s, data, len, flags, nullptr, err);
}

int VNet::SendTo(SocketId s, const void* data, size_t len, int flags, Endpoint to, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  // Winsock ignores the address for connection-oriented sockets.
  if (k->IsStream()) return impl_->SendStream(g.lk, s, data, len, flags, err);
  return impl_->SendDgram(k, data, len, flags, to, err);
}

int VNet::RecvFrom(SocketId s, void* data, size_t len, int flags, Endpoint* from, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (k->IsStream()) return impl_->RecvStream(g.lk, s, data, len, flags, from, err);
  return impl_->RecvDgram(g.lk, s, data, len, flags, from, err);
}

int VNet::Shutdown(SocketId s, int how, int* err) {
  Impl::Guard g(*impl_);
  return impl_->ShutdownLocked(s, how, err);
}

int VNet::SetNonBlocking(SocketId s, bool non_blocking, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  k->non_blocking = non_blocking;
  *err = wsa::kOk;
  return 0;
}

int VNet::Available(SocketId s, uint32_t* bytes, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (!bytes) return Fail(err, wsa::kFault);
  size_t n = 0;
  if (!k->IsStream()) {
    n = k->dgram_bytes;  // Winsock FIONREAD: all queued datagram bytes
  } else if (k->st == StreamState::kConnected) {
    n = k->conn->rx_bytes;
  }
  *bytes = uint32_t(std::min<size_t>(n, UINT32_MAX));
  *err = wsa::kOk;
  return 0;
}

int VNet::SetOption(SocketId s, int level, int option, const void* value, size_t len, int* err) {
  Impl::Guard g(*impl_);
  return impl_->SetOptionLocked(s, level, option, value, len, err);
}

int VNet::GetOption(SocketId s, int level, int option, void* value, size_t* len, int* err) {
  Impl::Guard g(*impl_);
  return impl_->GetOptionLocked(s, level, option, value, len, err);
}

int VNet::GetSockName(SocketId s, Endpoint* local, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (!local) return Fail(err, wsa::kFault);
  if (!k->bound) return Fail(err, wsa::kInval);
  Endpoint e = k->local;
  // A connected stream reports the address it actually uses; datagram sockets keep 0.0.0.0.
  if (k->IsStream() && e.ip == kInaddrAny && k->conn) {
    e.ip = k->conn->loopback ? k->conn->peer_ip : impl_->OwnVip();
  }
  *local = e;
  *err = wsa::kOk;
  return 0;
}

int VNet::GetPeerName(SocketId s, Endpoint* remote, int* err) {
  Impl::Guard g(*impl_);
  Impl::Sock* k = impl_->Lookup(s, err);
  if (!k) return -1;
  if (!remote) return Fail(err, wsa::kFault);
  const bool connected = k->IsStream() ? k->st == StreamState::kConnected : k->has_peer;
  if (!connected) return Fail(err, wsa::kNotConn);
  *remote = k->peer;
  *err = wsa::kOk;
  return 0;
}

int VNet::Select(SelectSets* sets, int64_t timeout_us, int* err) {
  Impl::Guard g(*impl_);
  return impl_->SelectLocked(g.lk, sets, timeout_us, err);
}

bool VNet::IsSocket(SocketId s) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->Find(s) != nullptr;
}

void VNet::OnLinkData(uint32_t from_vip, uint8_t channel, const uint8_t* data, size_t len) {
  Impl::Guard g(*impl_);
  if (!data && len) return;
  switch (Channel(channel)) {
    case Channel::kDatagram:
      impl_->OnDgramPacket(from_vip, data, len);
      break;
    case Channel::kStream:
      impl_->OnStreamPacket(from_vip, kDirRemote, data, len);
      break;
    case Channel::kStreamCtl:
      impl_->OnCtlPacket(from_vip, kDirRemote, data, len);
      break;
    default:
      break;
  }
}

void VNet::OnPeerLost(uint32_t vip) {
  Impl::Guard g(*impl_);
  impl_->PeerLostLocked(vip);
}

void VNet::Tick(uint64_t now_ms) {
  Impl::Guard g(*impl_);
  impl_->TickLocked(now_ms);
}

Stats VNet::stats() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  Stats s = impl_->stats;
  s.sockets_open = uint32_t(impl_->socks.size());
  s.streams_open = uint32_t(impl_->conns.size());
  return s;
}

void VNet::CloseAll() {
  Impl::Guard g(*impl_);
  impl_->CloseAllLocked();
}

}  // namespace ac6net::vnet
