// In-memory overlay stand-in for vnet and qos tests: N nodes (one VNet and/or QosService each)
// joined by a FakeNet with configurable loss, delay and jitter (jitter reorders packets).
//
// A single network thread delivers due packets and ticks every node every 5 ms. Like the real
// overlay it never holds its own lock while it calls into vnet/qos.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "ac6net/netplay.h"
#include "ac6net/vnet.h"
#include "link.h"
#include "qos.h"

namespace vnet_fake {

using ac6net::Channel;

inline uint64_t SteadyMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct NetParams {
  double loss = 0.0;       // per-packet drop probability (all channels)
  uint32_t delay_ms = 1;   // base one-way latency
  uint32_t jitter_ms = 0;  // extra uniform delay in [0, jitter]; reorders packets
  uint32_t seed = 12345;
  size_t max_payload = 1200;  // the contract's minimum MaxPayload
};

class FakeNet;

class FakeLink final : public ac6net::PeerLink {
 public:
  FakeLink(FakeNet* net, uint32_t vip) : net_(net), vip_(vip) {}
  bool Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) override;
  uint32_t LocalVip() const override { return vip_; }
  size_t MaxPayload() const override;
  uint64_t NowMs() const override;

 private:
  FakeNet* net_;
  uint32_t vip_;
};

struct Node {
  uint32_t vip = 0;
  std::unique_ptr<FakeLink> link;
  std::unique_ptr<ac6net::vnet::VNet> vnet;
  std::unique_ptr<ac6net::QosService> qos;
};

class FakeNet {
 public:
  explicit FakeNet(NetParams params = {}) : params_(params), rng_(params.seed), t0_(SteadyMs()) {}
  ~FakeNet() { Stop(); }

  // Add every node before Start().
  Node& AddNode(uint32_t vip, bool with_vnet = true, bool with_qos = false) {
    auto n = std::make_unique<Node>();
    n->vip = vip;
    n->link = std::make_unique<FakeLink>(this, vip);
    if (with_vnet) n->vnet = std::make_unique<ac6net::vnet::VNet>(n->link.get());
    if (with_qos) n->qos = std::make_unique<ac6net::QosService>(n->link.get());
    Node& ref = *n;
    nodes_[vip] = std::move(n);
    return ref;
  }

  void Start() {
    running_ = true;
    thread_ = std::thread([this] { Run(); });
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    thread_.join();
  }

  uint64_t NowMs() const { return SteadyMs() - t0_ + 1000; }
  size_t max_payload() const { return params_.max_payload; }

  void SetLoss(double loss) {
    std::lock_guard<std::mutex> lk(mu_);
    params_.loss = loss;
  }
  // Silently drops everything sent to or from `vip` (a peer that vanished without BYE).
  void SetBlackhole(uint32_t vip, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (on) {
      blackholes_.insert(vip);
    } else {
      blackholes_.erase(vip);
    }
  }

  // Observes every accepted Send (called with the FakeNet lock held; must not call back in).
  using Tap = std::function<void(uint32_t from, uint32_t to, Channel, const uint8_t*, size_t)>;
  void SetTap(Tap tap) {
    std::lock_guard<std::mutex> lk(mu_);
    tap_ = std::move(tap);
  }

  uint64_t packets_sent() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sent_;
  }

  bool Send(uint32_t from, uint32_t to, Channel channel, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!nodes_.count(to) || len > params_.max_payload) return false;  // not ours / too large
    ++sent_;
    if (tap_) tap_(from, to, channel, data, len);
    if (blackholes_.count(to) || blackholes_.count(from)) return true;
    if (params_.loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng_) < params_.loss) {
      return true;
    }
    uint64_t due = NowMs() + params_.delay_ms;
    if (params_.jitter_ms)
      due += std::uniform_int_distribution<uint32_t>(0, params_.jitter_ms)(rng_);
    queue_.push(Packet{due, seq_++, from, to, channel, std::vector<uint8_t>(data, data + len)});
    return true;
  }

 private:
  struct Packet {
    uint64_t due;
    uint64_t seq;
    uint32_t from, to;
    Channel channel;
    std::vector<uint8_t> data;
    bool operator>(const Packet& o) const { return due != o.due ? due > o.due : seq > o.seq; }
  };

  void Run() {
    uint64_t next_tick = 0;
    while (running_) {
      const uint64_t now = NowMs();
      std::vector<Packet> due;
      {
        std::lock_guard<std::mutex> lk(mu_);
        while (!queue_.empty() && queue_.top().due <= now) {
          due.push_back(queue_.top());
          queue_.pop();
        }
      }
      for (const Packet& p : due) {  // no FakeNet lock held here
        Node& n = *nodes_.at(p.to);
        if (p.channel == Channel::kQos) {
          if (n.qos) n.qos->OnLinkData(p.from, p.data.data(), p.data.size());
        } else if (n.vnet) {
          n.vnet->OnLinkData(p.from, uint8_t(p.channel), p.data.data(), p.data.size());
        }
      }
      if (now >= next_tick) {
        for (auto& [vip, n] : nodes_) {
          if (n->vnet) n->vnet->Tick(now);
          if (n->qos) n->qos->Tick(now);
        }
        next_tick = now + 5;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }

  NetParams params_;
  mutable std::mutex mu_;
  std::mt19937 rng_;
  uint64_t t0_;
  std::map<uint32_t, std::unique_ptr<Node>> nodes_;  // fixed after Start()
  std::priority_queue<Packet, std::vector<Packet>, std::greater<Packet>> queue_;
  std::set<uint32_t> blackholes_;
  Tap tap_;
  uint64_t seq_ = 0;
  uint64_t sent_ = 0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

inline bool FakeLink::Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) {
  return net_->Send(vip_, vip, channel, data, len);
}
inline size_t FakeLink::MaxPayload() const { return net_->max_payload(); }
inline uint64_t FakeLink::NowMs() const { return net_->NowMs(); }

// Polls `pred` every millisecond until it holds or `timeout_ms` passes.
inline bool WaitFor(const std::function<bool()>& pred, int timeout_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

}  // namespace vnet_fake
