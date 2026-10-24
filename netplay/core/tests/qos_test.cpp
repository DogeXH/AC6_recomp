// QosService unit tests over the in-memory FakeNet. Run: ac6net_tests qos
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "mini_test.h"
#include "vnet_fake_link.h"

using namespace ac6net;
using vnet_fake::FakeNet;
using vnet_fake::NetParams;
using vnet_fake::WaitFor;

namespace {

constexpr uint32_t kHostVip = 0x0A400020;
constexpr uint32_t kClientVip = 0x0A400021;
constexpr uint32_t kGhostVip = 0x0A400022;  // registered with the FakeNet but blackholed

constexpr uint32_t kEnable = 0x01, kDisable = 0x02, kSetData = 0x04, kSetBps = 0x08,
                   kRelease = 0x10;

XnKid Kid(uint8_t tag) {
  XnKid k;
  k.b = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, tag};
  return k;
}

// Collects callbacks; thread-safe (callbacks run on the FakeNet thread).
struct Recorder {
  std::mutex mu;
  std::vector<QosTargetResult> last;  // latest result per target
  int calls = 0;
  int done_calls = 0;
  bool done = false;

  explicit Recorder(size_t n) : last(n) {}
  QosCallback Callback() {
    return [this](size_t index, const QosTargetResult& r, bool all_done) {
      std::lock_guard<std::mutex> lk(mu);
      if (index < last.size()) last[index] = r;
      ++calls;
      if (all_done) {
        ++done_calls;
        done = true;
      }
    };
  }
  bool Done() {
    std::lock_guard<std::mutex> lk(mu);
    return done;
  }
  int Calls() {
    std::lock_guard<std::mutex> lk(mu);
    return calls;
  }
  int DoneCalls() {
    std::lock_guard<std::mutex> lk(mu);
    return done_calls;
  }
  QosTargetResult Get(size_t i) {
    std::lock_guard<std::mutex> lk(mu);
    return last[i];
  }
};

std::vector<uint8_t> Blob40() {
  std::vector<uint8_t> b(40);
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(0xA0 + i);
  return b;
}

}  // namespace

TEST_CASE("qos: listen and lookup returns the host's blob") {
  NetParams p;
  p.delay_ms = 3;
  FakeNet net(p);
  auto& host = net.AddNode(kHostVip, false, true);
  auto& client = net.AddNode(kClientVip, false, true);
  net.Start();
  const auto blob = Blob40();
  // AC6: QosListen(kid, blob, 40, 0, ENABLE|SET_DATA|SET_BITSPERSEC = 13).
  host.qos->Listen(Kid(1), blob.data(), blob.size(), 0, kEnable | kSetData | kSetBps);
  // The joining client also publishes under the host's XNKID; that must not affect the host.
  client.qos->Listen(Kid(1), blob.data(), 8, 0, kEnable | kSetData | kSetBps);

  Recorder rec(1);
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t id = client.qos->Lookup({kHostVip}, {Kid(1)}, 8, rec.Callback());
  CHECK(id > 0);
  REQUIRE(WaitFor([&] { return rec.Done(); }, 3000));
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  const QosTargetResult r = rec.Get(0);
  CHECK(r.complete);
  CHECK(r.contacted);
  CHECK(r.data_received);
  CHECK(r.data == blob);
  CHECK(r.probes_sent == 8);
  CHECK(r.probes_recv == 8);
  CHECK(r.rtt_min_ms >= 1);
  CHECK(r.rtt_min_ms <= r.rtt_med_ms);
  CHECK(r.rtt_med_ms < 200);
  CHECK(r.up_bps == 10'000'000);
  CHECK(r.dn_bps == 10'000'000);
  CHECK(rec.DoneCalls() == 1);
  CHECK(rec.Calls() >= 2);  // per-change updates before the final call
  CHECK(ms < 1500);         // completes when all 8 probes are answered, not after the 2 s timeout
}

TEST_CASE("qos: target that is not listening is contacted without data") {
  FakeNet net;
  auto& host = net.AddNode(kHostVip, false, true);
  auto& client = net.AddNode(kClientVip, false, true);
  net.Start();
  const auto blob = Blob40();
  host.qos->Listen(Kid(1), blob.data(), blob.size(), 0, kEnable | kSetData);
  host.qos->Listen(Kid(2), blob.data(), blob.size(), 0, kSetData);
  host.qos->Listen(Kid(2), nullptr, 0, 0, kDisable);
  host.qos->Listen(Kid(3), blob.data(), blob.size(), 0, kEnable | kSetData);
  host.qos->Listen(Kid(3), nullptr, 0, 0, kRelease);

  Recorder rec(3);
  // Kid 9: never registered; kid 2: disabled; kid 3: released.
  client.qos->Lookup({kHostVip, kHostVip, kHostVip}, {Kid(9), Kid(2), Kid(3)}, 4, rec.Callback());
  REQUIRE(WaitFor([&] { return rec.Done(); }, 3000));
  for (size_t i = 0; i < 3; ++i) {
    const QosTargetResult r = rec.Get(i);
    CHECK(r.complete);
    CHECK(r.contacted);
    CHECK(!r.data_received);
    CHECK(r.data.empty());
    CHECK(r.probes_recv == 4);
    CHECK(r.up_bps == 10'000'000);
  }

  // Re-enabling a disabled registration answers again.
  host.qos->Listen(Kid(2), nullptr, 0, 0, kEnable);
  Recorder rec2(1);
  client.qos->Lookup({kHostVip}, {Kid(2)}, 2, rec2.Callback());
  REQUIRE(WaitFor([&] { return rec2.Done(); }, 3000));
  CHECK(rec2.Get(0).data_received);
  CHECK(rec2.Get(0).data == blob);
}

TEST_CASE("qos: unresolvable and unreachable targets complete without contact") {
  FakeNet net;
  auto& host = net.AddNode(kHostVip, false, true);
  auto& client = net.AddNode(kClientVip, false, true);
  net.AddNode(kGhostVip, false, true);
  net.Start();
  net.SetBlackhole(kGhostVip, true);
  const auto blob = Blob40();
  host.qos->Listen(Kid(1), blob.data(), blob.size(), 0, kEnable | kSetData);

  // vip 0 = unresolvable: completes on the next tick with contacted = false.
  Recorder quick(1);
  const auto t0 = std::chrono::steady_clock::now();
  client.qos->Lookup({0}, {Kid(1)}, 8, quick.Callback());
  REQUIRE(WaitFor([&] { return quick.Done(); }, 500));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
  CHECK(quick.Get(0).complete);
  CHECK(!quick.Get(0).contacted);
  CHECK(quick.Get(0).up_bps == 0);

  // Mixed lookup: the reachable host answers; the silent target times out 2 s after its last
  // probe (8 probes at 50 ms -> about 2.35 s); all_done comes once, at the end.
  Recorder mixed(3);
  const auto t1 = std::chrono::steady_clock::now();
  client.qos->Lookup({kHostVip, kGhostVip, 0}, {Kid(1), Kid(1), Kid(1)}, 8, mixed.Callback());
  REQUIRE(WaitFor([&] { return mixed.Get(0).complete; }, 2000));
  CHECK(!mixed.Done());
  REQUIRE(WaitFor([&] { return mixed.Done(); }, 4000));
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1)
          .count();
  CHECK(ms >= 2200);
  CHECK(mixed.Get(0).data_received);
  CHECK(mixed.Get(1).complete);
  CHECK(!mixed.Get(1).contacted);
  CHECK(mixed.Get(1).probes_sent == 8);
  CHECK(mixed.Get(1).probes_recv == 0);
  CHECK(!mixed.Get(2).contacted);
  CHECK(mixed.DoneCalls() == 1);

  // A peer that is reported lost completes at once.
  Recorder lost(1);
  client.qos->Lookup({kGhostVip}, {Kid(1)}, 8, lost.Callback());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  client.qos->OnPeerLost(kGhostVip);
  REQUIRE(WaitFor([&] { return lost.Done(); }, 200));
  CHECK(!lost.Get(0).contacted);

  // Bad arguments.
  CHECK(client.qos->Lookup({}, {}, 8, lost.Callback()) == 0);
  CHECK(client.qos->Lookup({kHostVip}, {Kid(1)}, 8, nullptr) == 0);
}

TEST_CASE("qos: cancel stops callbacks") {
  FakeNet net;
  auto& client = net.AddNode(kClientVip, false, true);
  net.AddNode(kGhostVip, false, true);
  net.Start();
  net.SetBlackhole(kGhostVip, true);
  Recorder rec(1);
  const uint64_t id = client.qos->Lookup({kGhostVip}, {Kid(1)}, 8, rec.Callback());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.qos->Cancel(id);
  const int calls = rec.Calls();
  std::this_thread::sleep_for(std::chrono::milliseconds(2600));
  CHECK(rec.Calls() == calls);
  CHECK(!rec.Done());
  client.qos->Cancel(id);      // idempotent
  client.qos->Cancel(424242);  // unknown id

  // Cancel from inside a callback suppresses the rest of that lookup's calls.
  FakeNet net2;
  auto& h2 = net2.AddNode(kHostVip, false, true);
  auto& c2 = net2.AddNode(kClientVip, false, true);
  net2.Start();
  const auto blob = Blob40();
  h2.qos->Listen(Kid(5), blob.data(), blob.size(), 0, kEnable | kSetData);
  std::atomic<int> after_cancel{0};
  std::atomic<uint64_t> lookup_id{0};
  std::atomic<bool> cancelled{false};
  lookup_id = c2.qos->Lookup({kHostVip}, {Kid(5)}, 8, [&](size_t, const QosTargetResult&, bool) {
    if (cancelled) {
      ++after_cancel;
      return;
    }
    cancelled = true;
    c2.qos->Cancel(lookup_id);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  CHECK(cancelled);
  CHECK(after_cancel == 0);
}

TEST_CASE("qos: lookup of own vip and malformed packets") {
  FakeNet net;
  auto& host = net.AddNode(kHostVip, false, true);
  net.Start();
  const auto blob = Blob40();
  host.qos->Listen(Kid(1), blob.data(), blob.size(), 0, kEnable | kSetData);
  const uint64_t sent_before = net.packets_sent();
  Recorder rec(2);
  host.qos->Lookup({kHostVip, kLoopback}, {Kid(1), Kid(1)}, 3, rec.Callback());
  REQUIRE(WaitFor([&] { return rec.Done(); }, 1000));
  CHECK(rec.Get(0).data == blob);
  CHECK(rec.Get(1).data == blob);
  CHECK(net.packets_sent() == sent_before);

  // Garbage, truncated and spoofed responses are ignored.
  std::vector<uint8_t> pkt(64, 0);
  for (size_t len = 0; len <= pkt.size(); ++len) {
    for (uint8_t type : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(9)}) {
      if (len) pkt[0] = type;
      host.qos->OnLinkData(kClientVip, len ? pkt.data() : nullptr, len);
    }
  }
  pkt.assign(24, 0);
  pkt[0] = 2;
  pkt[22] = 0xFF;  // data_len 255 > remaining bytes
  host.qos->OnLinkData(kClientVip, pkt.data(), pkt.size());
}
