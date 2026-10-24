/**
 * @file        netplay_net_test.cpp
 * @brief       Standalone unit tests for the AC6 netplay NetDll/XNet glue helpers
 *              (netplay_net_marshal.h, netplay_net_qos.h).
 *
 * Not part of the SDK build (it is not in any CMake source list). Build and run it on its own:
 *
 *   clang++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
 *     -I/Users/cole/AC6_recomp_netplay/netplay/core/include \
 *     src/kernel/xam/netplay_net_test.cpp -o <build-dir>/netplay_net_test -lpthread
 *   <build-dir>/netplay_net_test
 *
 * Only ac6net headers are needed: the helpers never call into the ac6net library.
 *
 * @license     BSD 3-Clause License
 */

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "netplay_net_marshal.h"
#include "netplay_net_qos.h"

namespace np = rex::kernel::xam::netplay_net;
namespace wsa = ac6net::wsa;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      ++g_failures;                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// Guest heap over a host arena, mapped at a fixed guest base. Bump allocation, zero-filled.
class FakeHeap final : public np::GuestHeap {
 public:
  static constexpr uint32_t kBase = 0x40000000;
  explicit FakeHeap(size_t size = 1 << 16) : arena_(size, 0xCD) {}

  uint32_t Alloc(uint32_t size) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fail_allocs_)
      return 0;
    const uint32_t aligned = (next_ + 31) & ~31u;
    if (aligned + size > arena_.size())
      return 0;
    std::memset(arena_.data() + aligned, 0, size);
    next_ = aligned + size;
    ++live_;
    return kBase + aligned;
  }
  void Free(uint32_t guest_addr) override {
    std::lock_guard<std::mutex> lock(mutex_);
    freed_.push_back(guest_addr);
    --live_;
  }
  uint8_t* Translate(uint32_t guest_addr) override { return arena_.data() + (guest_addr - kBase); }

  void set_fail_allocs(bool fail) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_allocs_ = fail;
  }
  int live() const { return live_; }
  const std::vector<uint32_t>& freed() const { return freed_; }

 private:
  std::mutex mutex_;
  std::vector<uint8_t> arena_;
  uint32_t next_ = 0;
  int live_ = 0;
  bool fail_allocs_ = false;
  std::vector<uint32_t> freed_;
};

void TestHandles() {
  CHECK(!np::IsVnetHandle(0));
  CHECK(!np::IsVnetHandle(0x0FFF));
  CHECK(np::IsVnetHandle(0x1000));
  CHECK(np::IsVnetHandle(0x1001));
  CHECK(np::IsVnetHandle(0x0FFFFFFF));
  CHECK(!np::IsVnetHandle(0x10000000));
  CHECK(!np::IsVnetHandle(0xF8000004));  // XObject handles
  CHECK(!np::IsVnetHandle(0xFFFFFFFF));  // INVALID_SOCKET
  CHECK_EQ(np::HandleFromId(1), 0x1001u);
  CHECK_EQ(np::HandleFromId(0), 0u);
  CHECK_EQ(np::HandleFromId(-5), 0u);
  CHECK_EQ(np::HandleFromId(0x0FFFEFFF), 0x0FFFFFFFu);
  CHECK_EQ(np::HandleFromId(0x0FFFF000), 0u);
  CHECK_EQ(np::IdFromHandle(np::HandleFromId(42)), 42);
  CHECK_EQ(np::IdFromHandle(0x1000), 0);
}

void TestBigEndian() {
  uint8_t b[8];
  np::StoreBe16(b, 0x03E7);
  CHECK(b[0] == 0x03 && b[1] == 0xE7);
  CHECK_EQ(np::LoadBe16(b), 0x03E7);
  np::StoreBe32(b, 0x0A400007);
  CHECK(b[0] == 0x0A && b[1] == 0x40 && b[2] == 0x00 && b[3] == 0x07);
  CHECK_EQ(np::LoadBe32(b), 0x0A400007u);
  np::StoreBe64(b, 0x0102030405060708ull);
  CHECK(b[0] == 1 && b[7] == 8);
  CHECK_EQ(np::LoadBe64(b), 0x0102030405060708ull);
}

void TestGuestRange() {
  CHECK(np::GuestRangeOk(0, 0));
  CHECK(!np::GuestRangeOk(0, 1));
  CHECK(np::GuestRangeOk(0x80000000, 16));
  CHECK(np::GuestRangeOk(0xFFFFFFF0, 16));
  CHECK(!np::GuestRangeOk(0xFFFFFFF1, 16));
  CHECK(!np::GuestRangeOk(0x10, uint64_t{1} << 32));
}

void TestSockaddr() {
  // {AF_INET, 999, 10.64.0.7}
  const uint8_t good[16] = {0x00, 0x02, 0x03, 0xE7, 0x0A, 0x40, 0x00, 0x07};
  ac6net::vnet::Endpoint ep;
  CHECK_EQ(np::ParseSockaddrIn(good, 16, &ep), wsa::kOk);
  CHECK_EQ(ep.port, 999);
  CHECK_EQ(ep.ip, 0x0A400007u);
  CHECK_EQ(np::ParseSockaddrIn(good, 15, &ep), wsa::kFault);
  CHECK_EQ(np::ParseSockaddrIn(nullptr, 16, &ep), wsa::kFault);
  uint8_t bad_family[16] = {0x02, 0x00};  // host-order family, as the old XSocket path saw it
  CHECK_EQ(np::ParseSockaddrIn(bad_family, 16, &ep), wsa::kAfNoSupport);

  uint8_t out[16];
  std::memset(out, 0xEE, sizeof(out));
  np::StoreSockaddrIn(out, {0x0A400009, 21000});
  const uint8_t expect[16] = {0x00, 0x02, 0x52, 0x08, 0x0A, 0x40, 0x00, 0x09};
  CHECK(std::memcmp(out, expect, 16) == 0);

  CHECK_EQ(np::CheckSockaddrOut(false, false, 0), wsa::kOk);
  CHECK_EQ(np::CheckSockaddrOut(true, false, 0), wsa::kFault);
  CHECK_EQ(np::CheckSockaddrOut(true, true, 15), wsa::kFault);
  CHECK_EQ(np::CheckSockaddrOut(true, true, 16), wsa::kOk);
}

void TestXnAddr() {
  ac6net::XnAddr a;
  a.ina = 0x0A400007;
  a.ina_online = 0x0A400007;
  a.port_online = 1000;
  a.ab_enet = {0x00, 0x22, 0x48, 0x00, 0x00, 0x07};
  for (size_t i = 0; i < a.ab_online.size(); ++i)
    a.ab_online[i] = static_cast<uint8_t>(i + 1);
  uint8_t g[36];
  np::WriteXnAddr(g, a);
  CHECK(g[0] == 0x0A && g[3] == 0x07 && g[4] == 0x0A && g[7] == 0x07);
  CHECK(g[8] == 0x03 && g[9] == 0xE8);  // wPortOnline 1000 BE
  CHECK(g[10] == 0x00 && g[11] == 0x22 && g[12] == 0x48 && g[15] == 0x07);
  CHECK(g[16] == 1 && g[35] == 20);
  CHECK(np::ReadXnAddr(g) == a);

  ac6net::XnKid kid;
  kid.b = {0x80, 1, 2, 3, 4, 5, 6, 7};
  uint8_t k[8];
  np::WriteXnKid(k, kid);
  CHECK(k[0] == 0x80 && k[7] == 7);
  CHECK(np::ReadXnKid(k) == kid);
  np::MakeSystemLinkKid(&kid);
  CHECK_EQ(kid.b[0], 0x00);
}

void TestFdSets() {
  std::vector<uint8_t> set(np::kFdSetBytes, 0);
  np::StoreBe32(set.data(), 3);
  np::StoreBe32(set.data() + 4, 0x1002);
  np::StoreBe32(set.data() + 8, 0x1005);
  np::StoreBe32(set.data() + 12, 0x1002);
  std::vector<uint32_t> handles;
  CHECK_EQ(np::ReadFdSet(set.data(), &handles), wsa::kOk);
  CHECK((handles == std::vector<uint32_t>{0x1002, 0x1005, 0x1002}));

  std::vector<ac6net::vnet::SocketId> ids;
  CHECK_EQ(np::HandlesToIds(handles, &ids), wsa::kOk);
  CHECK((ids == std::vector<ac6net::vnet::SocketId>{2, 5, 2}));
  CHECK_EQ(np::HandlesToIds({0x1002, 0xF8000010}, &ids), wsa::kNotSock);

  // Ready = {2}: both copies of 0x1002 stay, in order; 0x1005 goes.
  const auto ready = np::FilterReady(handles, {2});
  CHECK((ready == std::vector<uint32_t>{0x1002, 0x1002}));
  np::WriteFdSet(set.data(), ready);
  CHECK_EQ(np::LoadBe32(set.data()), 2u);
  CHECK_EQ(np::LoadBe32(set.data() + 8), 0x1002u);
  CHECK(np::FilterReady(handles, {}).empty());

  np::StoreBe32(set.data(), 64);
  CHECK_EQ(np::ReadFdSet(set.data(), &handles), wsa::kOk);
  CHECK_EQ(handles.size(), 64u);
  np::StoreBe32(set.data(), 65);
  CHECK_EQ(np::ReadFdSet(set.data(), &handles), wsa::kInval);
  np::StoreBe32(set.data(), 0xFFFFFFFF);
  CHECK_EQ(np::ReadFdSet(set.data(), &handles), wsa::kInval);
  CHECK(handles.empty());

  int64_t us = 123;
  CHECK_EQ(np::TimevalToMicros(0, 0, &us), wsa::kOk);
  CHECK_EQ(us, 0);
  CHECK_EQ(np::TimevalToMicros(1, 500, &us), wsa::kOk);
  CHECK_EQ(us, 1000500);
  CHECK_EQ(np::TimevalToMicros(-1, 0, &us), wsa::kInval);
  CHECK_EQ(np::TimevalToMicros(0, -1, &us), wsa::kInval);
}

std::vector<uint8_t> MakeFdSet(const std::vector<uint32_t>& handles) {
  std::vector<uint8_t> set(np::kFdSetBytes, 0);
  np::StoreBe32(set.data(), static_cast<uint32_t>(handles.size()));
  for (size_t i = 0; i < handles.size(); ++i)
    np::StoreBe32(set.data() + 4 + 4 * i, handles[i]);
  return set;
}

void TestSelectPlan() {
  // AC6's read poll: one TCP socket in the read set, write/except NULL.
  auto rd = MakeFdSet({0x1003});
  uint8_t* sets[3] = {rd.data(), nullptr, nullptr};
  np::SelectPlan plan = np::PlanSelect(sets);
  CHECK(plan.vnet);
  CHECK_EQ(plan.err, wsa::kOk);
  CHECK((plan.query.read == std::vector<ac6net::vnet::SocketId>{3}));
  CHECK(plan.query.write.empty() && plan.query.except.empty());
  ac6net::vnet::SelectSets ready;
  ready.read = {3};
  CHECK_EQ(np::FinishSelect(sets, plan, ready), 1);
  CHECK_EQ(np::LoadBe32(rd.data()), 1u);
  CHECK_EQ(np::LoadBe32(rd.data() + 4), 0x1003u);
  // Timeout: nothing ready, the set is emptied.
  CHECK_EQ(np::FinishSelect(sets, plan, ac6net::vnet::SelectSets{}), 0);
  CHECK_EQ(np::LoadBe32(rd.data()), 0u);

  // Read + write + except, counted across sets.
  auto r2 = MakeFdSet({0x1001, 0x1002});
  auto w2 = MakeFdSet({0x1002});
  auto e2 = MakeFdSet({0x1004});
  uint8_t* sets2[3] = {r2.data(), w2.data(), e2.data()};
  plan = np::PlanSelect(sets2);
  CHECK(plan.vnet && plan.err == wsa::kOk);
  ready = {};
  ready.read = {2};
  ready.write = {2};
  ready.except = {4};
  CHECK_EQ(np::FinishSelect(sets2, plan, ready), 3);
  CHECK_EQ(np::LoadBe32(r2.data()), 1u);
  CHECK_EQ(np::LoadBe32(r2.data() + 4), 0x1002u);

  // Only XSocket handles: the old path keeps the call.
  auto xs = MakeFdSet({0xF8000040});
  uint8_t* sets3[3] = {xs.data(), nullptr, nullptr};
  CHECK(!np::PlanSelect(sets3).vnet);
  // Mixed: a vnet select that names an XSocket handle fails with WSAENOTSOCK.
  auto mixed = MakeFdSet({0x1001, 0xF8000040});
  uint8_t* sets4[3] = {mixed.data(), nullptr, nullptr};
  plan = np::PlanSelect(sets4);
  CHECK(plan.vnet);
  CHECK_EQ(plan.err, wsa::kNotSock);
  // Empty sets with netplay on still go to vnet.
  uint8_t* none[3] = {};
  CHECK(np::PlanSelect(none).vnet);
  // Corrupt count.
  auto bad = MakeFdSet({});
  np::StoreBe32(bad.data(), 1000);
  uint8_t* sets5[3] = {bad.data(), nullptr, nullptr};
  plan = np::PlanSelect(sets5);
  CHECK(plan.vnet);
  CHECK_EQ(plan.err, wsa::kInval);
}

void TestOptions() {
  // SO_SNDBUF 200000 as AC6 passes it: 4 bytes BE.
  const uint8_t guest[4] = {0x00, 0x03, 0x0D, 0x40};
  uint8_t host[4];
  np::SwapOptionValue(ac6net::vnet::kSolSocket, ac6net::vnet::kSoSndBuf, guest, host, 4, true);
  uint32_t v;
  std::memcpy(&v, host, 4);
  CHECK_EQ(v, 200000u);
  uint8_t back[4];
  np::SwapOptionValue(ac6net::vnet::kSolSocket, ac6net::vnet::kSoSndBuf, host, back, 4, false);
  CHECK(std::memcmp(back, guest, 4) == 0);

  // SO_TYPE result 1 (SOCK_STREAM) written back BE.
  const uint32_t stream = 1;
  np::SwapOptionValue(ac6net::vnet::kSolSocket, ac6net::vnet::kSoType,
                      reinterpret_cast<const uint8_t*>(&stream), back, 4, false);
  CHECK(back[0] == 0 && back[1] == 0 && back[2] == 0 && back[3] == 1);

  // SO_LINGER {onoff 1, linger 5} is two u16s, not one u32.
  const uint8_t linger[4] = {0x00, 0x01, 0x00, 0x05};
  np::SwapOptionValue(ac6net::vnet::kSolSocket, ac6net::vnet::kSoLinger, linger, host, 4, true);
  uint16_t pair[2];
  std::memcpy(pair, host, 4);
  CHECK(pair[0] == 1 && pair[1] == 5);

  const uint8_t one = 1;
  uint8_t one_out = 0;
  np::SwapOptionValue(ac6net::vnet::kSolSocket, ac6net::vnet::kSoReuseAddr, &one, &one_out, 1,
                      true);
  CHECK_EQ(one_out, 1);
  const uint8_t odd[3] = {1, 2, 3};
  uint8_t odd_out[3] = {};
  np::SwapOptionValue(ac6net::vnet::kSolSocket, 0x9999, odd, odd_out, 3, true);
  CHECK(std::memcmp(odd, odd_out, 3) == 0);

  CHECK(np::IsIgnoredXboxOption(0xFFFF, 0x5801));
  CHECK(np::IsIgnoredXboxOption(0xFFFF, 0x5803));
  CHECK(!np::IsIgnoredXboxOption(6, 0x5801));
  CHECK(!np::IsIgnoredXboxOption(0xFFFF, ac6net::vnet::kSoReuseAddr));
}

// Helpers to read a guest XNQOS.
struct QosView {
  uint8_t* base;
  uint32_t count() const { return np::LoadBe32(base); }
  uint32_t pending() const { return np::LoadBe32(base + 4); }
  uint8_t* info(uint32_t i) const { return base + 8 + 24 * i; }
  uint8_t flags(uint32_t i) const { return info(i)[0]; }
  uint16_t u16(uint32_t i, uint32_t off) const { return np::LoadBe16(info(i) + off); }
  uint32_t u32(uint32_t i, uint32_t off) const { return np::LoadBe32(info(i) + off); }
};

ac6net::QosTargetResult HostReply(bool complete) {
  ac6net::QosTargetResult r;
  r.contacted = true;
  r.complete = complete;
  r.data_received = complete;
  r.rtt_min_ms = 12;
  r.rtt_med_ms = 15;
  r.probes_sent = 8;
  r.probes_recv = complete ? 8 : 3;
  if (complete) {
    r.data.resize(40);
    for (size_t i = 0; i < r.data.size(); ++i)
      r.data[i] = static_cast<uint8_t>(0xA0 + i);
  }
  return r;
}

void TestQosSingleTarget() {
  FakeHeap heap;
  const uint32_t addr = heap.Alloc(np::XnQosBytes(1));
  int signals = 0;
  np::QosResults results(&heap, addr, 1, 1, [&] { ++signals; });
  results.Init();
  QosView q{heap.Translate(addr)};
  CHECK_EQ(q.count(), 1u);
  CHECK_EQ(q.pending(), 1u);
  CHECK_EQ(q.flags(0), 0);

  // Progress: contacted, not complete yet.
  results.OnResult(0, HostReply(false), false);
  CHECK_EQ(q.pending(), 1u);
  CHECK_EQ(q.flags(0), np::kQosTargetContacted);
  CHECK_EQ(q.u16(0, np::kQosInfoProbesRecv), 3);
  CHECK_EQ(signals, 0);

  // Final: the AC6 join condition (DATA_RECEIVED, cbData != 0, pbData != 0).
  results.OnResult(0, HostReply(true), true);
  CHECK_EQ(q.pending(), 0u);
  CHECK_EQ(q.flags(0), np::kQosComplete | np::kQosTargetContacted | np::kQosDataReceived);
  CHECK_EQ(q.u16(0, np::kQosInfoProbesXmit), 8);
  CHECK_EQ(q.u16(0, np::kQosInfoProbesRecv), 8);
  CHECK_EQ(q.u16(0, np::kQosInfoDataLen), 40);
  const uint32_t data_addr = q.u32(0, np::kQosInfoDataPtr);
  CHECK(data_addr != 0);
  CHECK_EQ(heap.Translate(data_addr)[0], 0xA0);
  CHECK_EQ(heap.Translate(data_addr)[39], 0xA0 + 39);
  CHECK_EQ(q.u16(0, np::kQosInfoRttMin), 12);
  CHECK_EQ(q.u16(0, np::kQosInfoRttMed), 15);
  CHECK_EQ(q.u32(0, np::kQosInfoUpBps), np::kQosNominalBps);
  CHECK_EQ(q.u32(0, np::kQosInfoDownBps), np::kQosNominalBps);
  CHECK_EQ(signals, 1);

  // A late duplicate neither reallocates nor decrements again.
  results.OnResult(0, HostReply(true), true);
  CHECK_EQ(q.u32(0, np::kQosInfoDataPtr), data_addr);
  CHECK_EQ(q.pending(), 0u);
  CHECK_EQ(signals, 1);

  // Out-of-range index is ignored.
  results.OnResult(7, HostReply(true), true);

  const auto frees = results.Release();
  CHECK((frees == std::vector<uint32_t>{data_addr, addr}));
  CHECK(results.Release().empty());
  // After release nothing is written any more.
  std::memset(q.base, 0x5A, np::XnQosBytes(1));
  results.OnResult(0, HostReply(true), true);
  CHECK_EQ(q.base[0], 0x5A);
  CHECK_EQ(q.flags(0), 0x5A);
}

void TestQosGatewaysAndAllDone() {
  FakeHeap heap;
  const uint32_t addr = heap.Alloc(np::XnQosBytes(4));
  np::QosResults results(&heap, addr, 4, 2, {});
  results.Init();
  QosView q{heap.Translate(addr)};
  CHECK_EQ(q.count(), 4u);
  CHECK_EQ(q.pending(), 2u);
  CHECK_EQ(q.flags(2), np::kQosComplete);
  CHECK_EQ(q.flags(3), np::kQosComplete);

  // Target 0 unreachable, reported complete without contact.
  ac6net::QosTargetResult none;
  none.complete = true;
  results.OnResult(0, none, false);
  CHECK_EQ(q.pending(), 1u);
  CHECK_EQ(q.flags(0), np::kQosComplete);
  CHECK_EQ(q.u32(0, np::kQosInfoUpBps), 0u);
  CHECK_EQ(q.u32(0, np::kQosInfoDataPtr), 0u);

  // all_done on target 0 again must still finish target 1, which never reported.
  results.OnResult(0, none, true);
  CHECK_EQ(q.pending(), 0u);
  CHECK(q.flags(1) & np::kQosComplete);
  CHECK(!(q.flags(1) & np::kQosTargetContacted));
  CHECK_EQ(results.Release().size(), 1u);
}

void TestQosDataLimits() {
  FakeHeap heap;
  const uint32_t addr = heap.Alloc(np::XnQosBytes(2));
  np::QosResults results(&heap, addr, 2, 2, {});
  results.Init();
  QosView q{heap.Translate(addr)};

  ac6net::QosTargetResult big = HostReply(true);
  big.data.assign(np::kMaxQosDataBytes + 1, 0x11);
  results.OnResult(0, big, false);
  CHECK_EQ(q.flags(0), np::kQosComplete | np::kQosTargetContacted);
  CHECK_EQ(q.u16(0, np::kQosInfoDataLen), 0);

  heap.set_fail_allocs(true);
  results.OnResult(1, HostReply(true), true);
  CHECK_EQ(q.flags(1), np::kQosComplete | np::kQosTargetContacted);
  CHECK_EQ(q.u32(1, np::kQosInfoDataPtr), 0u);
  CHECK_EQ(q.pending(), 0u);
  CHECK_EQ(results.Release().size(), 1u);
}

void TestQosService() {
  FakeHeap heap;
  const uint32_t addr = heap.Alloc(np::XnQosBytes(1));
  np::QosResults results(&heap, addr, 1, 1, {});
  results.Init();
  results.OnResult(0, np::MakeServiceResult(true, 70000, 0, 0), true);
  QosView q{heap.Translate(addr)};
  CHECK_EQ(q.pending(), 0u);
  CHECK_EQ(q.flags(0), np::kQosComplete | np::kQosTargetContacted);
  CHECK_EQ(q.u16(0, np::kQosInfoRttMed), 0xFFFF);
  CHECK(q.u32(0, np::kQosInfoUpBps) > 4000000u);  // AC6 tier 5

  const uint32_t addr2 = heap.Alloc(np::XnQosBytes(1));
  np::QosResults offline(&heap, addr2, 1, 1, {});
  offline.Init();
  offline.OnResult(0, np::MakeServiceResult(false, 0, 0, 0), true);
  QosView q2{heap.Translate(addr2)};
  CHECK_EQ(q2.pending(), 0u);
  CHECK_EQ(q2.flags(0), np::kQosComplete);
  CHECK_EQ(q2.u32(0, np::kQosInfoUpBps), 0u);
}

// Network-thread callbacks racing a guest Release: no write may land after Release returns,
// and every data blob must be reported for freeing exactly once. Run under ASan/TSan.
void TestQosReleaseRace() {
  for (int round = 0; round < 200; ++round) {
    FakeHeap heap(1 << 20);
    const uint32_t addr = heap.Alloc(np::XnQosBytes(8));
    np::QosResults results(&heap, addr, 8, 8, {});
    results.Init();
    std::atomic<bool> go{false};
    std::thread net([&] {
      while (!go.load()) {}
      for (int i = 0; i < 8; ++i)
        results.OnResult(i, HostReply(true), i == 7);
    });
    go.store(true);
    const auto frees = results.Release();
    net.join();
    CHECK(!frees.empty());
    CHECK_EQ(frees.back(), addr);
    // Every allocation except the ones listed for freeing must not exist: blobs allocated
    // after Release would be leaks.
    CHECK_EQ(static_cast<size_t>(heap.live()), frees.size());
  }
}

// Malformed guest input never reads out of bounds (meaningful under ASan/UBSan).
void TestFuzz() {
  std::mt19937 rng(1234);
  for (int i = 0; i < 20000; ++i) {
    std::vector<uint8_t> buf(np::kFdSetBytes);
    for (auto& b : buf)
      b = static_cast<uint8_t>(rng());
    std::vector<uint32_t> handles;
    const int err = np::ReadFdSet(buf.data(), &handles);
    CHECK(err == wsa::kOk || err == wsa::kInval);
    CHECK(handles.size() <= np::kFdSetSize);
    ac6net::vnet::Endpoint ep;
    const uint32_t len = rng() % 32;
    std::vector<uint8_t> sa(len ? len : 1);
    for (auto& b : sa)
      b = static_cast<uint8_t>(rng());
    (void)np::ParseSockaddrIn(sa.data(), len, &ep);
    (void)np::ReadXnAddr(buf.data());
    uint8_t opt_out[np::kMaxOptionBytes];
    const size_t opt_len = rng() % (np::kMaxOptionBytes + 1);
    np::SwapOptionValue(0xFFFF, static_cast<int>(rng() % 0x2000), buf.data(), opt_out, opt_len,
                        rng() & 1);
  }
}

}  // namespace

int main() {
  TestHandles();
  TestBigEndian();
  TestGuestRange();
  TestSockaddr();
  TestXnAddr();
  TestFdSets();
  TestSelectPlan();
  TestOptions();
  TestQosSingleTarget();
  TestQosGatewaysAndAllDone();
  TestQosDataLimits();
  TestQosService();
  TestQosReleaseRace();
  TestFuzz();
  std::printf("netplay_net_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
