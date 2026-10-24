// ac6net - TEST-ONLY network impairment (see overlay_impair.h).
#include "overlay_impair.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ac6net {

namespace {

constexpr uint32_t kMaxDelayMs = 10000;

std::string_view TrimView(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool ParseMs(std::string_view v, uint32_t* out) {
  if (v.empty() || v.size() > 6) return false;
  uint32_t x = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    x = x * 10 + static_cast<uint32_t>(c - '0');
  }
  if (x > kMaxDelayMs) return false;
  *out = x;
  return true;
}

bool ParseFraction(std::string_view v, double* out) {
  if (v.empty() || v.size() > 32) return false;
  for (char c : v) {
    if (!((c >= '0' && c <= '9') || c == '.')) return false;  // no sign, exponent, inf or nan
  }
  const std::string s(v);
  char* end = nullptr;
  errno = 0;
  const double x = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size() || !(x >= 0.0 && x <= 1.0)) return false;
  *out = x;
  return true;
}

}  // namespace

std::string ImpairConfig::Describe() const {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "delay=%ums jitter=+/-%ums loss=%.2f%% reorder=%s", delay_ms,
                jitter_ms, loss * 100.0, reorder ? "on" : "off");
  return buf;
}

bool ParseImpairSpec(std::string_view spec, ImpairConfig* out, std::string* error) {
  ImpairConfig cfg;
  spec = TrimView(spec);
  if (spec.empty()) {
    *out = cfg;
    return true;
  }
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };
  while (true) {
    const size_t comma = spec.find(',');
    const std::string_view item = TrimView(spec.substr(0, comma));
    const size_t eq = item.find('=');
    if (item.empty() || eq == std::string_view::npos) {
      return fail("expected key=value, got \"" + std::string(item) + "\"");
    }
    const std::string_view key = TrimView(item.substr(0, eq));
    const std::string_view val = TrimView(item.substr(eq + 1));
    if (key == "delay") {
      if (!ParseMs(val, &cfg.delay_ms))
        return fail("delay must be an integer number of ms in 0..10000");
    } else if (key == "jitter") {
      if (!ParseMs(val, &cfg.jitter_ms))
        return fail("jitter must be an integer number of ms in 0..10000");
    } else if (key == "loss") {
      if (!ParseFraction(val, &cfg.loss)) return fail("loss must be a fraction in 0..1");
    } else if (key == "reorder") {
      if (val == "1" || val == "true" || val == "on") {
        cfg.reorder = true;
      } else if (val == "0" || val == "false" || val == "off") {
        cfg.reorder = false;
      } else {
        return fail("reorder must be 0 or 1");
      }
    } else {
      return fail("unknown key \"" + std::string(key) + "\" (delay, jitter, loss, reorder)");
    }
    if (comma == std::string_view::npos) break;
    spec.remove_prefix(comma + 1);
  }
  *out = cfg;
  return true;
}

void Impairment::Configure(const ImpairConfig& cfg, uint64_t seed) {
  cfg_ = cfg;
  rng_ = seed ? seed : 0x9E3779B97F4A7C15ull;
  always_drop_ = cfg.loss >= 1.0;
  loss_threshold_ = 0;
  if (cfg.loss > 0.0 && !always_drop_) {
    // P(draw < threshold) = loss for a uniform 64-bit draw.
    const double t = std::ldexp(cfg.loss, 64);
    if (t >= std::ldexp(1.0, 64)) {
      always_drop_ = true;
    } else {
      loss_threshold_ = std::max<uint64_t>(static_cast<uint64_t>(t), 1);
    }
  }
  next_seq_ = 0;
  last_due_us_ = 0;
  bytes_ = 0;
  heap_.clear();
  counters_ = ImpairCounters{};
}

uint64_t Impairment::NextRandom() {
  rng_ ^= rng_ >> 12;
  rng_ ^= rng_ << 25;
  rng_ ^= rng_ >> 27;
  return rng_ * 0x2545F4914F6CDD1Dull;
}

Impairment::Verdict Impairment::Submit(const net::Addr& to, const uint8_t* data, size_t len,
                                       uint64_t now_us) {
  if (always_drop_ || (loss_threshold_ != 0 && NextRandom() < loss_threshold_)) {
    ++counters_.dropped;
    return Verdict::kDropped;
  }
  if (cfg_.delay_ms == 0 && cfg_.jitter_ms == 0) return Verdict::kSendNow;  // loss only

  int64_t delay_us = static_cast<int64_t>(cfg_.delay_ms) * 1000;
  if (cfg_.jitter_ms != 0) {
    const uint64_t j = static_cast<uint64_t>(cfg_.jitter_ms) * 1000;
    delay_us += static_cast<int64_t>(NextRandom() % (2 * j + 1)) - static_cast<int64_t>(j);
  }
  uint64_t due = now_us + static_cast<uint64_t>(std::max<int64_t>(delay_us, 0));
  if (!cfg_.reorder) due = std::max(due, last_due_us_);  // never overtake an earlier datagram
  if (due <= now_us && heap_.empty()) {
    last_due_us_ = std::max(last_due_us_, due);
    return Verdict::kSendNow;
  }
  if (heap_.size() >= kMaxPackets || bytes_ + len > kMaxBytes) {
    ++counters_.overflow;
    return Verdict::kDropped;
  }
  last_due_us_ = std::max(last_due_us_, due);
  Packet pkt;
  pkt.to = to;
  pkt.data.assign(data, data + len);
  pkt.due_us = due;
  pkt.seq = next_seq_++;
  heap_.push_back(std::move(pkt));
  std::push_heap(heap_.begin(), heap_.end(), Later{});
  bytes_ += len;
  ++counters_.delayed;
  counters_.queue_max = std::max<uint64_t>(counters_.queue_max, heap_.size());
  counters_.queue_max_bytes = std::max<uint64_t>(counters_.queue_max_bytes, bytes_);
  return Verdict::kQueued;
}

void Impairment::PopFront(Packet* out) {
  std::pop_heap(heap_.begin(), heap_.end(), Later{});
  *out = std::move(heap_.back());
  heap_.pop_back();
  bytes_ -= out->data.size();
  ++counters_.released;
}

bool Impairment::PopDue(uint64_t now_us, Packet* out) {
  if (heap_.empty() || heap_.front().due_us > now_us) return false;
  PopFront(out);
  return true;
}

bool Impairment::PopAny(Packet* out) {
  if (heap_.empty()) return false;
  PopFront(out);
  return true;
}

uint64_t Impairment::NextDueUs() const {
  return heap_.empty() ? std::numeric_limits<uint64_t>::max() : heap_.front().due_us;
}

}  // namespace ac6net
