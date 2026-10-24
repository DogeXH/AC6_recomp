// ac6net - TEST-ONLY network impairment for the overlay's outgoing datagrams.
//
// Turned on only by the environment variable AC6NET_TEST_IMPAIR, read once by Overlay::Start():
//
//   AC6NET_TEST_IMPAIR="delay=75,jitter=20,loss=0.02"        (optionally ",reorder=1")
//
//   delay   one-way delay added to every outgoing datagram, ms (integer, 0..10000)
//   jitter  uniform +/- jitter around the delay, ms (integer, 0..10000; per-packet delay >= 0)
//   loss    independent drop probability per outgoing datagram (0..1)
//   reorder 1 lets jitter reorder datagrams; 0 (default) keeps send order
//
// Unset or empty: off, and the send path costs one branch. Malformed: the overlay logs an error
// and stays off. This is for running two instances on one LAN with internet-like conditions;
// it is never meant for real play.
//
// Not thread-safe: the overlay calls every method with Overlay::Impl::mu held, the same lock
// that serialises its socket sends (see overlay_internal.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "overlay_net.h"

namespace ac6net {

inline constexpr const char kImpairEnvVar[] = "AC6NET_TEST_IMPAIR";

struct ImpairConfig {
  uint32_t delay_ms = 0;
  uint32_t jitter_ms = 0;
  double loss = 0.0;     // 0..1
  bool reorder = false;  // allow jitter to reorder datagrams

  // True if the config changes anything (delay, jitter or loss is non-zero).
  bool active() const { return delay_ms != 0 || jitter_ms != 0 || loss > 0.0; }
  // "delay=75ms jitter=+/-20ms loss=2.00% reorder=off"
  std::string Describe() const;
};

// Parses "key=value[,key=value...]" with keys delay, jitter, loss, reorder (spaces around items
// are ignored; a later duplicate key wins). An empty or all-blank spec is valid and yields the
// default (inactive) config. Returns false with `error` set on anything else.
bool ParseImpairSpec(std::string_view spec, ImpairConfig* out, std::string* error);

struct ImpairCounters {
  uint64_t delayed = 0;     // datagrams that went through the delay queue
  uint64_t dropped = 0;     // dropped by the random loss
  uint64_t overflow = 0;    // dropped because the delay queue was full
  uint64_t released = 0;    // delayed datagrams handed back for sending
  uint64_t queue_max = 0;   // high-water mark of the delay queue (datagrams)
  uint64_t queue_max_bytes = 0;
};

class Impairment {
 public:
  static constexpr size_t kMaxPackets = 4096;
  static constexpr size_t kMaxBytes = 8u << 20;  // 8 MiB of queued payload

  struct Packet {
    net::Addr to;
    std::vector<uint8_t> data;
    uint64_t due_us = 0;
    uint64_t seq = 0;  // submission order (tie-break and FIFO check)
  };

  enum class Verdict {
    kSendNow,  // send it yourself, now (no delay applies and nothing is queued ahead of it)
    kQueued,   // copied into the delay queue; Release() hands it back when due
    kDropped,  // random loss or queue overflow (counted)
  };

  // Resets the queue and counters. `seed` drives the loss and jitter draws (deterministic for
  // tests; the overlay passes a random seed).
  void Configure(const ImpairConfig& cfg, uint64_t seed);
  const ImpairConfig& config() const { return cfg_; }
  bool active() const { return cfg_.active(); }

  Verdict Submit(const net::Addr& to, const uint8_t* data, size_t len, uint64_t now_us);

  // Removes the next datagram whose due time is <= now_us into `out`. Returns false when none
  // is due. Datagrams come out in due-time order (submission order unless reorder is on).
  bool PopDue(uint64_t now_us, Packet* out);
  // Removes the next queued datagram regardless of its due time (shutdown flush).
  bool PopAny(Packet* out);

  // Due time of the next queued datagram, or UINT64_MAX when the queue is empty.
  uint64_t NextDueUs() const;
  size_t queued() const { return heap_.size(); }
  size_t queued_bytes() const { return bytes_; }
  const ImpairCounters& counters() const { return counters_; }

 private:
  // Heap order for std::push_heap/pop_heap: the earliest (due_us, seq) at the front.
  struct Later {
    bool operator()(const Packet& a, const Packet& b) const {
      return a.due_us != b.due_us ? a.due_us > b.due_us : a.seq > b.seq;
    }
  };

  uint64_t NextRandom();  // xorshift64*
  void PopFront(Packet* out);

  ImpairConfig cfg_;
  uint64_t rng_ = 0x9E3779B97F4A7C15ull;
  uint64_t loss_threshold_ = 0;  // draw < threshold => drop (0 = never, max = always)
  bool always_drop_ = false;
  uint64_t next_seq_ = 0;
  uint64_t last_due_us_ = 0;  // FIFO floor when reorder is off
  size_t bytes_ = 0;
  std::vector<Packet> heap_;  // binary heap ordered by Later
  ImpairCounters counters_;
};

}  // namespace ac6net
