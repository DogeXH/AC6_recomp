// vnet unit tests: guest socket semantics over the in-memory FakeNet (no game, no real sockets).
// Run: ac6net_tests vnet
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mini_test.h"
#include "vnet_fake_link.h"
#include "vnet_wire.h"

using namespace ac6net;
using namespace ac6net::vnet;
using vnet_fake::FakeNet;
using vnet_fake::NetParams;
using vnet_fake::WaitFor;

namespace {

constexpr uint32_t kVipA = 0x0A400001;
constexpr uint32_t kVipB = 0x0A400002;
constexpr uint32_t kVipC = 0x0A400003;

using Ms = std::chrono::milliseconds;

int64_t ElapsedMs(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - t0).count();
}

SocketId MakeSocket(VNet& v, int type, int proto, bool non_blocking = true) {
  int err = -1;
  const SocketId s = v.Socket(type, proto, &err);
  REQUIRE(s > 0 && err == wsa::kOk);
  if (non_blocking) REQUIRE(v.SetNonBlocking(s, true, &err) == 0);
  return s;
}

SocketId Udp(VNet& v, uint16_t port = 0, int proto = kProtoUdp, uint32_t ip = kInaddrAny) {
  const SocketId s = MakeSocket(v, kSockDgram, proto);
  if (port) {
    int err = -1;
    REQUIRE(v.Bind(s, {ip, port}, &err) == 0);
  }
  return s;
}

SocketId TcpListen(VNet& v, uint16_t port, int backlog = 16) {
  const SocketId s = MakeSocket(v, kSockStream, 0);
  int err = -1;
  REQUIRE(v.Bind(s, {kInaddrAny, port}, &err) == 0);
  REQUIRE(v.Listen(s, backlog, &err) == 0);
  return s;
}

// Non-blocking connect: must return -1 / WSAEWOULDBLOCK, like AC6 expects.
SocketId TcpConnect(VNet& v, uint32_t ip, uint16_t port) {
  const SocketId s = MakeSocket(v, kSockStream, 0);
  int err = -1;
  REQUIRE(v.Connect(s, {ip, port}, &err) == -1);
  REQUIRE(err == wsa::kWouldBlock);
  return s;
}

int SelectOne(VNet& v, SocketId s, char which, int64_t timeout_us) {
  SelectSets sets;
  (which == 'r' ? sets.read : which == 'w' ? sets.write : sets.except).push_back(s);
  int err = -1;
  return v.Select(&sets, timeout_us, &err);
}

SocketId AcceptWait(VNet& v, SocketId l, Endpoint* from, int timeout_ms) {
  if (SelectOne(v, l, 'r', int64_t(timeout_ms) * 1000) != 1) return -1;
  int err = -1;
  return v.Accept(l, from, &err);
}

struct TcpPair {
  SocketId client = 0;    // on the connecting VNet
  SocketId accepted = 0;  // on the listening VNet
  SocketId listener = 0;
};

TcpPair Connect(VNet& client_vnet, VNet& server_vnet, uint32_t server_ip, uint16_t port) {
  TcpPair p;
  p.listener = TcpListen(server_vnet, port);
  p.client = TcpConnect(client_vnet, server_ip, port);
  REQUIRE(SelectOne(client_vnet, p.client, 'w', 5'000'000) == 1);
  Endpoint from;
  p.accepted = AcceptWait(server_vnet, p.listener, &from, 5000);
  REQUIRE(p.accepted > 0);
  return p;
}

// Sends all of `data` (non-blocking socket), waiting for writability on WOULDBLOCK.
bool SendAll(VNet& v, SocketId s, const uint8_t* data, size_t len, int timeout_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  size_t off = 0;
  while (off < len) {
    int err = -1;
    const int n = v.Send(s, data + off, len - off, 0, &err);
    if (n > 0) {
      off += size_t(n);
      continue;
    }
    if (err != wsa::kWouldBlock || ElapsedMs(t0) > timeout_ms) return false;
    SelectOne(v, s, 'w', 20'000);
  }
  return true;
}

// Receives exactly `len` bytes (non-blocking socket).
bool RecvExactly(VNet& v, SocketId s, uint8_t* out, size_t len, int timeout_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  size_t off = 0;
  while (off < len) {
    int err = -1;
    const int n = v.Recv(s, out + off, len - off, 0, &err);
    if (n > 0) {
      off += size_t(n);
      continue;
    }
    if (n == 0 || err != wsa::kWouldBlock || ElapsedMs(t0) > timeout_ms) return false;
    SelectOne(v, s, 'r', 20'000);
  }
  return true;
}

std::vector<uint8_t> Pattern(size_t len, uint32_t seed) {
  std::vector<uint8_t> v(len);
  std::mt19937 rng(seed);
  for (auto& b : v) b = uint8_t(rng());
  return v;
}

int GetInt(VNet& v, SocketId s, int level, int opt, int* err) {
  int32_t value = -1;
  size_t len = sizeof(value);
  if (v.GetOption(s, level, opt, &value, &len, err) != 0) return -1;
  return value;
}

int SetInt(VNet& v, SocketId s, int level, int opt, int32_t value) {
  int err = -1;
  return v.SetOption(s, level, opt, &value, sizeof(value), &err) == 0 ? 0 : err;
}

}  // namespace

// ---------------------------------------------------------------------------------- datagrams

TEST_CASE("vnet: udp both ways, source port 999 preserved") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  const SocketId sa = Udp(*a.vnet, 999, kProtoVdp);
  const SocketId sb = Udp(*b.vnet, 999, kProtoVdp);
  int err = -1;
  uint8_t buf[1281];
  Endpoint from;

  CHECK(a.vnet->SendTo(sa, "probe", 5, 0, {kVipB, 999}, &err) == 5);
  REQUIRE(
      WaitFor([&] { return b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == 5; }, 1000));
  CHECK(from.ip == kVipA);
  CHECK(from.port == 999);
  CHECK(std::memcmp(buf, "probe", 5) == 0);

  CHECK(b.vnet->SendTo(sb, "pong", 4, 0, from, &err) == 4);  // reply to the source
  REQUIRE(
      WaitFor([&] { return a.vnet->RecvFrom(sa, buf, sizeof(buf), 0, &from, &err) == 4; }, 1000));
  CHECK(from.ip == kVipB);
  CHECK(from.port == 999);

  // An unbound socket gets an implicit ephemeral bind; the receiver sees that port.
  const SocketId su = Udp(*a.vnet);
  Endpoint local;
  CHECK(a.vnet->GetSockName(su, &local, &err) == -1);
  CHECK(err == wsa::kInval);
  CHECK(a.vnet->RecvFrom(su, buf, sizeof(buf), 0, &from, &err) == -1);
  CHECK(err == wsa::kInval);  // Winsock: recvfrom on an unbound socket
  CHECK(a.vnet->SendTo(su, "x", 1, 0, {kVipB, 999}, &err) == 1);
  CHECK(a.vnet->GetSockName(su, &local, &err) == 0);
  CHECK(local.ip == kInaddrAny);
  CHECK(local.port >= 49152);
  REQUIRE(
      WaitFor([&] { return b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == 1; }, 1000));
  CHECK(from.ip == kVipA);
  CHECK(from.port == local.port);

  // Empty queue, truncation, peek, zero-length datagram.
  CHECK(b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  CHECK(a.vnet->SendTo(sa, "abcdef", 6, 0, {kVipB, 999}, &err) == 6);
  REQUIRE(WaitFor(
      [&] { return b.vnet->RecvFrom(sb, buf, sizeof(buf), 2 /*MSG_PEEK*/, &from, &err) == 6; },
      1000));
  uint32_t avail = 0;
  CHECK(b.vnet->Available(sb, &avail, &err) == 0);
  CHECK(avail == 6);
  CHECK(b.vnet->RecvFrom(sb, buf, 3, 0, &from, &err) == -1);
  CHECK(err == wsa::kMsgSize);
  CHECK(std::memcmp(buf, "abc", 3) == 0);
  CHECK(b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == -1);  // rest was discarded
  CHECK(err == wsa::kWouldBlock);
  CHECK(a.vnet->SendTo(sa, nullptr, 0, 0, {kVipB, 999}, &err) == 0);
  REQUIRE(
      WaitFor([&] { return b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == 0; }, 1000));
  CHECK(err == wsa::kOk);

  // Invalid destinations.
  CHECK(a.vnet->SendTo(sa, "x", 1, 0, {kInaddrAny, 999}, &err) == -1);
  CHECK(err == wsa::kAddrNotAvail);
  CHECK(a.vnet->SendTo(sa, "x", 1, 0, {kVipB, 0}, &err) == -1);
  CHECK(err == wsa::kAddrNotAvail);
}

TEST_CASE("vnet: udp fragmentation of 2000-byte datagrams") {
  NetParams p;
  p.jitter_ms = 6;  // reorders fragments
  FakeNet net(p);
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  const SocketId sa = Udp(*a.vnet, 20000);
  const SocketId sb = Udp(*b.vnet, 20001);
  const size_t per = p.max_payload - wire::kDgramHeader;  // 1191
  int err = -1;
  std::vector<uint8_t> buf(16384);
  Endpoint from;
  for (size_t len : {size_t(2000), per, per + 1, size_t(4096), per * 8}) {
    const auto data = Pattern(len, uint32_t(len));
    const uint64_t sent_before = net.packets_sent();
    CHECK(a.vnet->SendTo(sa, data.data(), data.size(), 0, {kVipB, 20001}, &err) == int(len));
    CHECK(net.packets_sent() - sent_before == (len + per - 1) / per);
    int n = -1;
    REQUIRE(WaitFor(
        [&] { return (n = b.vnet->RecvFrom(sb, buf.data(), buf.size(), 0, &from, &err)) >= 0; },
        2000));
    CHECK(size_t(n) == len);
    CHECK(std::memcmp(buf.data(), data.data(), len) == 0);
    CHECK(from.ip == kVipA);
    CHECK(from.port == 20000);
  }
  // More than 8 fragments is refused.
  const auto big = Pattern(per * 8 + 1, 1);
  CHECK(a.vnet->SendTo(sa, big.data(), big.size(), 0, {kVipB, 20001}, &err) == -1);
  CHECK(err == wsa::kMsgSize);

  // A lost fragment: the partial datagram is dropped after 2 s and nothing is delivered.
  uint8_t frag[64] = {};
  wire::DgramHeader h;
  h.src_port = 20000;
  h.dst_port = 20001;
  h.msg_id = 0x7777;
  h.frag_idx = 0;
  h.frag_cnt = 2;
  wire::EncodeDgramHeader(h, frag);
  const uint64_t dropped = b.vnet->stats().dgrams_dropped;
  b.vnet->OnLinkData(kVipA, uint8_t(Channel::kDatagram), frag, sizeof(frag));
  REQUIRE(WaitFor([&] { return b.vnet->stats().dgrams_dropped == dropped + 1; }, 3000));
  CHECK(b.vnet->RecvFrom(sb, buf.data(), buf.size(), 0, &from, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
}

TEST_CASE("vnet: udp receive queue overflow drops beyond 256") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  const SocketId sa = Udp(*a.vnet);
  const SocketId sb = Udp(*b.vnet, 20000);
  int err = -1;
  for (int i = 0; i < 300; ++i) {
    const uint32_t v = uint32_t(i);
    CHECK(a.vnet->SendTo(sa, &v, sizeof(v), 0, {kVipB, 20000}, &err) == 4);
  }
  REQUIRE(WaitFor(
      [&] {
        const Stats s = b.vnet->stats();
        return s.dgrams_recv + s.dgrams_dropped >= 300;
      },
      3000));
  CHECK(b.vnet->stats().dgrams_recv == 256);
  CHECK(b.vnet->stats().dgrams_dropped == 44);
  int got = 0;
  uint32_t v = 0;
  Endpoint from;
  while (b.vnet->RecvFrom(sb, &v, sizeof(v), 0, &from, &err) == 4) ++got;
  CHECK(got == 256);
  CHECK(err == wsa::kWouldBlock);

  // Loopback obeys the same limit.
  const SocketId sl = Udp(*b.vnet);
  for (int i = 0; i < 260; ++i) b.vnet->SendTo(sl, &v, sizeof(v), 0, {kVipB, 20000}, &err);
  got = 0;
  while (b.vnet->RecvFrom(sb, &v, sizeof(v), 0, &from, &err) == 4) ++got;
  CHECK(got == 256);
}

TEST_CASE("vnet: udp loopback to own vip and 127.0.0.1") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  net.Start();
  VNet& v = *a.vnet;
  const SocketId rx = Udp(v, 999, kProtoVdp);
  const SocketId tx = Udp(v, 5000);
  const uint64_t sent_before = net.packets_sent();
  int err = -1;
  uint8_t buf[64];
  Endpoint from;

  CHECK(v.SendTo(tx, "own", 3, 0, {kVipA, 999}, &err) == 3);
  CHECK(v.RecvFrom(rx, buf, sizeof(buf), 0, &from, &err) == 3);  // delivered synchronously
  CHECK(from.ip == kVipA);
  CHECK(from.port == 5000);

  CHECK(v.SendTo(tx, "lo", 2, 0, {kLoopback, 999}, &err) == 2);
  CHECK(v.RecvFrom(rx, buf, sizeof(buf), 0, &from, &err) == 2);
  CHECK(from.ip == kLoopback);
  CHECK(from.port == 5000);

  // A socket bound to 127.0.0.1 does not receive datagrams addressed to the vip.
  const SocketId lo_only = Udp(v, 6000, kProtoUdp, kLoopback);
  CHECK(v.SendTo(tx, "x", 1, 0, {kVipA, 6000}, &err) == 1);
  CHECK(v.RecvFrom(lo_only, buf, sizeof(buf), 0, &from, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  CHECK(v.SendTo(tx, "y", 1, 0, {kLoopback, 6000}, &err) == 1);
  CHECK(v.RecvFrom(lo_only, buf, sizeof(buf), 0, &from, &err) == 1);
  CHECK(net.packets_sent() == sent_before);  // nothing touched the link
}

TEST_CASE("vnet: udp broadcast is dropped and counted") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  const SocketId s = Udp(*a.vnet, 1001);
  const SocketId r = Udp(*b.vnet, 1001);
  int err = -1;
  CHECK(a.vnet->SendTo(s, "hi", 2, 0, {kInaddrBroadcast, 1001}, &err) == -1);
  CHECK(err == wsa::kAccess);  // Winsock needs SO_BROADCAST first
  CHECK(SetInt(*a.vnet, s, kSolSocket, kSoBroadcast, 1) == 0);
  const uint64_t sent_before = net.packets_sent();
  const uint64_t dropped = a.vnet->stats().dgrams_dropped;
  CHECK(a.vnet->SendTo(s, "hi", 2, 0, {kInaddrBroadcast, 1001}, &err) == 2);
  CHECK(a.vnet->stats().dgrams_dropped == dropped + 1);
  CHECK(net.packets_sent() == sent_before);
  std::this_thread::sleep_for(Ms(20));
  uint8_t buf[8];
  Endpoint from;
  CHECK(b.vnet->RecvFrom(r, buf, sizeof(buf), 0, &from, &err) == -1);
}

TEST_CASE("vnet: udp connect sets the default destination and filters sources") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  auto& c = net.AddNode(kVipC);
  net.Start();
  int err = -1;
  const SocketId sb = Udp(*b.vnet, 999);
  const SocketId sa = Udp(*a.vnet, 999);
  const SocketId sc = Udp(*c.vnet, 999);
  CHECK(a.vnet->Send(sa, "x", 1, 0, &err) == -1);
  CHECK(err == wsa::kNotConn);
  CHECK(a.vnet->Connect(sa, {kVipB, 999}, &err) == 0);
  Endpoint peer;
  CHECK(a.vnet->GetPeerName(sa, &peer, &err) == 0);
  CHECK(peer.ip == kVipB);
  CHECK(a.vnet->Send(sa, "hi", 2, 0, &err) == 2);
  uint8_t buf[8];
  Endpoint from;
  REQUIRE(
      WaitFor([&] { return b.vnet->RecvFrom(sb, buf, sizeof(buf), 0, &from, &err) == 2; }, 1000));
  CHECK(from.ip == kVipA);
  // Only the connected peer's datagrams reach a connected socket.
  CHECK(c.vnet->SendTo(sc, "no", 2, 0, {kVipA, 999}, &err) == 2);
  CHECK(b.vnet->SendTo(sb, "ok", 2, 0, {kVipA, 999}, &err) == 2);
  REQUIRE(WaitFor([&] { return a.vnet->Recv(sa, buf, sizeof(buf), 0, &err) == 2; }, 1000));
  CHECK(std::memcmp(buf, "ok", 2) == 0);
  std::this_thread::sleep_for(Ms(20));
  CHECK(a.vnet->Recv(sa, buf, sizeof(buf), 0, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  // Connecting to 0.0.0.0:0 dissolves the association.
  CHECK(a.vnet->Connect(sa, {kInaddrAny, 0}, &err) == 0);
  CHECK(a.vnet->GetPeerName(sa, &peer, &err) == -1);
  CHECK(err == wsa::kNotConn);
}

// ---------------------------------------------------------------------------------- streams

TEST_CASE("vnet: tcp connect, accept and 1 MiB transfer under 10% loss and reorder") {
  NetParams p;
  p.loss = 0.10;
  p.delay_ms = 2;
  p.jitter_ms = 8;
  FakeNet net(p);
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;

  const SocketId l = TcpListen(vb, 21000);
  const SocketId c = TcpConnect(va, kVipB, 21000);
  REQUIRE(SelectOne(va, c, 'w', 5'000'000) == 1);
  Endpoint from, local;
  const SocketId s = AcceptWait(vb, l, &from, 5000);
  REQUIRE(s > 0);
  int err = -1;
  CHECK(va.GetSockName(c, &local, &err) == 0);
  CHECK(from.ip == kVipA);
  CHECK(from.port == local.port);

  const size_t total = 1 << 20;
  const auto data = Pattern(total, 42);
  std::vector<uint8_t> got(total);
  std::mt19937 rng(7);
  size_t sent = 0, recvd = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (recvd < total && ElapsedMs(t0) < 30000) {
    bool progress = false;
    if (sent < total) {
      const size_t chunk = std::min<size_t>(total - sent, 1 + rng() % 2002);
      const int n = va.Send(c, data.data() + sent, chunk, 0, &err);
      if (n > 0) {
        CHECK(size_t(n) == chunk);  // no partial sends below the 32 KiB cap
        sent += size_t(n);
        progress = true;
      } else {
        CHECK(err == wsa::kWouldBlock);
      }
    }
    const int n = vb.Recv(s, got.data() + recvd, total - recvd, 0, &err);
    if (n > 0) {
      recvd += size_t(n);
      progress = true;
    } else {
      CHECK(n == -1);
      CHECK(err == wsa::kWouldBlock);
    }
    if (!progress) {
      SelectSets sets;
      sets.read.push_back(s);
      vb.Select(&sets, 5'000, &err);
    }
  }
  std::fprintf(stderr, "  1 MiB under 10%% loss: %lld ms, %llu link packets\n",
               (long long)ElapsedMs(t0), (unsigned long long)net.packets_sent());
  REQUIRE(recvd == total);
  CHECK(got == data);
  CHECK(va.stats().stream_bytes_sent == total);
  CHECK(vb.stats().stream_bytes_recv == total);

  // And back the other way.
  const auto back = Pattern(64 * 1024, 43);
  std::vector<uint8_t> back_got(back.size());
  std::thread sender([&] { SendAll(vb, s, back.data(), back.size(), 20000); });
  CHECK(RecvExactly(va, c, back_got.data(), back_got.size(), 20000));
  sender.join();
  CHECK(back_got == back);
}

TEST_CASE("vnet: tcp framing invariant: recv(1), recv(1), recv(n) after select") {
  NetParams p;
  p.loss = 0.10;
  p.jitter_ms = 8;
  FakeNet net(p);
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  TcpPair t = Connect(*a.vnet, *b.vnet, kVipB, 21000);
  VNet& tx = *a.vnet;
  VNet& rx = *b.vnet;

  constexpr int kFrames = 1500;
  std::atomic<bool> send_ok{true};
  std::thread sender([&] {
    std::mt19937 rng(99);
    for (int i = 0; i < kFrames && send_ok; ++i) {
      const uint16_t len = uint16_t(1 + rng() % 2000);
      std::vector<uint8_t> frame(2 + len);
      frame[0] = uint8_t(len);
      frame[1] = uint8_t(len >> 8);
      const auto body = Pattern(len, uint32_t(i));
      std::memcpy(frame.data() + 2, body.data(), len);
      for (;;) {  // AC6 sends each frame with one send() and retries on WOULDBLOCK
        int err = -1;
        const int n = tx.Send(t.client, frame.data(), frame.size(), 0, &err);
        if (n == int(frame.size())) break;
        if (n != -1 || err != wsa::kWouldBlock) {
          send_ok = false;
          break;
        }
        SelectOne(tx, t.client, 'w', 10'000);
      }
    }
  });

  int frames = 0;
  int violations = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (frames < kFrames && ElapsedMs(t0) < 30000) {
    SelectSets sets;
    sets.read.push_back(t.accepted);
    int err = -1;
    if (rx.Select(&sets, 0, &err) != 1) {  // AC6 polls with a {0,0} timeout
      std::this_thread::sleep_for(Ms(1));
      continue;
    }
    // The game's state machine: recv(1) -> recv(1) -> recv(len) with no select in between.
    uint8_t lo = 0, hi = 0;
    if (rx.Recv(t.accepted, &lo, 1, 0, &err) != 1) ++violations;
    if (rx.Recv(t.accepted, &hi, 1, 0, &err) != 1) ++violations;
    const uint16_t len = uint16_t(lo | (hi << 8));
    std::vector<uint8_t> body(len);
    if (rx.Recv(t.accepted, body.data(), len, 0, &err) != int(len)) ++violations;
    if (body != Pattern(len, uint32_t(frames))) ++violations;
    ++frames;
    if (violations) break;
  }
  sender.join();
  CHECK(send_ok);
  CHECK(violations == 0);
  CHECK(frames == kFrames);
}

TEST_CASE("vnet: tcp fin and half-close") {
  NetParams p;
  p.jitter_ms = 15;  // lets the FIN overtake data on the wire
  FakeNet net(p);
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  TcpPair t = Connect(va, vb, kVipB, 21000);
  int err = -1;

  const auto m1 = Pattern(3000, 1), m2 = Pattern(10, 2), m3 = Pattern(20000, 3);
  CHECK(va.Send(t.client, m1.data(), m1.size(), 0, &err) == int(m1.size()));
  CHECK(va.Send(t.client, m2.data(), m2.size(), 0, &err) == int(m2.size()));
  CHECK(va.Send(t.client, m3.data(), m3.size(), 0, &err) == int(m3.size()));
  CHECK(va.Shutdown(t.client, kSdSend, &err) == 0);
  CHECK(va.Send(t.client, "x", 1, 0, &err) == -1);
  CHECK(err == wsa::kShutdown);

  // B must read every byte before it sees EOF, even though the FIN may arrive first.
  const size_t total = m1.size() + m2.size() + m3.size();
  std::vector<uint8_t> got;
  bool early_eof = false;
  const auto t0 = std::chrono::steady_clock::now();
  while (got.size() < total && ElapsedMs(t0) < 5000) {
    uint8_t buf[4096];
    const int n = vb.Recv(t.accepted, buf, sizeof(buf), 0, &err);
    if (n == 0) early_eof = true;
    if (n > 0) got.insert(got.end(), buf, buf + n);
    if (n <= 0) std::this_thread::sleep_for(Ms(1));
    if (early_eof) break;
  }
  CHECK(!early_eof);
  REQUIRE(got.size() == total);
  std::vector<uint8_t> expect = m1;
  expect.insert(expect.end(), m2.begin(), m2.end());
  expect.insert(expect.end(), m3.begin(), m3.end());
  CHECK(got == expect);
  uint8_t c = 0;
  REQUIRE(WaitFor([&] { return vb.Recv(t.accepted, &c, 1, 0, &err) == 0; }, 2000));
  CHECK(SelectOne(vb, t.accepted, 'r', 0) == 1);  // EOF is readable

  // The half-closed side still receives.
  CHECK(vb.Send(t.accepted, "reply", 5, 0, &err) == 5);
  uint8_t reply[5];
  CHECK(RecvExactly(va, t.client, reply, 5, 2000));
  CHECK(std::memcmp(reply, "reply", 5) == 0);

  // Graceful close from B: A sees EOF; both sides eventually free the connection.
  CHECK(vb.Close(t.accepted, &err) == 0);
  REQUIRE(WaitFor([&] { return va.Recv(t.client, &c, 1, 0, &err) == 0; }, 2000));
  CHECK(va.Close(t.client, &err) == 0);
  CHECK(
      WaitFor([&] { return va.stats().streams_open == 0 && vb.stats().streams_open == 0; }, 5000));
}

TEST_CASE("vnet: late duplicate syn after a graceful close is ignored") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  std::mutex tap_mu;
  std::vector<uint8_t> syn;
  net.SetTap([&](uint32_t, uint32_t, Channel ch, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lk(tap_mu);
    if (ch == Channel::kStreamCtl && n >= wire::kCtlHeader && d[0] == uint8_t(wire::Ctl::kSyn)) {
      syn.assign(d, d + n);
    }
  });
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  TcpPair t = Connect(va, vb, kVipB, 21000);
  int err = -1;
  CHECK(va.Close(t.client, &err) == 0);
  CHECK(vb.Close(t.accepted, &err) == 0);
  REQUIRE(WaitFor([&] { return va.stats().streams_open == 0 && vb.stats().streams_open == 0; }, 5000));
  std::vector<uint8_t> old_syn;
  {
    std::lock_guard<std::mutex> lk(tap_mu);
    old_syn = syn;
  }
  REQUIRE(!old_syn.empty());
  // The listener is still open; a stale SYN for the finished connection must not create a child.
  vb.OnLinkData(kVipA, uint8_t(Channel::kStreamCtl), old_syn.data(), old_syn.size());
  CHECK(SelectOne(vb, t.listener, 'r', 50'000) == 0);
  CHECK(vb.stats().streams_open == 0);
  // A fresh connection still works.
  const SocketId c2 = TcpConnect(va, kVipB, 21000);
  CHECK(SelectOne(va, c2, 'w', 2'000'000) == 1);
  CHECK(SelectOne(vb, t.listener, 'r', 1'000'000) == 1);
}

TEST_CASE("vnet: tcp close with unread data sends rst (peer sees 10054)") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  TcpPair t = Connect(va, vb, kVipB, 21000);
  int err = -1;
  const auto msg = Pattern(100, 5);
  CHECK(va.Send(t.client, msg.data(), msg.size(), 0, &err) == 100);
  uint32_t avail = 0;
  REQUIRE(
      WaitFor([&] { return vb.Available(t.accepted, &avail, &err) == 0 && avail == 100; }, 2000));
  CHECK(vb.Close(t.accepted, &err) == 0);
  uint8_t buf[16];
  REQUIRE(WaitFor(
      [&] { return va.Recv(t.client, buf, sizeof(buf), 0, &err) == -1 && err == wsa::kConnReset; },
      2000));
  SelectSets sets;
  sets.read.push_back(t.client);
  sets.write.push_back(t.client);
  sets.except.push_back(t.client);
  CHECK(va.Select(&sets, 0, &err) == 2);  // readable and writable; not a connect failure
  CHECK(va.Send(t.client, "x", 1, 0, &err) == -1);
  CHECK(err == wsa::kConnReset);
  CHECK(GetInt(va, t.client, kSolSocket, kSoError, &err) == wsa::kConnReset);
  CHECK(va.Close(t.client, &err) == 0);
  CHECK(va.stats().streams_open == 0);
}

TEST_CASE("vnet: tcp lost rst is repaired when the peer sends again") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  TcpPair t = Connect(va, vb, kVipB, 21000);
  int err = -1;
  CHECK(va.Send(t.client, "unread", 6, 0, &err) == 6);
  uint32_t avail = 0;
  REQUIRE(WaitFor([&] { return vb.Available(t.accepted, &avail, &err) == 0 && avail == 6; }, 2000));
  net.SetBlackhole(kVipB, true);  // B's RST is lost
  CHECK(vb.Close(t.accepted, &err) == 0);
  std::this_thread::sleep_for(Ms(20));
  net.SetBlackhole(kVipB, false);
  uint8_t buf[8];
  CHECK(va.Recv(t.client, buf, sizeof(buf), 0, &err) == -1);
  CHECK(err == wsa::kWouldBlock);  // A does not know yet
  CHECK(va.Send(t.client, "more", 4, 0, &err) == 4);
  REQUIRE(WaitFor(
      [&] { return va.Recv(t.client, buf, sizeof(buf), 0, &err) == -1 && err == wsa::kConnReset; },
      2000));
}

TEST_CASE("vnet: tcp peer lost resets streams and fails pending connects") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  auto& c = net.AddNode(kVipC);
  net.Start();
  VNet& va = *a.vnet;
  TcpPair t = Connect(va, *b.vnet, kVipB, 21000);
  net.SetBlackhole(kVipC, true);
  TcpListen(*c.vnet, 21000);
  const SocketId pending = TcpConnect(va, kVipC, 21000);
  int err = -1;

  va.OnPeerLost(kVipB);
  uint8_t buf[8];
  CHECK(va.Recv(t.client, buf, sizeof(buf), 0, &err) == -1);
  CHECK(err == wsa::kConnReset);
  CHECK(SelectOne(va, pending, 'e', 0) == 0);  // C is unaffected so far

  va.OnPeerLost(kVipC);
  CHECK(SelectOne(va, pending, 'e', 0) == 1);
  CHECK(SelectOne(va, pending, 'w', 0) == 0);
  CHECK(GetInt(va, pending, kSolSocket, kSoError, &err) == wsa::kTimedOut);
  CHECK(GetInt(va, pending, kSolSocket, kSoError, &err) == 0);  // read clears it
}

TEST_CASE("vnet: tcp connect refused") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  const SocketId s = TcpConnect(va, kVipB, 21000);  // nothing listens there
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(SelectOne(va, s, 'e', 2'000'000) == 1);
  CHECK(ElapsedMs(t0) < 500);
  int err = -1;
  CHECK(GetInt(va, s, kSolSocket, kSoError, &err) == wsa::kConnRefused);
  CHECK(va.Send(s, "x", 1, 0, &err) == -1);
  CHECK(err == wsa::kNotConn);

  // Blocking connect reports the error directly.
  const SocketId sb = MakeSocket(va, kSockStream, 0, /*non_blocking=*/false);
  CHECK(va.Connect(sb, {kVipB, 21000}, &err) == -1);
  CHECK(err == wsa::kConnRefused);
}

TEST_CASE("vnet: tcp connect timeout after 5 s") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  net.AddNode(kVipB);
  net.Start();
  net.SetBlackhole(kVipB, true);
  VNet& va = *a.vnet;
  const auto t0 = std::chrono::steady_clock::now();
  const SocketId s = TcpConnect(va, kVipB, 21000);
  CHECK(SelectOne(va, s, 'e', 8'000'000) == 1);
  const int64_t ms = ElapsedMs(t0);
  CHECK(ms >= 4900);
  CHECK(ms < 6000);
  int err = -1;
  CHECK(GetInt(va, s, kSolSocket, kSoError, &err) == wsa::kTimedOut);
  CHECK(va.stats().streams_open == 0);
}

TEST_CASE("vnet: select read/write/except and timeouts") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  int err = -1;
  SelectSets empty;
  CHECK(va.Select(&empty, 0, &err) == -1);
  CHECK(err == wsa::kInval);
  SelectSets bogus;
  bogus.read.push_back(777);
  CHECK(va.Select(&bogus, 0, &err) == -1);
  CHECK(err == wsa::kNotSock);

  const SocketId u = Udp(va, 20000);
  // Any UDP socket is writable; nothing to read yet.
  SelectSets sets;
  sets.read.push_back(u);
  sets.write.push_back(u);
  sets.except.push_back(u);
  CHECK(va.Select(&sets, 0, &err) == 1);
  CHECK(sets.read.empty());
  CHECK(sets.write.size() == 1);
  CHECK(sets.except.empty());

  // Timeout without data.
  auto t0 = std::chrono::steady_clock::now();
  CHECK(SelectOne(va, u, 'r', 50'000) == 0);
  CHECK(ElapsedMs(t0) >= 45);

  // An infinite wait is woken by data from another node.
  const SocketId ub = Udp(*b.vnet);
  std::thread poke([&] {
    std::this_thread::sleep_for(Ms(100));
    int e = -1;
    b.vnet->SendTo(ub, "wake", 4, 0, {kVipA, 20000}, &e);
  });
  t0 = std::chrono::steady_clock::now();
  CHECK(SelectOne(va, u, 'r', -1) == 1);
  CHECK(ElapsedMs(t0) >= 90);
  poke.join();

  // Mixed sets: the count is the total over all sets.
  const SocketId l = TcpListen(va, 21000);
  sets = {};
  sets.read = {u, l};
  sets.write = {u, l};
  CHECK(va.Select(&sets, 0, &err) == 2);  // u readable + u writable; the listener is neither
  CHECK(sets.read.size() == 1);
  CHECK(sets.write.size() == 1);
}

TEST_CASE("vnet: nonblocking connect becomes writable; accept wouldblock") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  int err = -1;
  const SocketId l = TcpListen(vb, 21000);
  Endpoint from;
  CHECK(vb.Accept(l, &from, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  CHECK(SelectOne(vb, l, 'r', 0) == 0);

  const SocketId c = TcpConnect(va, kVipB, 21000);
  CHECK(va.Connect(c, {kVipB, 21000}, &err) == -1);
  CHECK(err == wsa::kAlready);
  CHECK(va.Send(c, "x", 1, 0, &err) == -1);  // still connecting
  CHECK(err == wsa::kWouldBlock);
  Endpoint peer;
  CHECK(va.GetPeerName(c, &peer, &err) == -1);
  CHECK(err == wsa::kNotConn);
  CHECK(SelectOne(va, c, 'w', 2'000'000) == 1);
  CHECK(va.GetPeerName(c, &peer, &err) == 0);
  CHECK(peer.ip == kVipB);
  CHECK(peer.port == 21000);
  CHECK(va.Connect(c, {kVipB, 21000}, &err) == -1);
  CHECK(err == wsa::kIsConn);
  CHECK(SelectOne(vb, l, 'r', 1'000'000) == 1);
  const SocketId s = vb.Accept(l, &from, &err);
  CHECK(s > 0);
  CHECK(vb.Accept(l, &from, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  // Not a listener / not a stream.
  CHECK(vb.Accept(s, &from, &err) == -1);
  CHECK(err == wsa::kInval);
  const SocketId u = Udp(vb, 20000);
  CHECK(vb.Listen(u, 4, &err) == -1);
  CHECK(err == wsa::kOpNotSupp);
}

TEST_CASE("vnet: getsockname for 0.0.0.0-bound udp and an accepted tcp socket") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  int err = -1;
  Endpoint e;
  const SocketId u = Udp(vb, 20000);
  CHECK(vb.GetSockName(u, &e, &err) == 0);
  CHECK(e.ip == kInaddrAny);
  CHECK(e.port == 20000);

  TcpPair t = Connect(va, vb, kVipB, 21000);
  CHECK(vb.GetSockName(t.listener, &e, &err) == 0);
  CHECK(e.ip == kInaddrAny);
  CHECK(e.port == 21000);
  CHECK(vb.GetSockName(t.accepted, &e, &err) == 0);
  CHECK(e.ip == kVipB);
  CHECK(e.port == 21000);
  Endpoint client_local, peer;
  CHECK(va.GetSockName(t.client, &client_local, &err) == 0);
  CHECK(client_local.ip == kVipA);
  CHECK(client_local.port >= 49152);
  CHECK(vb.GetPeerName(t.accepted, &peer, &err) == 0);
  CHECK(peer.ip == kVipA);
  CHECK(peer.port == client_local.port);
}

TEST_CASE("vnet: tcp loopback to own vip and 127.0.0.1") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  net.Start();
  VNet& v = *a.vnet;
  const uint64_t sent_before = net.packets_sent();
  for (uint32_t ip : {kVipA, kLoopback}) {
    TcpPair t = Connect(v, v, ip, uint16_t(ip == kVipA ? 21000 : 21001));
    int err = -1;
    Endpoint e;
    CHECK(v.GetSockName(t.accepted, &e, &err) == 0);
    CHECK(e.ip == ip);
    CHECK(v.GetPeerName(t.accepted, &e, &err) == 0);
    CHECK(e.ip == ip);
    const auto msg = Pattern(40000, ip);  // > 32 KiB: the first send is capped
    CHECK(v.Send(t.client, msg.data(), msg.size(), 0, &err) == 32768);
    CHECK(SendAll(v, t.client, msg.data() + 32768, msg.size() - 32768, 2000));
    std::vector<uint8_t> got(msg.size());
    CHECK(RecvExactly(v, t.accepted, got.data(), got.size(), 2000));
    CHECK(got == msg);
    CHECK(v.Close(t.client, &err) == 0);
    CHECK(v.Close(t.accepted, &err) == 0);
    CHECK(v.Close(t.listener, &err) == 0);
  }
  CHECK(net.packets_sent() == sent_before);
  CHECK(WaitFor([&] { return v.stats().streams_open == 0; }, 5000));
}

TEST_CASE("vnet: tcp send window backpressure") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  TcpPair t = Connect(va, vb, kVipB, 21000);
  const auto chunk = Pattern(32768, 9);
  size_t accepted = 0;
  int err = -1;
  // B never reads: A must eventually see WOULDBLOCK and an unwritable socket.
  auto stalled_since = std::chrono::steady_clock::now();
  while (ElapsedMs(stalled_since) < 300 && accepted < (64u << 20)) {
    const int n = va.Send(t.client, chunk.data(), chunk.size(), 0, &err);
    if (n > 0) {
      accepted += size_t(n);
      stalled_since = std::chrono::steady_clock::now();
    } else {
      CHECK(err == wsa::kWouldBlock);
      std::this_thread::sleep_for(Ms(2));
    }
  }
  std::fprintf(stderr, "  accepted %zu bytes before the window closed\n", accepted);
  CHECK(accepted >= 256u * 1024);
  CHECK(accepted < 4u << 20);
  CHECK(SelectOne(va, t.client, 'w', 0) == 0);
  // Draining B reopens the window.
  std::vector<uint8_t> sink(accepted);
  CHECK(RecvExactly(vb, t.accepted, sink.data(), sink.size(), 10000));
  CHECK(SelectOne(va, t.client, 'w', 2'000'000) == 1);
}

// ---------------------------------------------------------------------------------- blocking

TEST_CASE("vnet: blocking mode, SO_RCVTIMEO and wake-ups") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& va = *a.vnet;
  VNet& vb = *b.vnet;
  int err = -1;
  const SocketId u = MakeSocket(va, kSockDgram, 0, /*non_blocking=*/false);
  CHECK(va.Bind(u, {kInaddrAny, 20000}, &err) == 0);
  CHECK(SetInt(va, u, kSolSocket, kSoRcvTimeo, 100) == 0);
  uint8_t buf[16];
  Endpoint from;
  auto t0 = std::chrono::steady_clock::now();
  CHECK(va.RecvFrom(u, buf, sizeof(buf), 0, &from, &err) == -1);
  CHECK(err == wsa::kTimedOut);
  CHECK(ElapsedMs(t0) >= 90);

  // A blocking receive without timeout is woken by data.
  CHECK(SetInt(va, u, kSolSocket, kSoRcvTimeo, 0) == 0);
  const SocketId ub = Udp(vb);
  std::thread poke([&] {
    std::this_thread::sleep_for(Ms(50));
    int e = -1;
    vb.SendTo(ub, "data", 4, 0, {kVipA, 20000}, &e);
  });
  CHECK(va.RecvFrom(u, buf, sizeof(buf), 0, &from, &err) == 4);
  CHECK(from.ip == kVipB);
  poke.join();

  // Closing the socket from another thread wakes a blocked receive.
  std::thread closer([&] {
    std::this_thread::sleep_for(Ms(50));
    int e = -1;
    va.Close(u, &e);
  });
  CHECK(va.RecvFrom(u, buf, sizeof(buf), 0, &from, &err) == -1);
  CHECK(err == wsa::kInterrupted);
  closer.join();

  // Blocking connect, accept, send and recv.
  const SocketId l = MakeSocket(vb, kSockStream, 0, false);
  CHECK(vb.Bind(l, {kInaddrAny, 21000}, &err) == 0);
  CHECK(vb.Listen(l, 4, &err) == 0);
  SocketId accepted = -1;
  std::thread server([&] {
    int e = -1;
    Endpoint f;
    accepted = vb.Accept(l, &f, &e);
    uint8_t m[5];
    if (accepted > 0 && vb.Recv(accepted, m, 5, 0, &e) == 5) vb.Send(accepted, m, 5, 0, &e);
  });
  const SocketId c = MakeSocket(va, kSockStream, 0, false);
  CHECK(va.Connect(c, {kVipB, 21000}, &err) == 0);
  CHECK(va.Send(c, "hello", 5, 0, &err) == 5);
  uint8_t echo[5] = {};
  CHECK(va.Recv(c, echo, 5, 0, &err) == 5);
  CHECK(std::memcmp(echo, "hello", 5) == 0);
  server.join();
  CHECK(accepted > 0);
}

// ---------------------------------------------------------------------------------- misc

TEST_CASE("vnet: bind conflicts, socket creation and options") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  net.Start();
  VNet& v = *a.vnet;
  int err = -1;
  CHECK(v.Socket(3, 0, &err) == -1);
  CHECK(err == 10044);
  CHECK(v.Socket(kSockStream, kProtoUdp, &err) == -1);
  CHECK(err == wsa::kProtoType);
  CHECK(v.Socket(kSockDgram, 99, &err) == -1);
  CHECK(err == wsa::kProtoNoSupport);

  const SocketId u1 = Udp(v);
  const SocketId u2 = Udp(v);
  CHECK(SetInt(v, u1, kSolSocket, kSoReuseAddr, 1) == 0);
  CHECK(SetInt(v, u2, kSolSocket, kSoReuseAddr, 1) == 0);
  CHECK(v.Bind(u1, {kInaddrAny, 20000}, &err) == 0);
  CHECK(v.Bind(u2, {kInaddrAny, 20000}, &err) == -1);
  CHECK(err == wsa::kAddrInUse);  // SO_REUSEADDR never shares a live port
  CHECK(v.Bind(u2, {kVipA, 20000}, &err) == -1);
  CHECK(err == wsa::kAddrInUse);
  CHECK(v.Bind(u1, {kInaddrAny, 20001}, &err) == -1);
  CHECK(err == wsa::kInval);  // already bound
  CHECK(v.Bind(u2, {kVipB, 20002}, &err) == -1);
  CHECK(err == wsa::kAddrNotAvail);  // not our address
  const SocketId t1 = MakeSocket(v, kSockStream, kProtoTcp);
  CHECK(v.Bind(t1, {kInaddrAny, 20000}, &err) == 0);  // separate stream port space
  CHECK(v.Close(u1, &err) == 0);
  CHECK(v.Bind(u2, {kInaddrAny, 20000}, &err) == 0);  // free again after close

  CHECK(GetInt(v, u2, kSolSocket, kSoType, &err) == kSockDgram);
  CHECK(GetInt(v, t1, kSolSocket, kSoType, &err) == kSockStream);
  CHECK(GetInt(v, t1, kSolSocket, kSoError, &err) == 0);
  CHECK(SetInt(v, t1, kSolSocket, kSoSndBuf, 200000) == 0);
  CHECK(SetInt(v, t1, kSolSocket, kSoRcvBuf, 2000000) == 0);
  CHECK(GetInt(v, t1, kSolSocket, kSoSndBuf, &err) == 200000);
  CHECK(GetInt(v, t1, kSolSocket, kSoRcvBuf, &err) == 2000000);
  CHECK(GetInt(v, t1, kSolSocket, kSoReuseAddr, &err) == 0);
  CHECK(SetInt(v, t1, kSolSocket, kSoKeepAlive, 1) == 0);
  CHECK(SetInt(v, t1, kSolSocket, kSoLinger, 0) == 0);
  CHECK(SetInt(v, u2, kSolSocket, kSoBroadcast, 1) == 0);
  CHECK(GetInt(v, u2, kSolSocket, kSoBroadcast, &err) == 1);
  CHECK(SetInt(v, t1, kIpprotoTcp, kTcpNoDelay, 1) == 0);
  CHECK(GetInt(v, t1, kIpprotoTcp, kTcpNoDelay, &err) == 1);
  CHECK(SetInt(v, u2, kIpprotoTcp, kTcpNoDelay, 1) == wsa::kNoProtoOpt);
  CHECK(SetInt(v, t1, kSolSocket, 0x1234, 1) == wsa::kNoProtoOpt);
  CHECK(SetInt(v, t1, kSolSocket, kSoType, 2) == wsa::kNoProtoOpt);
  CHECK(GetInt(v, t1, kSolSocket, 0x1234, &err) == -1);
  CHECK(err == wsa::kNoProtoOpt);
  CHECK(SetInt(v, t1, 0x77, 1, 1) == wsa::kInval);
  // The glue passes host-endian integers at the guest's optlen (1, 2, 4 or 8 bytes).
  const uint8_t one = 1;
  CHECK(v.SetOption(t1, kSolSocket, kSoReuseAddr, &one, 1, &err) == 0);
  CHECK(GetInt(v, t1, kSolSocket, kSoReuseAddr, &err) == 1);
  const uint16_t buf16 = 4096;
  CHECK(v.SetOption(t1, kSolSocket, kSoSndBuf, &buf16, 2, &err) == 0);
  CHECK(GetInt(v, t1, kSolSocket, kSoSndBuf, &err) == 4096);
  const int64_t buf64 = 8192;
  CHECK(v.SetOption(t1, kSolSocket, kSoRcvBuf, &buf64, 8, &err) == 0);
  CHECK(GetInt(v, t1, kSolSocket, kSoRcvBuf, &err) == 8192);
  const uint8_t three[3] = {1, 0, 0};
  CHECK(v.SetOption(t1, kSolSocket, kSoSndBuf, three, 3, &err) == -1);
  CHECK(err == wsa::kFault);
  CHECK(v.SetOption(t1, kSolSocket, kSoSndBuf, nullptr, 4, &err) == -1);
  CHECK(err == wsa::kFault);
  const int32_t negative = -5;
  CHECK(v.SetOption(t1, kSolSocket, kSoSndBuf, &negative, 4, &err) == -1);
  CHECK(err == wsa::kInval);
  const uint16_t linger[2] = {1, 7};
  CHECK(v.SetOption(t1, kSolSocket, kSoLinger, linger, sizeof(linger), &err) == 0);
  uint16_t linger_out[2] = {};
  size_t linger_len = sizeof(linger_out);
  CHECK(v.GetOption(t1, kSolSocket, kSoLinger, linger_out, &linger_len, &err) == 0);
  CHECK(linger_len == 4);
  CHECK(linger_out[0] == 1);
  CHECK(linger_out[1] == 7);
  int32_t tiny = 0;
  size_t tiny_len = 2;
  CHECK(v.GetOption(t1, kSolSocket, kSoType, &tiny, &tiny_len, &err) == -1);
  CHECK(err == wsa::kFault);

  // Unknown or closed handles.
  CHECK(!v.IsSocket(999));
  CHECK(v.Close(999, &err) == -1);
  CHECK(err == wsa::kNotSock);
  CHECK(v.Close(u2, &err) == 0);
  CHECK(!v.IsSocket(u2));
  uint8_t buf[4];
  CHECK(v.Recv(u2, buf, 4, 0, &err) == -1);
  CHECK(err == wsa::kNotSock);
}

TEST_CASE("vnet: malformed link packets are dropped safely") {
  FakeNet net;
  auto& a = net.AddNode(kVipA);
  auto& b = net.AddNode(kVipB);
  net.Start();
  VNet& vb = *b.vnet;
  TcpPair live = Connect(*a.vnet, vb, kVipB, 21000);
  const SocketId u = Udp(vb, 999);
  std::mt19937 rng(2026);
  std::vector<uint8_t> pkt(1400);
  for (int i = 0; i < 20000; ++i) {
    const size_t len = rng() % (pkt.size() + 1);
    for (size_t j = 0; j < len; ++j) pkt[j] = uint8_t(rng());
    const uint8_t channel = uint8_t(1 + rng() % 4);
    if (channel == 2 && len >= 4 && (i & 1)) wire::PutLe32(pkt.data(), 0);  // unknown conv
    if (channel == 1 && len >= wire::kDgramHeader && (i & 1)) {
      pkt[0] = 17;
      pkt[8] = uint8_t(1 + rng() % 8);
      pkt[7] = uint8_t(rng() % 10);
    }
    vb.OnLinkData(kVipA + uint32_t(rng() % 3), channel, len ? pkt.data() : nullptr, len);
  }
  vb.OnLinkData(kVipA, 0x7F, pkt.data(), 10);
  vb.OnLinkData(kVipA, 1, nullptr, 0);
  // Drain whatever random datagrams happened to land on port 999.
  uint8_t buf[2048];
  Endpoint from;
  int err = -1;
  while (vb.RecvFrom(u, buf, sizeof(buf), 0, &from, &err) >= 0 || err == wsa::kMsgSize) {
  }
  // The vnet still works afterwards.
  const SocketId ua = Udp(*a.vnet);
  CHECK(a.vnet->SendTo(ua, "ok", 2, 0, {kVipB, 999}, &err) == 2);
  CHECK(WaitFor([&] { return vb.RecvFrom(u, buf, sizeof(buf), 0, &from, &err) == 2; }, 1000));
  TcpPair fresh = Connect(*a.vnet, vb, kVipB, 21001);
  CHECK(a.vnet->Send(fresh.client, "still", 5, 0, &err) == 5);
  uint8_t got[5];
  CHECK(RecvExactly(vb, fresh.accepted, got, 5, 2000));
  (void)live;
}

// AC6's host: TCP listener on 21000 (backlog 16) plus, per accepted client, a UDP socket on
// 20000+i found with a bind retry loop; a 4-byte handshake {tcp port, udp port} over TCP; then
// UDP both ways with the host learning each client's endpoint from its first datagram. The
// host's own local client connects over loopback. 15 remote clients + 1 local = 16 slots.
TEST_CASE("vnet: ac6 host model, listener 21000 and per-client udp 20000+i, 15 clients") {
  NetParams p;
  p.loss = 0.02;
  p.jitter_ms = 3;
  FakeNet net(p);
  constexpr uint32_t kHost = 0x0A400010;
  constexpr int kRemote = 15;
  auto& host = net.AddNode(kHost);
  std::vector<vnet_fake::Node*> clients;
  for (int i = 0; i < kRemote; ++i) clients.push_back(&net.AddNode(kHost + 1 + uint32_t(i)));
  net.Start();
  VNet& hv = *host.vnet;

  auto set_opts = [](VNet& v, SocketId s, int snd, int rcv) {
    CHECK(SetInt(v, s, kSolSocket, kSoSndBuf, snd) == 0);
    CHECK(SetInt(v, s, kSolSocket, kSoRcvBuf, rcv) == 0);
    CHECK(SetInt(v, s, kSolSocket, kSoReuseAddr, 1) == 0);
  };
  const SocketId listener = MakeSocket(hv, kSockStream, 0);
  set_opts(hv, listener, 200000, 2000000);
  int err = -1;
  REQUIRE(hv.Bind(listener, {kInaddrAny, 21000}, &err) == 0);
  REQUIRE(hv.Listen(listener, 16, &err) == 0);

  std::atomic<int> clients_done{0};
  std::atomic<int> client_failures{0};
  auto run_client = [&](VNet& v, uint32_t host_ip, uint32_t my_ip, int k) {
    auto fail = [&] { client_failures++; };
    const SocketId tcp = MakeSocket(v, kSockStream, 0);
    const SocketId udp = MakeSocket(v, kSockDgram, 0);
    int e = -1;
    if (v.Connect(tcp, {host_ip, 21000}, &e) != -1 || e != wsa::kWouldBlock) return fail();
    if (SelectOne(v, tcp, 'w', 10'000'000) != 1) return fail();
    uint8_t lo, hi, hs[4];
    if (!RecvExactly(v, tcp, &lo, 1, 10000) || !RecvExactly(v, tcp, &hi, 1, 10000)) return fail();
    if ((lo | (hi << 8)) != 4 || !RecvExactly(v, tcp, hs, 4, 10000)) return fail();
    const uint16_t tcp_port = uint16_t(hs[0] | (hs[1] << 8));
    const uint16_t udp_port = uint16_t(hs[2] | (hs[3] << 8));
    if (tcp_port != 21000 || udp_port < 20000 || udp_port >= 20016) return fail();
    // UDP hello until the host acknowledges it (UDP is lossy).
    const uint32_t hello = 0xC0DE0000u | uint32_t(k);
    bool acked = false;
    for (int attempt = 0; attempt < 100 && !acked; ++attempt) {
      v.SendTo(udp, &hello, 4, 0, {host_ip, udp_port}, &e);
      if (SelectOne(v, udp, 'r', 50'000) == 1) {
        uint32_t ack = 0;
        Endpoint from;
        if (v.RecvFrom(udp, &ack, 4, 0, &from, &e) == 4 && ack == ~hello && from.ip == host_ip &&
            from.port == udp_port) {
          acked = true;
        }
      }
    }
    if (!acked) return fail();
    // Reliable frames over TCP, echoed by the host.
    for (int f = 0; f < 20; ++f) {
      const auto body = Pattern(100 + size_t(f) * 50, uint32_t(k * 100 + f));
      std::vector<uint8_t> frame = {uint8_t(body.size()), uint8_t(body.size() >> 8)};
      frame.insert(frame.end(), body.begin(), body.end());
      if (!SendAll(v, tcp, frame.data(), frame.size(), 10000)) return fail();
      std::vector<uint8_t> echo(frame.size());
      if (!RecvExactly(v, tcp, echo.data(), echo.size(), 10000) || echo != frame) return fail();
    }
    (void)my_ip;
    clients_done++;
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kRemote; ++i) {
    threads.emplace_back(
        [&, i] { run_client(*clients[i]->vnet, kHost, kHost + 1 + uint32_t(i), i); });
  }
  threads.emplace_back([&] { run_client(hv, kHost, kHost, 99); });  // the host's local client

  struct Slot {
    SocketId tcp = 0, udp = 0;
    uint16_t udp_port = 0;
  };
  std::vector<Slot> slots;
  const auto t0 = std::chrono::steady_clock::now();
  while (clients_done + client_failures < kRemote + 1 && ElapsedMs(t0) < 60000) {
    SelectSets sets;
    sets.read.push_back(listener);
    for (auto& s : slots) {
      sets.read.push_back(s.tcp);
      sets.read.push_back(s.udp);
    }
    if (hv.Select(&sets, 0, &err) <= 0) {
      std::this_thread::sleep_for(Ms(1));
      continue;
    }
    for (SocketId r : sets.read) {
      if (r == listener) {
        Endpoint from;
        const SocketId t = hv.Accept(listener, &from, &err);
        if (t <= 0) continue;
        set_opts(hv, t, 200000, 2000000);
        hv.SetNonBlocking(t, true, &err);
        const SocketId u = MakeSocket(hv, kSockDgram, 0);
        Endpoint lname;
        CHECK(hv.GetSockName(listener, &lname, &err) == 0);
        uint16_t port = 0;
        for (int i = 0; i < 16 && !port; ++i) {
          if (hv.Bind(u, {lname.ip, uint16_t(20000 + i)}, &err) == 0)
            port = uint16_t(20000 + i);
          else
            CHECK(err == wsa::kAddrInUse);
        }
        if (!port) {
          CHECK(port != 0);
          continue;
        }
        set_opts(hv, u, 2000, 2000);
        Endpoint tname, uname;
        CHECK(hv.GetSockName(t, &tname, &err) == 0);
        CHECK(hv.GetSockName(u, &uname, &err) == 0);
        CHECK(tname.ip == kHost);
        CHECK(tname.port == 21000);
        CHECK(uname.ip == kInaddrAny);
        CHECK(uname.port == port);
        const uint8_t frame[6] = {4,
                                  0,
                                  uint8_t(tname.port),
                                  uint8_t(tname.port >> 8),
                                  uint8_t(uname.port),
                                  uint8_t(uname.port >> 8)};
        CHECK(hv.Send(t, frame, sizeof(frame), 0, &err) == 6);
        slots.push_back({t, u, port});
        continue;
      }
      for (auto& s : slots) {
        if (r == s.udp) {
          uint32_t hello = 0;
          Endpoint from;
          if (hv.RecvFrom(s.udp, &hello, 4, 0, &from, &err) == 4) {
            const uint32_t ack = ~hello;
            hv.SendTo(s.udp, &ack, 4, 0, from, &err);  // reply to the learned endpoint
          }
        } else if (r == s.tcp) {
          // Echo whole frames back (the engine's recv(1)/recv(1)/recv(n) pattern).
          uint8_t lo, hi;
          if (hv.Recv(s.tcp, &lo, 1, 0, &err) != 1) continue;
          CHECK(hv.Recv(s.tcp, &hi, 1, 0, &err) == 1);
          const uint16_t len = uint16_t(lo | (hi << 8));
          std::vector<uint8_t> frame(2 + len);
          frame[0] = lo;
          frame[1] = hi;
          CHECK(hv.Recv(s.tcp, frame.data() + 2, len, 0, &err) == int(len));
          CHECK(SendAll(hv, s.tcp, frame.data(), frame.size(), 5000));
        }
      }
    }
  }
  for (auto& t : threads) t.join();
  std::fprintf(stderr, "  host model: %d clients done, %d failed, %lld ms\n", clients_done.load(),
               client_failures.load(), (long long)ElapsedMs(t0));
  CHECK(client_failures == 0);
  CHECK(clients_done == kRemote + 1);
  CHECK(slots.size() == size_t(kRemote + 1));
  // All 16 UDP slots are taken now: a 17th client would find no port.
  const SocketId extra = MakeSocket(hv, kSockDgram, 0);
  for (int i = 0; i < 16; ++i) {
    CHECK(hv.Bind(extra, {kInaddrAny, uint16_t(20000 + i)}, &err) == -1);
    CHECK(err == wsa::kAddrInUse);
  }
}

// ---------------------------------------------------------------------------------- hostile KCP

namespace {

// A PeerLink driven by hand: records every payload and has a settable clock.
class ManualLink final : public PeerLink {
 public:
  struct Sent {
    uint32_t vip;
    Channel channel;
    std::vector<uint8_t> data;
  };
  explicit ManualLink(uint32_t vip) : vip_(vip) {}
  bool Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) override {
    std::lock_guard<std::mutex> lk(mu);
    sent.push_back({vip, channel, std::vector<uint8_t>(data, data + len)});
    return true;
  }
  uint32_t LocalVip() const override { return vip_; }
  size_t MaxPayload() const override { return 1200; }
  uint64_t NowMs() const override { return now.load(); }

  std::mutex mu;
  std::vector<Sent> sent;
  std::atomic<uint64_t> now{100000};

 private:
  uint32_t vip_;
};

std::vector<uint8_t> KcpSegment(uint32_t conv, uint8_t cmd, uint8_t frg, uint16_t wnd, uint32_t ts,
                                uint32_t sn, uint32_t una, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> s(wire::kKcpOverhead + payload.size());
  wire::PutLe32(s.data(), conv);
  s[4] = cmd;
  s[5] = frg;
  wire::PutLe16(s.data() + 6, wnd);
  wire::PutLe32(s.data() + 8, ts);
  wire::PutLe32(s.data() + 12, sn);
  wire::PutLe32(s.data() + 16, una);
  wire::PutLe32(s.data() + 20, uint32_t(payload.size()));
  std::copy(payload.begin(), payload.end(), s.begin() + wire::kKcpOverhead);
  return s;
}

}  // namespace

TEST_CASE("vnet: hostile kcp input on a live connection is contained") {
  ManualLink link(kVipA);
  VNet v(&link);
  const SocketId c = TcpConnect(v, kVipB, 21000);
  wire::CtlMsg syn;
  {
    std::lock_guard<std::mutex> lk(link.mu);
    REQUIRE(!link.sent.empty());
    const auto& p = link.sent.back();
    REQUIRE(p.channel == Channel::kStreamCtl && p.vip == kVipB);
    REQUIRE(wire::DecodeCtl(p.data.data(), p.data.size(), &syn));
    REQUIRE(syn.type == wire::Ctl::kSyn);
    CHECK(syn.dst_port == 21000);
    CHECK((syn.conn_id & 0x80000000u) != 0);  // initiator A has the lower vip
  }
  wire::CtlMsg synack;
  synack.type = wire::Ctl::kSynAck;
  synack.conn_id = syn.conn_id;
  synack.src_port = 21000;
  synack.dst_port = syn.src_port;
  uint8_t buf[wire::kCtlMaxSize];
  v.OnLinkData(kVipB, uint8_t(Channel::kStreamCtl), buf, wire::EncodeCtl(synack, buf));
  REQUIRE(SelectOne(v, c, 'w', 0) == 1);
  v.Tick(link.now += 10);
  const uint32_t now32 = uint32_t(link.now.load());

  // A well-formed PUSH from B is delivered.
  const std::vector<uint8_t> world = {'w', 'o', 'r', 'l', 'd'};
  auto push = KcpSegment(syn.conn_id, wire::kKcpCmdPush, 0, 256, now32, 0, 0, world);
  v.OnLinkData(kVipB, uint8_t(Channel::kStream), push.data(), push.size());
  uint8_t got[8];
  int err = -1;
  CHECK(v.Recv(c, got, sizeof(got), 0, &err) == 5);
  CHECK(std::memcmp(got, "world", 5) == 0);

  // An ACK echoing a timestamp ~24 days old would overflow KCP's RTT arithmetic; it is dropped
  // before ikcp_input (the UBSan build turns that overflow into a test failure).
  auto ack = KcpSegment(syn.conn_id, wire::kKcpCmdAck, 0, 256, now32 - 0x7FFFFFF0u, 0, 0, {});
  v.OnLinkData(kVipB, uint8_t(Channel::kStream), ack.data(), ack.size());
  CHECK(!wire::KcpPacketSane(ack.data(), ack.size(), syn.conn_id, now32));
  auto big_frg = KcpSegment(syn.conn_id, wire::kKcpCmdPush, 200, 256, now32, 1, 0, world);
  CHECK(!wire::KcpPacketSane(big_frg.data(), big_frg.size(), syn.conn_id, now32));
  auto trailing = push;
  trailing.push_back(0);
  CHECK(!wire::KcpPacketSane(trailing.data(), trailing.size(), syn.conn_id, now32));
  CHECK(wire::KcpPacketSane(push.data(), push.size(), syn.conn_id, now32));

  // Random segments with the live conv: every field hostile, some lengths inconsistent.
  std::mt19937 rng(77);
  for (int i = 0; i < 20000; ++i) {
    std::vector<uint8_t> pkt;
    const int segs = 1 + int(rng() % 3);
    for (int s = 0; s < segs; ++s) {
      std::vector<uint8_t> payload(rng() % 64);
      for (auto& b : payload) b = uint8_t(rng());
      const uint8_t cmd = uint8_t((rng() & 3) ? 81 + rng() % 4 : rng());
      auto seg = KcpSegment(syn.conn_id, cmd, uint8_t(rng()), uint16_t(rng()), uint32_t(rng()),
                            uint32_t(rng() % 512), uint32_t(rng() % 512), payload);
      if (rng() % 8 == 0) wire::PutLe32(seg.data() + 20, uint32_t(rng()));  // bogus length
      pkt.insert(pkt.end(), seg.begin(), seg.end());
    }
    v.OnLinkData(kVipB, uint8_t(Channel::kStream), pkt.data(), pkt.size());
    if (i % 100 == 0) v.Tick(link.now += 5);
    while (v.Recv(c, got, sizeof(got), 0, &err) > 0) {
    }
  }
  CHECK(v.IsSocket(c));
  CHECK(v.Close(c, &err) == 0);
  v.CloseAll();
  CHECK(v.stats().streams_open == 0);
  CHECK(v.stats().sockets_open == 0);
}
