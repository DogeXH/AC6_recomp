// ac6net - in-band QoS probes for XNetQosListen / XNetQosLookup (overlay channel 0x04).
//
// Wire format (PROTOCOL.md 2.8, little-endian):
//   REQ  = type u8 (1), xnkid 8 bytes (guest order), seq u32, t_us u64
//   RESP = type u8 (2), xnkid, seq, t_us (echoed), status u8 (0 ok, 1 not listening),
//          data_len u16, data
//
// Callbacks run only on the network thread (OnLinkData / OnPeerLost / Tick), never while the
// service mutex is held. Cancel() returns only after any callback batch that is running has
// finished, so the glue may free its XNQOS right after Cancel().
#include "qos.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "vnet_wire.h"

namespace ac6net {

namespace {

constexpr uint8_t kTypeReq = 1;
constexpr uint8_t kTypeResp = 2;
constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusNotListening = 1;
constexpr size_t kReqSize = 1 + 8 + 4 + 8;
constexpr size_t kRespHeader = kReqSize + 1 + 2;

// XNET_QOS_LISTEN_* flags.
constexpr uint32_t kFlagEnable = 0x01;
constexpr uint32_t kFlagDisable = 0x02;
constexpr uint32_t kFlagSetData = 0x04;
constexpr uint32_t kFlagSetBps = 0x08;
constexpr uint32_t kFlagRelease = 0x10;

constexpr uint64_t kProbeIntervalMs = 50;
constexpr uint64_t kTargetTimeoutMs = 2000;  // after the last probe
constexpr uint32_t kNominalBps = 10'000'000;
constexpr uint32_t kMaxProbes = 64;

uint64_t KidKey(const XnKid& kid) {
  uint64_t v = 0;
  for (uint8_t b : kid.b) v = (v << 8) | b;
  return v;
}

uint64_t Elapsed(uint64_t now, uint64_t since) { return now > since ? now - since : 0; }

}  // namespace

struct QosService::Impl {
  struct Listener {
    std::vector<uint8_t> data;
    uint32_t bps = 0;
    bool listening = false;
    bool disabled = false;  // DISABLE seen and no ENABLE since
  };

  struct Target {
    uint32_t vip = 0;
    XnKid kid;
    QosTargetResult res;
    std::vector<uint16_t> rtts;
    uint64_t next_probe_ms = 0;
    uint64_t last_probe_ms = 0;
    bool dirty = false;  // changed since the last callback
  };

  struct Lookup {
    QosCallback cb;
    uint32_t probes = 1;
    std::vector<Target> targets;
  };

  struct Probe {
    uint64_t lookup = 0;
    size_t target = 0;
    uint64_t sent_ms = 0;
  };

  struct LoopPacket {
    uint32_t from;
    std::vector<uint8_t> data;
  };

  struct PendingCall {
    uint64_t lookup;
    QosCallback cb;
    size_t index;
    QosTargetResult res;
    bool all_done;
  };

  explicit Impl(PeerLink* l) : link(l) {}

  PeerLink* link;
  std::mutex mu;
  std::mutex cb_mu;           // held by the network thread while it runs a callback batch
  std::thread::id cb_thread;  // thread running a batch (guarded by mu)
  std::set<uint64_t> cancelled_in_batch;  // Cancel() called from inside a callback
  std::map<uint64_t, Listener> listeners;
  std::map<uint64_t, Lookup> lookups;
  std::map<uint32_t, Probe> probes;
  std::deque<LoopPacket> loop_q;
  uint64_t next_lookup = 1;
  uint32_t next_seq = 1;

  bool IsLocal(uint32_t vip) const {
    const uint32_t own = link->LocalVip();
    return vip == kLoopback || (own != 0 && vip == own);
  }

  void Transmit(uint32_t vip, const uint8_t* data, size_t len) {
    if (IsLocal(vip)) {
      loop_q.push_back({vip, std::vector<uint8_t>(data, data + len)});
    } else {
      link->Send(vip, Channel::kQos, data, len);
    }
  }

  void SendProbe(uint64_t lookup_id, size_t index, Target& t, uint64_t now) {
    const uint32_t seq = next_seq++;
    probes[seq] = {lookup_id, index, now};
    uint8_t buf[kReqSize];
    buf[0] = kTypeReq;
    std::memcpy(buf + 1, t.kid.b.data(), 8);
    vnet::wire::PutLe32(buf + 9, seq);
    vnet::wire::PutLe64(buf + 13, now * 1000);
    Transmit(t.vip, buf, sizeof(buf));
    t.res.probes_sent++;
    t.last_probe_ms = now;
    t.next_probe_ms = now + kProbeIntervalMs;
  }

  static void Complete(Target& t) {
    t.res.complete = true;
    t.dirty = true;
  }

  void OnReq(uint32_t from, const uint8_t* data, size_t len) {
    if (len < kReqSize) return;
    XnKid kid;
    std::memcpy(kid.b.data(), data + 1, 8);
    auto it = listeners.find(KidKey(kid));
    const bool ok = it != listeners.end() && it->second.listening;
    const size_t dlen = ok ? it->second.data.size() : 0;
    std::vector<uint8_t> resp(kRespHeader + dlen);
    std::memcpy(resp.data(), data, kReqSize);  // echo kid, seq and t_us
    resp[0] = kTypeResp;
    resp[kReqSize] = ok ? kStatusOk : kStatusNotListening;
    vnet::wire::PutLe16(resp.data() + kReqSize + 1, uint16_t(dlen));
    if (dlen) std::memcpy(resp.data() + kRespHeader, it->second.data.data(), dlen);
    Transmit(from, resp.data(), resp.size());
  }

  void OnResp(uint32_t from, const uint8_t* data, size_t len, uint64_t now) {
    if (len < kRespHeader) return;
    const uint8_t status = data[kReqSize];
    const size_t dlen = vnet::wire::GetLe16(data + kReqSize + 1);
    if (dlen > len - kRespHeader) return;
    if (status != kStatusOk && status != kStatusNotListening) return;
    auto pit = probes.find(vnet::wire::GetLe32(data + 9));
    if (pit == probes.end()) return;  // unknown, duplicate or cancelled
    const Probe probe = pit->second;
    auto lit = lookups.find(probe.lookup);
    if (lit == lookups.end() || probe.target >= lit->second.targets.size()) return;
    Lookup& l = lit->second;
    Target& t = l.targets[probe.target];
    if (t.vip != from || std::memcmp(t.kid.b.data(), data + 1, 8) != 0) return;
    probes.erase(pit);
    if (t.res.complete) return;

    const uint64_t rtt = std::clamp<uint64_t>(Elapsed(now, probe.sent_ms), 1, 0xFFFF);
    t.rtts.push_back(uint16_t(rtt));
    std::vector<uint16_t> sorted = t.rtts;
    std::sort(sorted.begin(), sorted.end());
    t.res.rtt_min_ms = sorted.front();
    t.res.rtt_med_ms = sorted[(sorted.size() - 1) / 2];
    t.res.probes_recv++;
    t.res.contacted = true;
    t.res.up_bps = kNominalBps;
    t.res.dn_bps = kNominalBps;
    if (status == kStatusOk && dlen > 0 && !t.res.data_received) {
      t.res.data_received = true;
      t.res.data.assign(data + kRespHeader, data + kRespHeader + dlen);
    }
    t.dirty = true;
    if (t.res.probes_recv >= l.probes) Complete(t);
  }

  void OnPacket(uint32_t from, const uint8_t* data, size_t len, uint64_t now) {
    if (!data || len < 1) return;
    if (data[0] == kTypeReq) {
      OnReq(from, data, len);
    } else if (data[0] == kTypeResp) {
      OnResp(from, data, len, now);
    }
  }

  void DrainLoop(uint64_t now) {
    // A local REQ produces a local RESP; both are handled in this pass.
    while (!loop_q.empty()) {
      LoopPacket p = std::move(loop_q.front());
      loop_q.pop_front();
      OnPacket(p.from, p.data.data(), p.data.size(), now);
    }
  }

  void Advance(uint64_t now) {
    for (auto& [id, l] : lookups) {
      for (size_t i = 0; i < l.targets.size(); ++i) {
        Target& t = l.targets[i];
        if (t.res.complete) continue;
        if (t.res.probes_sent < l.probes) {
          if (now >= t.next_probe_ms) SendProbe(id, i, t, now);
        } else if (Elapsed(now, t.last_probe_ms) >= kTargetTimeoutMs) {
          Complete(t);
        }
      }
    }
  }

  void ErasePendingProbes(uint64_t lookup_id) {
    for (auto it = probes.begin(); it != probes.end();) {
      it = it->second.lookup == lookup_id ? probes.erase(it) : std::next(it);
    }
  }

  // One call per changed target; the last call of a finished lookup carries all_done.
  void Collect(std::vector<PendingCall>* calls) {
    for (auto it = lookups.begin(); it != lookups.end();) {
      Lookup& l = it->second;
      const size_t first = calls->size();
      bool all_complete = true;
      for (size_t i = 0; i < l.targets.size(); ++i) {
        Target& t = l.targets[i];
        all_complete = all_complete && t.res.complete;
        if (t.dirty) {
          calls->push_back({it->first, l.cb, i, t.res, false});
          t.dirty = false;
        }
      }
      if (!all_complete) {
        ++it;
        continue;
      }
      if (calls->size() > first) {
        calls->back().all_done = true;
      } else {
        const size_t last = l.targets.size() - 1;
        calls->push_back({it->first, l.cb, last, l.targets[last].res, true});
      }
      ErasePendingProbes(it->first);
      it = lookups.erase(it);
    }
  }

  // Runs `work` under the state mutex, then the resulting callbacks without it.
  template <typename F>
  void NetworkThreadStep(F&& work) {
    std::lock_guard<std::mutex> batch(cb_mu);
    std::vector<PendingCall> calls;
    {
      std::lock_guard<std::mutex> lk(mu);
      cb_thread = std::this_thread::get_id();
      cancelled_in_batch.clear();
      work();
      Collect(&calls);
    }
    for (auto& c : calls) {
      {
        std::lock_guard<std::mutex> lk(mu);
        if (cancelled_in_batch.count(c.lookup)) continue;
      }
      c.cb(c.index, c.res, c.all_done);
    }
    std::lock_guard<std::mutex> lk(mu);
    cb_thread = std::thread::id();
  }
};

QosService::QosService(PeerLink* link) : impl_(std::make_unique<Impl>(link)) {}

// The network thread must have stopped calling into the service.
QosService::~QosService() = default;

void QosService::Listen(const XnKid& kid, const uint8_t* data, size_t len, uint32_t bps,
                        uint32_t flags) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  const uint64_t key = KidKey(kid);
  if (flags & kFlagRelease) {
    impl_->listeners.erase(key);
    return;
  }
  Impl::Listener& l = impl_->listeners[key];
  if (flags & kFlagEnable) {
    l.listening = true;
    l.disabled = false;
  }
  if (flags & kFlagSetData) {
    // The blob must fit one RESP; AC6 publishes 40 bytes.
    const size_t mp = impl_->link->MaxPayload();
    const size_t cap = mp > kRespHeader ? std::min<size_t>(mp - kRespHeader, 0xFFFF) : 0;
    const size_t n = data ? std::min(len, cap) : 0;
    l.data.assign(data, data + n);
    if (!l.disabled) l.listening = true;
  }
  if (flags & kFlagSetBps) l.bps = bps;
  if (flags & kFlagDisable) {
    l.listening = false;
    l.disabled = true;
  }
}

uint64_t QosService::Lookup(const std::vector<uint32_t>& vips, const std::vector<XnKid>& kids,
                            uint32_t probes, QosCallback cb) {
  if (vips.empty() || !cb) return 0;
  std::lock_guard<std::mutex> lk(impl_->mu);
  const uint64_t id = impl_->next_lookup++;
  Impl::Lookup l;
  l.cb = std::move(cb);
  l.probes = std::clamp<uint32_t>(probes, 1, kMaxProbes);
  l.targets.resize(vips.size());
  for (size_t i = 0; i < vips.size(); ++i) {
    Impl::Target& t = l.targets[i];
    t.vip = vips[i];
    if (i < kids.size()) t.kid = kids[i];
    if (t.vip == 0) Impl::Complete(t);  // unresolvable: reported on the next Tick
  }
  Impl::Lookup& stored = impl_->lookups[id] = std::move(l);
  const uint64_t now = impl_->link->NowMs();
  for (size_t i = 0; i < stored.targets.size(); ++i) {
    if (!stored.targets[i].res.complete) impl_->SendProbe(id, i, stored.targets[i], now);
  }
  return id;
}

void QosService::Cancel(uint64_t lookup_id) {
  std::thread::id batch_thread;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->lookups.erase(lookup_id);
    impl_->ErasePendingProbes(lookup_id);
    batch_thread = impl_->cb_thread;
    if (batch_thread == std::this_thread::get_id()) impl_->cancelled_in_batch.insert(lookup_id);
  }
  // Called from outside a callback: wait for a running batch so no callback can follow.
  if (batch_thread != std::this_thread::get_id()) {
    std::lock_guard<std::mutex> wait(impl_->cb_mu);
  }
}

void QosService::OnLinkData(uint32_t from_vip, const uint8_t* data, size_t len) {
  impl_->NetworkThreadStep([&] { impl_->OnPacket(from_vip, data, len, impl_->link->NowMs()); });
}

void QosService::OnPeerLost(uint32_t vip) {
  impl_->NetworkThreadStep([&] {
    for (auto& [id, l] : impl_->lookups) {
      for (auto& t : l.targets) {
        if (t.vip == vip && !t.res.complete) Impl::Complete(t);
      }
    }
  });
}

void QosService::Tick(uint64_t now_ms) {
  impl_->NetworkThreadStep([&] {
    impl_->DrainLoop(now_ms);
    impl_->Advance(now_ms);
    impl_->DrainLoop(now_ms);
  });
}

}  // namespace ac6net
