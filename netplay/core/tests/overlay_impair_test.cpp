// ac6net TEST-ONLY network impairment (AC6NET_TEST_IMPAIR, src/overlay_impair.h): spec parsing,
// loss rate, delay/jitter bounds, ordering, the queue bound, and the overlay hook (off by
// default, delays and drops real datagrams, flushes at Stop). No server needed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "crypto.h"
#include "mini_test.h"
#include "nlohmann/json.hpp"
#include "overlay.h"
#include "overlay_impair.h"
#include "overlay_net.h"
#include "overlay_test_util.h"

using namespace ac6net;
using json = nlohmann::json;

namespace {

// Sets (or, with nullptr, unsets) an environment variable for the scope and restores the old
// value afterwards.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* old = std::getenv(name);
    had_old_ = old != nullptr;
    if (had_old_) old_ = old;
    Set(value);
  }
  ~ScopedEnv() { Set(had_old_ ? old_.c_str() : nullptr); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void Set(const char* value) {
#if defined(_WIN32)
    _putenv_s(name_.c_str(), value ? value : "");
#else
    if (value) setenv(name_.c_str(), value, 1);
    else unsetenv(name_.c_str());
#endif
  }
  std::string name_;
  std::string old_;
  bool had_old_ = false;
};

net::Addr Loopback(uint16_t port) {
  net::Addr a;
  net::Addr::FromIpPort("127.0.0.1", port, &a);
  return a;
}

ImpairConfig Cfg(uint32_t delay, uint32_t jitter, double loss, bool reorder = false) {
  ImpairConfig c;
  c.delay_ms = delay;
  c.jitter_ms = jitter;
  c.loss = loss;
  c.reorder = reorder;
  return c;
}

// Packet payload = its submission index (4 bytes LE).
std::vector<uint8_t> Index(uint32_t i) {
  return {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i >> 16),
          static_cast<uint8_t>(i >> 24)};
}
uint32_t IndexOf(const std::vector<uint8_t>& d) {
  return d.size() < 4 ? 0xFFFFFFFFu
                      : uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 |
                            uint32_t(d[3]) << 24;
}

// The overlay under test talks to a "server" address where our own socket listens, so we can
// see when its datagrams (HELLOs) actually leave.
constexpr uint16_t kSinkPort = 47819;

Config SinkConfig() {
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  Config c;
  c.enabled = true;
  c.server = "127.0.0.1:" + std::to_string(kSinkPort);
  c.server_key_hex = HexEncode(pk.data(), pk.size());
  c.gamertag = "Impaired";
  c.port = overlay_test::kClientPortBase;
  return c;
}

// Milliseconds from `t0` until the first datagram arrives at `sink`, or -1 after `timeout_ms`.
long long FirstArrivalMs(net::UdpSocket& sink, std::chrono::steady_clock::time_point t0,
                         int timeout_ms) {
  const auto deadline = t0 + std::chrono::milliseconds(timeout_ms);
  uint8_t buf[2048];
  while (std::chrono::steady_clock::now() < deadline) {
    net::UdpSocket* socks[1] = {&sink};
    bool readable[1] = {false};
    net::PollReadable(socks, 1, 2, readable);
    net::Addr from;
    if (readable[0] && sink.RecvFrom(buf, sizeof(buf), &from) > 0) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0)
          .count();
    }
  }
  return -1;
}

void Drain(net::UdpSocket& sink) {
  uint8_t buf[2048];
  net::Addr from;
  while (sink.RecvFrom(buf, sizeof(buf), &from) != 0) {
  }
}

}  // namespace

TEST_CASE("impair: spec parsing") {
  ImpairConfig c;
  std::string err;
  REQUIRE(ParseImpairSpec("delay=75,jitter=20,loss=0.02", &c, &err));
  CHECK(c.delay_ms == 75);
  CHECK(c.jitter_ms == 20);
  CHECK(c.loss == 0.02);
  CHECK(!c.reorder);
  CHECK(c.active());
  CHECK(c.Describe() == "delay=75ms jitter=+/-20ms loss=2.00% reorder=off");

  REQUIRE(ParseImpairSpec(" delay = 40 , reorder=1 ,loss=1 ", &c, &err));
  CHECK(c.delay_ms == 40 && c.jitter_ms == 0 && c.loss == 1.0 && c.reorder);
  REQUIRE(ParseImpairSpec("loss=.5,loss=0.25", &c, &err));  // later duplicate wins
  CHECK(c.loss == 0.25 && c.delay_ms == 0);
  REQUIRE(ParseImpairSpec("jitter=10000,reorder=off", &c, &err));
  CHECK(c.jitter_ms == 10000 && !c.reorder && c.active());

  // Empty or blank: valid, inactive.
  for (const char* off : {"", "   ", "\t"}) {
    c = Cfg(1, 1, 0.5);
    CHECK(ParseImpairSpec(off, &c, &err));
    CHECK(!c.active());
  }
  // All zero: valid but changes nothing.
  REQUIRE(ParseImpairSpec("delay=0,jitter=0,loss=0", &c, &err));
  CHECK(!c.active());

  for (const char* bad :
       {"delay=abc", "delay=-5", "delay=75ms", "delay=10001", "delay=", "delay", "=5",
        "jitter=1.5", "loss=1.5", "loss=-0.1", "loss=nan", "loss=inf", "loss=1e-2", "loss=2%",
        "loss=", "reorder=2", "reorder=yes", "foo=1", "delay=75,,loss=0.1", "delay=75,",
        ",delay=75", "delay=75;loss=0.1", "delay=99999999999"}) {
    err.clear();
    c = Cfg(7, 7, 0.0);
    CHECK(!ParseImpairSpec(bad, &c, &err));
    CHECK(!err.empty());
    CHECK(c.delay_ms == 7 && c.jitter_ms == 7);  // untouched on failure
  }
}

TEST_CASE("impair: off by default; loss-only never queues") {
  Impairment imp;
  CHECK(!imp.active());
  const net::Addr to = Loopback(9);
  const uint8_t d[8] = {};
  for (int i = 0; i < 1000; ++i) {
    CHECK(imp.Submit(to, d, sizeof(d), 1000u * i) == Impairment::Verdict::kSendNow);
  }
  CHECK(imp.queued() == 0);
  CHECK(imp.counters().delayed == 0 && imp.counters().dropped == 0);
  CHECK(imp.NextDueUs() == UINT64_MAX);

  imp.Configure(Cfg(0, 0, 0.5), 42);
  int now_n = 0;
  for (int i = 0; i < 1000; ++i) {
    const auto v = imp.Submit(to, d, sizeof(d), 1000u * i);
    CHECK(v != Impairment::Verdict::kQueued);
    now_n += v == Impairment::Verdict::kSendNow;
  }
  CHECK(imp.queued() == 0);
  CHECK(now_n + imp.counters().dropped == 1000);
}

TEST_CASE("impair: loss rate within tolerance over many packets") {
  const net::Addr to = Loopback(9);
  const uint8_t d[4] = {};
  for (double loss : {0.02, 0.3}) {
    Impairment imp;
    imp.Configure(Cfg(0, 0, loss), crypto::RandomU64());
    const int n = 200000;
    for (int i = 0; i < n; ++i) imp.Submit(to, d, sizeof(d), i);
    const double rate = static_cast<double>(imp.counters().dropped) / n;
    // 6+ standard deviations: sqrt(p(1-p)/n) is 0.0003 at 2% and 0.001 at 30%.
    const double tol = loss < 0.1 ? 0.002 : 0.006;
    std::fprintf(stderr, "  loss %.2f: measured %.4f over %d\n", loss, rate, n);
    CHECK(rate > loss - tol && rate < loss + tol);
  }
  Impairment all;
  all.Configure(Cfg(50, 0, 1.0), 1);
  for (int i = 0; i < 1000; ++i) {
    CHECK(all.Submit(to, d, sizeof(d), i) == Impairment::Verdict::kDropped);
  }
  CHECK(all.counters().dropped == 1000 && all.queued() == 0);
  // Same seed, same draws (the overlay seeds randomly; tests can be exact).
  Impairment a, b;
  a.Configure(Cfg(0, 0, 0.1), 77);
  b.Configure(Cfg(0, 0, 0.1), 77);
  for (int i = 0; i < 5000; ++i) {
    CHECK(a.Submit(to, d, sizeof(d), i) == b.Submit(to, d, sizeof(d), i));
  }
}

TEST_CASE("impair: delay and jitter bounds") {
  const net::Addr to = Loopback(9);
  {
    // reorder=1 so no FIFO clamp: every datagram is due exactly delay +/- jitter after submit.
    Impairment imp;
    imp.Configure(Cfg(75, 20, 0.0, /*reorder=*/true), crypto::RandomU64());
    const uint32_t n = 4000;
    std::vector<uint64_t> submit(n);
    for (uint32_t i = 0; i < n; ++i) {
      submit[i] = 1000000 + 250ull * i;
      const auto d = Index(i);
      REQUIRE(imp.Submit(to, d.data(), d.size(), submit[i]) == Impairment::Verdict::kQueued);
    }
    CHECK(imp.counters().delayed == n);
    uint64_t lo = UINT64_MAX, hi = 0;
    double sum = 0;
    Impairment::Packet p;
    uint32_t got = 0;
    while (imp.PopAny(&p)) {
      const uint32_t i = IndexOf(p.data);
      REQUIRE(i < n);
      const uint64_t delay = p.due_us - submit[i];
      lo = std::min(lo, delay);
      hi = std::max(hi, delay);
      sum += static_cast<double>(delay);
      CHECK(p.to == to);
      ++got;
    }
    CHECK(got == n);
    std::fprintf(stderr, "  delay 75 +/- 20 ms: min %.3f max %.3f mean %.3f ms\n", lo / 1000.0,
                 hi / 1000.0, sum / n / 1000.0);
    CHECK(lo >= 55000 && hi <= 95000);
    CHECK(lo < 56000 && hi > 94000);  // the whole range is used
    CHECK(std::abs(sum / n - 75000.0) < 1000.0);
  }
  {
    // Fixed delay, no jitter: exact.
    Impairment imp;
    imp.Configure(Cfg(30, 0, 0.0), 5);
    const auto d = Index(1);
    REQUIRE(imp.Submit(to, d.data(), d.size(), 5000) == Impairment::Verdict::kQueued);
    CHECK(imp.NextDueUs() == 35000);
    Impairment::Packet p;
    CHECK(!imp.PopDue(34999, &p));
    CHECK(imp.PopDue(35000, &p));
    CHECK(IndexOf(p.data) == 1);
  }
  {
    // Jitter larger than the delay: per-datagram delay is clamped at 0, never negative; a
    // datagram due immediately with nothing queued ahead is sent at once.
    Impairment imp;
    imp.Configure(Cfg(5, 30, 0.0, /*reorder=*/true), 9);
    int send_now = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
      const auto d = Index(i);
      const uint64_t t = 1000000 + 100000ull * i;  // far apart: the queue is drained between
      Impairment::Packet p;
      while (imp.PopDue(t, &p)) CHECK(p.due_us <= t);
      const auto v = imp.Submit(to, d.data(), d.size(), t);
      if (v == Impairment::Verdict::kSendNow) {
        ++send_now;
      } else {
        CHECK(v == Impairment::Verdict::kQueued);
        CHECK(imp.NextDueUs() > t && imp.NextDueUs() <= t + 35000);
      }
    }
    CHECK(send_now > 0);  // about 25/61 of the draws are <= 0 ms
  }
}

TEST_CASE("impair: ordering (FIFO by default, reorder=1 allows overtaking)") {
  const net::Addr to = Loopback(9);
  for (bool reorder : {false, true}) {
    Impairment imp;
    imp.Configure(Cfg(50, 20, 0.0, reorder), 1234);
    const uint32_t n = 3000;
    // 1 datagram per ms, released while submitting (as the network loop does).
    std::vector<uint32_t> order;
    uint64_t last_due = 0;
    bool due_respected = true;
    Impairment::Packet p;
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t now = 1000000 + 1000ull * i;
      while (imp.PopDue(now, &p)) {
        due_respected &= p.due_us <= now;
        due_respected &= p.due_us >= last_due;  // released in due-time order
        last_due = p.due_us;
        order.push_back(IndexOf(p.data));
      }
      const auto d = Index(i);
      REQUIRE(imp.Submit(to, d.data(), d.size(), now) == Impairment::Verdict::kQueued);
    }
    while (imp.PopDue(UINT64_MAX, &p)) order.push_back(IndexOf(p.data));
    CHECK(due_respected);
    REQUIRE(order.size() == n);
    int inversions = 0;
    for (size_t k = 1; k < order.size(); ++k) inversions += order[k] < order[k - 1];
    std::vector<uint32_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());  // nothing lost or doubled
    std::fprintf(stderr, "  reorder=%d: %d inversions in %u datagrams\n", reorder ? 1 : 0,
                 inversions, n);
    if (reorder) {
      CHECK(inversions > 100);
    } else {
      CHECK(inversions == 0);
    }
    CHECK(imp.counters().released == n);
    CHECK(imp.queued() == 0 && imp.queued_bytes() == 0);
  }
}

TEST_CASE("impair: queue is bounded (4096 datagrams / 8 MiB), overflow counted") {
  const net::Addr to = Loopback(9);
  {
    Impairment imp;
    imp.Configure(Cfg(1000, 0, 0.0), 3);
    const uint8_t d[64] = {};
    int queued = 0, dropped = 0;
    for (int i = 0; i < 5000; ++i) {
      const auto v = imp.Submit(to, d, sizeof(d), 1000);
      queued += v == Impairment::Verdict::kQueued;
      dropped += v == Impairment::Verdict::kDropped;
    }
    CHECK(queued == 4096 && dropped == 904);
    CHECK(imp.queued() == Impairment::kMaxPackets);
    CHECK(imp.counters().overflow == 904);
    CHECK(imp.counters().dropped == 0);
    CHECK(imp.counters().queue_max == 4096);
    // Space frees up as datagrams are released.
    Impairment::Packet p;
    REQUIRE(imp.PopDue(1001000, &p));
    CHECK(imp.Submit(to, d, sizeof(d), 2000) == Impairment::Verdict::kQueued);
    CHECK(imp.Submit(to, d, sizeof(d), 2000) == Impairment::Verdict::kDropped);
  }
  {
    Impairment imp;
    imp.Configure(Cfg(1000, 0, 0.0), 3);
    const std::vector<uint8_t> big(4096, 0x5A);  // 2048 of these fill 8 MiB
    int queued = 0;
    for (int i = 0; i < 3000; ++i) {
      queued += imp.Submit(to, big.data(), big.size(), 1000) == Impairment::Verdict::kQueued;
    }
    CHECK(queued == 2048);
    CHECK(imp.queued_bytes() == Impairment::kMaxBytes);
    CHECK(imp.counters().overflow == 952);
    CHECK(imp.counters().queue_max_bytes == Impairment::kMaxBytes);
    Impairment::Packet p;
    int n = 0;
    while (imp.PopAny(&p)) n += p.data == big;
    CHECK(n == 2048);
    CHECK(imp.queued_bytes() == 0);
  }
}

TEST_CASE("impair: overlay is unimpaired without AC6NET_TEST_IMPAIR, or with a bad value") {
  net::UdpSocket sink;
  std::string err;
  REQUIRE(sink.Open(AF_INET, kSinkPort, &err));
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  for (const char* value : {static_cast<const char*>(nullptr), "", "delay=fast,loss=0.5"}) {
    ScopedEnv env(kImpairEnvVar, value);
    Drain(sink);
    Overlay ov;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ov.Start(SinkConfig(), sk));
    const long long ms = FirstArrivalMs(sink, t0, 1000);
    std::fprintf(stderr, "  env %s: first HELLO after %lld ms\n",
                 value ? (*value ? value : "\"\"") : "unset", ms);
    CHECK(ms >= 0 && ms < 250);  // well below the 300 ms delay case below (slow hosts: Wine)
    const auto c = ov.counters();
    CHECK(c.datagrams_sent >= 1);
    CHECK(c.impair_delayed == 0 && c.impair_dropped == 0 && c.impair_overflow == 0 &&
          c.impair_queue_max == 0 && c.impair_queued == 0);
    const json st = json::parse(ov.StatusJson(), nullptr, false);
    REQUIRE(st.is_object());
    CHECK(st["impair"]["active"] == false);
    CHECK(!st["impair"].contains("delay_ms"));
    ov.Stop();
  }
}

TEST_CASE("impair: overlay delays and drops outgoing datagrams from AC6NET_TEST_IMPAIR") {
  net::UdpSocket sink;
  std::string err;
  REQUIRE(sink.Open(AF_INET, kSinkPort, &err));
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  {
    ScopedEnv env(kImpairEnvVar, "delay=300,jitter=0");
    Drain(sink);
    Overlay ov;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ov.Start(SinkConfig(), sk));
    const long long ms = FirstArrivalMs(sink, t0, 2000);
    std::fprintf(stderr, "  delay=300: first HELLO after %lld ms\n", ms);
    CHECK(ms >= 300 && ms < 600);
    const auto c = ov.counters();
    CHECK(c.impair_delayed >= 1);
    CHECK(c.impair_dropped == 0);
    CHECK(c.datagrams_sent >= 1);  // counted when it actually leaves
    const json st = json::parse(ov.StatusJson(), nullptr, false);
    REQUIRE(st.is_object());
    CHECK(st["impair"]["active"] == true);
    CHECK(st["impair"]["delay_ms"] == 300);
    CHECK(st["impair"]["delayed"].get<uint64_t>() >= 1);
    ov.Stop();
  }
  {
    // Whatever is still delayed at Stop() leaves at once (so BYE reaches peers and the server).
    ScopedEnv env(kImpairEnvVar, "delay=5000");
    Drain(sink);
    Overlay ov;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ov.Start(SinkConfig(), sk));
    overlay_test::SleepMs(100);
    CHECK(ov.counters().impair_queued >= 1);
    CHECK(ov.counters().datagrams_sent == 0);
    ov.Stop();
    const long long ms = FirstArrivalMs(sink, t0, 1000);
    std::fprintf(stderr, "  delay=5000, Stop at 100 ms: first HELLO after %lld ms\n", ms);
    CHECK(ms >= 0 && ms < 1000);
    CHECK(ov.counters().impair_queued == 0);
    CHECK(ov.counters().datagrams_sent >= 1);
  }
  {
    ScopedEnv env(kImpairEnvVar, "loss=1");
    Drain(sink);
    Overlay ov;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ov.Start(SinkConfig(), sk));
    CHECK(FirstArrivalMs(sink, t0, 400) == -1);
    const auto c = ov.counters();
    CHECK(c.impair_dropped >= 1);
    CHECK(c.datagrams_sent == 0);
    CHECK(c.impair_delayed == 0);
    ov.Stop();
  }
}

TEST_CASE("[server] impair: two impaired peers through a real lobby (RPC, intro, direct path)") {
  using namespace overlay_test;
  // A fresh self-started lobby only: an external lobby's new-account limit (20 per IP per hour)
  // is already used up by the other [server] tests.
  if (!TestServer::CanRestart()) SKIP("needs AC6NET_TEST_SERVER_BIN (a fresh lobby)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  // One direct candidate each (the server-reflexive 127.0.0.1 address). With several equally
  // good candidates (127.0.0.1, [::1], LAN IPs) the jittered RTT makes the overlay switch
  // between them, and datagrams arriving on two sockets around a switch can be reordered by
  // the receiver; that is overlay behaviour, not the impairment's, so keep this test to one path.
  a.config().lan = false;
  b.config().lan = false;
  {
    // Both instances impair their own sends, so a peer round trip carries the delay twice.
    ScopedEnv env(kImpairEnvVar, "delay=40,jitter=10,loss=0.05");
    REQUIRE(a.Start(10000));
    REQUIRE(b.Start(10000));
  }
  const uint32_t va = a.vip(), vb = b.vip();

  // RPC: only the client->server leg is delayed (the lobby itself is not impaired).
  int ok = 0;
  uint64_t rpc_min_ms = UINT64_MAX;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    ok += a.overlay().Call("whoami", "{}", 8000).ok;
    rpc_min_ms = std::min<uint64_t>(rpc_min_ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - t0)
                                                    .count());
  }
  CHECK(ok == 5);
  CHECK(rpc_min_ms >= 29);

  // Introduction and a validated direct path, despite delay and 5% loss.
  REQUIRE(WaitFor(
      [&] {
        const auto p = Payload(0, 50);
        a.overlay().Send(vb, Channel::kDatagram, p.data(), p.size());
        return b.CountFrom(va) > 0;
      },
      8000));
  REQUIRE(WaitFor([&] { return a.IsDirectTo(vb) && b.IsDirectTo(va); }, 8000));
  OverlayPeerInfo info;
  REQUIRE(a.Peer(vb, &info));
  std::fprintf(stderr, "  A->B direct via %s, probe rtt %u ms (2 x 40 +/- 10 expected)\n",
               info.path.c_str(), info.rtt_ms);
  CHECK(info.rtt_ms >= 55);
  CHECK(info.rtt_ms < 400);

  // Datagram echo round trip carries both delays. Let the datagrams still in the delay queues
  // land first, and match each echo by its sequence number.
  SleepMs(300);
  b.set_echo(true);
  auto echoed = [&](uint32_t seq) {
    for (const auto& r : a.received()) {
      if (r.vip == vb && r.data.size() == 64 && r.data == Payload(seq, 64)) return true;
    }
    return false;
  };
  uint64_t echo_min_ms = UINT64_MAX;
  for (uint32_t i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto p = Payload(1000 + i, 64);
    a.overlay().Send(vb, Channel::kDatagram, p.data(), p.size());
    if (WaitFor([&] { return echoed(1000 + i); }, 1000)) {
      echo_min_ms = std::min<uint64_t>(echo_min_ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now() - t0)
                                                        .count());
    }
  }
  b.set_echo(false);
  std::fprintf(stderr, "  datagram echo rtt min %llu ms\n", (unsigned long long)echo_min_ms);
  CHECK(echo_min_ms >= 59 && echo_min_ms < 400);

  // A burst: about 5% lost, the rest in send order.
  const size_t had = b.CountFrom(va);
  for (uint32_t i = 0; i < 300; ++i) {
    const auto p = Payload(2000 + i, 200);
    a.overlay().Send(vb, Channel::kDatagram, p.data(), p.size());
    SleepMs(1);
  }
  SleepMs(400);
  std::vector<uint32_t> seqs;
  for (const auto& r : b.received()) {
    if (r.vip != va || r.data.size() != 200) continue;
    const uint32_t seq = (uint32_t{r.data[0]} << 24) | (uint32_t{r.data[1]} << 16) |
                         (uint32_t{r.data[2]} << 8) | r.data[3];
    if (seq >= 2000) seqs.push_back(seq);
  }
  const size_t got = b.CountFrom(va) - had;
  std::fprintf(stderr, "  %zu/300 datagrams through 5%% loss; path switches A %llu B %llu\n", got,
               (unsigned long long)a.overlay().counters().path_switches,
               (unsigned long long)b.overlay().counters().path_switches);
  CHECK(got >= 255 && got <= 300);
  CHECK(std::is_sorted(seqs.begin(), seqs.end()));
  const auto ca = a.overlay().counters();
  CHECK(ca.impair_delayed > 300);
  CHECK(ca.impair_dropped > 0);
  CHECK(ca.impair_overflow == 0);
  CHECK(a.lost().empty() && b.lost().empty());
}

TEST_CASE("[server] overlay: jitter on several equal direct paths does not flap between them") {
  using namespace overlay_test;
  if (!TestServer::CanRestart()) SKIP("needs AC6NET_TEST_SERVER_BIN (a fresh lobby)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  // lan = true (default): 127.0.0.1, [::1] and the LAN addresses are all candidates of the same
  // type with the same real RTT, so only the jitter separates them. The path choice uses a
  // smoothed RTT, a proportional margin and a minimum dwell, so it must settle on one.
  {
    ScopedEnv env(kImpairEnvVar, "delay=20,jitter=15");
    REQUIRE(a.Start(10000));
    REQUIRE(b.Start(10000));
  }
  const uint32_t va = a.vip(), vb = b.vip();
  REQUIRE(WaitFor(
      [&] {
        const auto p = Payload(0, 50);
        a.overlay().Send(vb, Channel::kDatagram, p.data(), p.size());
        return b.CountFrom(va) > 0;
      },
      8000));
  REQUIRE(WaitFor([&] { return a.IsDirectTo(vb) && b.IsDirectTo(va); }, 8000));
  const uint64_t sa0 = a.overlay().counters().path_switches;
  const uint64_t sb0 = b.overlay().counters().path_switches;
  // 8 s of steady game-like traffic (a datagram every 20 ms) while probes keep sampling RTTs.
  for (uint32_t i = 0; i < 400; ++i) {
    const auto p = Payload(5000 + i, 72);
    a.overlay().Send(vb, Channel::kDatagram, p.data(), p.size());
    SleepMs(20);
  }
  SleepMs(300);
  std::vector<uint32_t> seqs;
  for (const auto& r : b.received()) {
    if (r.vip != va || r.data.size() != 72) continue;
    const uint32_t seq = (uint32_t{r.data[0]} << 24) | (uint32_t{r.data[1]} << 16) |
                         (uint32_t{r.data[2]} << 8) | r.data[3];
    if (seq >= 5000) seqs.push_back(seq);
  }
  const uint64_t sa = a.overlay().counters().path_switches - sa0;
  const uint64_t sb = b.overlay().counters().path_switches - sb0;
  std::fprintf(stderr, "  %zu/400 datagrams; path switches after settling: A %llu B %llu\n",
               seqs.size(), (unsigned long long)sa, (unsigned long long)sb);
  CHECK(seqs.size() == 400);
  CHECK(std::is_sorted(seqs.begin(), seqs.end()));
  CHECK(sa <= 1);
  CHECK(sb <= 1);
  CHECK(a.lost().empty() && b.lost().empty());
}
