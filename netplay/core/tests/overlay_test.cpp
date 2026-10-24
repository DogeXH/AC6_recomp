// ac6net overlay tests. The first group needs no server; "[server]" tests talk to a real
// ac6lobby (see overlay_test_util.h for how one is provided).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ac6net/netplay.h"
#include "crypto.h"
#include "mini_test.h"
#include "nlohmann/json.hpp"
#include "overlay.h"
#include "overlay_internal.h"
#include "overlay_net.h"
#include "overlay_test_util.h"
#include "overlay_wire.h"

using namespace ac6net;
using namespace overlay_test;
using json = nlohmann::json;

namespace {

uint64_t ElapsedMs(std::chrono::steady_clock::time_point t0) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
}

// A config pointing at a port in our range where nothing listens.
Config DeadServerConfig(uint16_t port) {
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  Config c;
  c.enabled = true;
  c.server = "127.0.0.1:47819";
  c.server_key_hex = HexEncode(pk.data(), pk.size());
  c.gamertag = "Nobody";
  c.port = port;
  return c;
}

void SendBurst(TestClient& from, uint32_t to_vip, uint32_t first_seq, int count, size_t len,
               Channel ch = Channel::kDatagram, int gap_ms = 2) {
  for (int i = 0; i < count; ++i) {
    const auto p = Payload(first_seq + static_cast<uint32_t>(i), len);
    from.overlay().Send(to_vip, ch, p.data(), p.size());
    if (gap_ms) SleepMs(gap_ms);
  }
}

}  // namespace

// --- No server needed ---

TEST_CASE("overlay: MaxPayload keeps a RELAY datagram within 1400 bytes") {
  Overlay ov;
  CHECK(ov.MaxPayload() == 1346);
  CHECK(ov.MaxPayload() >= 1200);
  // RELAY(18) + P2P header(18) + kind(1) + channel(1) + payload + tag(16).
  CHECK(18 + 18 + 1 + 1 + ov.MaxPayload() + 16 == 1400);
}

TEST_CASE("overlay: Start validates the configuration; calls fail cleanly when stopped") {
  Overlay ov;
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  Config c = DeadServerConfig(kClientPortBase);
  c.server_key_hex = "1234";
  CHECK(!ov.Start(c, sk));
  c.server_key_hex = std::string(64, '0');  // all-zero key: low order, DH output is zero
  CHECK(!ov.Start(c, sk));
  uint8_t d[4] = {1, 2, 3, 4};
  CHECK(!ov.Send(0x0A400002, Channel::kDatagram, d, sizeof(d)));
  const RpcResult r = ov.Call("whoami", "{}");
  CHECK(!r.transport_ok);
  CHECK(r.err == "offline");
  CHECK(ov.state() == NetState::kDisabled);
  CHECK(ov.LocalVip() == 0);
}

TEST_CASE("overlay: Call fails fast while not online; Send validates and queues") {
  Overlay ov;
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  REQUIRE(ov.Start(DeadServerConfig(kClientPortBase), sk));
  CHECK(ov.state() == NetState::kConnecting);
  const auto t0 = std::chrono::steady_clock::now();
  const RpcResult r = ov.Call("whoami", "{}", 3000);
  CHECK(ElapsedMs(t0) < 100);
  CHECK(!r.transport_ok);
  CHECK(r.err == "offline");
  CHECK(ov.Call("whoami", "not json").err == "bad_request");

  std::vector<uint8_t> big(ov.MaxPayload() + 1, 0xAB);
  CHECK(!ov.Send(0x0A400063, Channel::kDatagram, big.data(), big.size()));  // too large
  CHECK(!ov.Send(0xC0A80001, Channel::kDatagram, big.data(), 10));         // not a vIP
  // Unknown vIP: 32 payloads are queued for the introduction, then the queue is full.
  int accepted = 0;
  for (int i = 0; i < 40; ++i) accepted += ov.Send(0x0A400063, Channel::kDatagram, big.data(), 100);
  CHECK(accepted == 32);
  // 64 KiB per vIP: 1346-byte payloads fill it after 48.
  accepted = 0;
  for (int i = 0; i < 60; ++i) {
    accepted += ov.Send(0x0A400064, Channel::kDatagram, big.data(), ov.MaxPayload());
  }
  CHECK(accepted == 32);  // the count limit (32) binds before the byte limit (48 x 1346)
  const auto c = ov.counters();
  CHECK(c.data_queued == 64);
  CHECK(c.data_dropped >= 8 + 28);
  ov.Stop();
  CHECK(!ov.running());
}

TEST_CASE("overlay: malformed and hostile datagrams are dropped without harm") {
  Overlay ov;
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  REQUIRE(ov.Start(DeadServerConfig(kClientPortBase), sk));
  net::UdpSocket attacker;
  std::string err;
  REQUIRE(attacker.Open(AF_INET, kClientPortBase + 1, &err));
  net::Addr target;
  REQUIRE(net::Addr::FromIpPort("127.0.0.1", ov.local_port(), &target));

  std::vector<uint8_t> pkt;
  uint32_t rng = 12345;
  auto next = [&] {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
  };
  // Random garbage of every length, with and without the magic byte.
  for (int i = 0; i < 1500; ++i) {
    pkt.resize(next() % 1500);
    for (auto& b : pkt) b = static_cast<uint8_t>(next());
    if (!pkt.empty() && (i & 1)) pkt[0] = 0xA6;
    if (pkt.size() > 1 && (i % 4 == 1)) pkt[1] = static_cast<uint8_t>(next() % 0x12);
    if (!pkt.empty()) attacker.SendTo(target, pkt.data(), pkt.size());
  }
  // Every packet type, truncated at every length up to 80 bytes.
  for (uint8_t type : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10}) {
    for (size_t len = 2; len < 80; ++len) {
      pkt.assign(len, 0);
      pkt[0] = 0xA6;
      pkt[1] = type;
      for (size_t k = 2; k < len; ++k) pkt[k] = static_cast<uint8_t>(next());
      attacker.SendTo(target, pkt.data(), pkt.size());
    }
  }
  // P2P packets from 200 distinct unknown peer ids, several each.
  for (uint64_t src = 1; src <= 200; ++src) {
    for (int k = 0; k < 10; ++k) {
      pkt.assign(60, 0);
      pkt[0] = 0xA6;
      pkt[1] = 0x10;
      for (int b = 0; b < 8; ++b) pkt[2 + b] = static_cast<uint8_t>(src >> (8 * b));
      pkt[10] = static_cast<uint8_t>(k + 1);
      attacker.SendTo(target, pkt.data(), pkt.size());
    }
  }
  // The same from the configured server address (127.0.0.1:47819): forged RELAYED from 100
  // unknown peers (exercises the 8-per-source / 64-total buffer), S2C and HELLO_ACK with bad
  // tags, and REHELLO (rate-limited HELLO).
  net::UdpSocket fake_server;
  REQUIRE(fake_server.Open(AF_INET, 47819, &err));
  for (uint64_t src = 1; src <= 100; ++src) {
    for (int k = 0; k < 10; ++k) {
      pkt.assign(10 + 60, 0);
      pkt[0] = 0xA6;
      pkt[1] = 0x07;
      for (int b = 0; b < 8; ++b) pkt[2 + b] = static_cast<uint8_t>(src >> (8 * b));
      pkt[10] = 0xA6;
      pkt[11] = 0x10;
      std::memcpy(pkt.data() + 12, pkt.data() + 2, 8);
      pkt[20] = static_cast<uint8_t>(k + 1);
      fake_server.SendTo(target, pkt.data(), pkt.size());
    }
  }
  for (uint8_t type : {0x02, 0x04}) {
    for (int k = 0; k < 50; ++k) {
      pkt.assign(18 + 16 + 1 + (next() % 200), 0);
      for (auto& b : pkt) b = static_cast<uint8_t>(next());
      pkt[0] = 0xA6;
      pkt[1] = type;
      fake_server.SendTo(target, pkt.data(), pkt.size());
    }
  }
  const uint8_t rehello[2] = {0xA6, 0x05};
  for (int k = 0; k < 20; ++k) fake_server.SendTo(target, rehello, sizeof(rehello));
  SleepMs(300);
  CHECK(ov.running());
  const auto c = ov.counters();
  CHECK(c.datagrams_recv > 1000);
  CHECK(c.malformed > 500);
  CHECK(c.unknown_direct >= 1000);  // direct packets from unknown peers are never buffered
  CHECK(c.unknown_buffered <= 64);   // 64 total (8 per source)
  CHECK(c.unknown_buffered >= 8);
  CHECK(c.unknown_dropped >= 1000 - 64);
  CHECK(c.auth_failures >= 50);      // forged HELLO_ACKs fail the tag (S2C has no session yet)
  CHECK(c.rehello_recv == 20);
  CHECK(c.hello_sent <= 3);          // REHELLO is rate limited to one HELLO per second
  CHECK(c.intro_reqs <= 64);  // bounded in flight
  CHECK(c.hello_acks == 0);
  CHECK(ov.state() == NetState::kConnecting);
  std::fprintf(stderr,
               "  recv %llu malformed %llu auth %llu unknown direct %llu buffered %llu dropped "
               "%llu intro %llu hello %llu\n",
               (unsigned long long)c.datagrams_recv, (unsigned long long)c.malformed,
               (unsigned long long)c.auth_failures, (unsigned long long)c.unknown_direct,
               (unsigned long long)c.unknown_buffered, (unsigned long long)c.unknown_dropped,
               (unsigned long long)c.intro_reqs, (unsigned long long)c.hello_sent);
  ov.Stop();
}

TEST_CASE("overlay: network thread ticks and status JSON is valid") {
  Overlay ov;
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  std::atomic<int> ticks{0};
  ov.SetTickHandler([&](uint64_t) { ticks.fetch_add(1); });
  REQUIRE(ov.Start(DeadServerConfig(kClientPortBase), sk));
  SleepMs(200);
  CHECK(ticks.load() >= 15);  // ~5 ms loop
  const json st = json::parse(ov.StatusJson(), nullptr, false);
  REQUIRE(st.is_object());
  CHECK(st["server"]["state"] == "connecting");
  CHECK(st["max_payload"] == 1346);
  CHECK(st["counters"]["hello_sent"].get<uint64_t>() >= 1);
  ov.Stop();
}

// --- Against the real server ---

TEST_CASE("[server] overlay: three clients register, whoami, state callback") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  TestClient c("Bird", kClientPortBase + 2, server);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  REQUIRE(c.Start());
  std::fprintf(stderr, "  three registered in %llu ms\n", (unsigned long long)ElapsedMs(t0));
  std::set<uint32_t> vips = {a.vip(), b.vip(), c.vip()};
  CHECK(vips.size() == 3);
  for (uint32_t v : vips) CHECK(IsVip(v));
  const auto reg = a.overlay().registration();
  CHECK(reg.valid);
  CHECK(reg.gamertag.rfind("Talisman", 0) == 0);
  CHECK((reg.xuid >> 48) == 0x0009);
  CHECK(reg.public_ip == "127.0.0.1");
  CHECK(reg.public_port == a.overlay().local_port());
  CHECK(a.overlay().state() == NetState::kOnline);
  const auto states = a.states();
  REQUIRE(!states.empty());
  CHECK(states.back() == NetState::kOnline);

  const RpcResult r = a.overlay().Call("whoami", "{}");
  CHECK(r.transport_ok);
  CHECK(r.ok);
  const json w = json::parse(r.result_json, nullptr, false);
  REQUIRE(w.is_object());
  CHECK(w["vip"] == Ipv4ToString(a.vip()));
  CHECK(w["peer_id"] == U64ToHex(reg.peer_id));
  // Server-reported errors are passed through.
  const RpcResult bad = a.overlay().Call("no.such.method", "{}");
  CHECK(bad.transport_ok);
  CHECK(!bad.ok);
  CHECK(!bad.err.empty());
  // RTT measured from HELLO/HELLO_ACK.
  CHECK(a.overlay().ServerRttMs() >= 1);

  // A second boot with the same key keeps the identity.
  const PeerId first_id = reg.peer_id;
  const uint32_t first_vip = a.vip();
  a.Stop();
  REQUIRE(a.Start());
  CHECK(a.overlay().registration().peer_id == first_id);
  CHECK(a.vip() == first_vip);
}

TEST_CASE("[server] overlay: fragmented RPC request and response") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  REQUIRE(a.Start());
  // Four 1000-byte binary properties: ~5.5 KB request (6 fragments) and a similar response.
  json props = json::array();
  std::vector<std::string> blobs;
  for (int i = 0; i < 4; ++i) {
    std::vector<uint8_t> raw(1000);
    for (size_t k = 0; k < raw.size(); ++k) raw[k] = static_cast<uint8_t>(k * (i + 3));
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t k = 0; k < raw.size(); k += 3) {
      const uint32_t n = (uint32_t{raw[k]} << 16) |
                         (k + 1 < raw.size() ? uint32_t{raw[k + 1]} << 8 : 0) |
                         (k + 2 < raw.size() ? raw[k + 2] : 0);
      b64 += kB64[(n >> 18) & 63];
      b64 += kB64[(n >> 12) & 63];
      b64 += k + 1 < raw.size() ? kB64[(n >> 6) & 63] : '=';
      b64 += k + 2 < raw.size() ? kB64[n & 63] : '=';
    }
    blobs.push_back(b64);
    props.push_back({{"id", 0x40000100 + i}, {"t", 6}, {"v", b64}});
  }
  const json args = {{"title", "4E4D07D1"},  {"compat", TestClient::Compat()},
                     {"flags", 0x0F},        {"public_slots", 8},
                     {"private_slots", 0},   {"contexts", json::array({{0x800A, 1}, {0x800B, 2}})},
                     {"properties", props}};
  const std::string args_s = args.dump();
  CHECK(args_s.size() > 4 * 1100);
  const RpcResult created = a.overlay().Call("session.create", args_s);
  REQUIRE(created.transport_ok);
  if (!created.ok) std::fprintf(stderr, "  %s: %s\n", created.err.c_str(), created.msg.c_str());
  REQUIRE(created.ok);
  const json cr = json::parse(created.result_json, nullptr, false);
  REQUIRE(cr.is_object());
  const RpcResult got =
      a.overlay().Call("session.get", json{{"session_id", cr["session_id"]}}.dump());
  REQUIRE(got.transport_ok);
  REQUIRE(got.ok);
  CHECK(got.result_json.size() > 4 * 1100);
  const json doc = json::parse(got.result_json, nullptr, false);
  REQUIRE(doc.is_object());
  REQUIRE(doc["properties"].size() == 4);
  for (int i = 0; i < 4; ++i) CHECK(doc["properties"][i]["v"] == blobs[static_cast<size_t>(i)]);
  std::fprintf(stderr, "  request %zu bytes, response %zu bytes\n", args_s.size(),
               got.result_json.size());
  a.overlay().Call("session.delete", json{{"session_id", cr["session_id"]}}.dump());
}

TEST_CASE("[server] overlay: introduction, direct loopback path, datagrams both ways") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  const uint32_t va = a.vip(), vb = b.vip();

  // First send to an unknown vIP: queued, INTRO_REQ by vIP, flushed over the relay.
  const auto t0 = std::chrono::steady_clock::now();
  SendBurst(a, vb, 0, 5, 200, Channel::kDatagram, 0);
  REQUIRE(b.WaitCountFrom(va, 5, 3000));
  std::fprintf(stderr, "  first datagrams delivered after %llu ms\n",
               (unsigned long long)ElapsedMs(t0));
  OverlayPeerInfo info;
  REQUIRE(b.Peer(va, &info));  // B learned A through the unsolicited INTRO
  CHECK(info.peer_id == a.overlay().registration().peer_id);

  // Both sides validate a lan (loopback) path.
  REQUIRE(WaitFor([&] { return a.IsDirectTo(vb) && b.IsDirectTo(va); }, 4000));
  REQUIRE(a.Peer(vb, &info));
  CHECK(info.path_type == "lan");
  for (const auto& c : info.cands) {
    std::fprintf(stderr, "  cand %s %s %s %u ms\n", c.addr.c_str(), c.type.c_str(),
                 c.validated ? "validated" : "-", c.rtt_ms);
  }
  if (a.overlay().has_ipv6()) {
    // The [::1] loopback candidate validates through the IPv6 socket.
    CHECK(WaitFor(
        [&] {
          OverlayPeerInfo p;
          if (!a.Peer(vb, &p)) return false;
          for (const auto& c : p.cands)
            if (c.addr.rfind("[::1]:", 0) == 0 && c.validated) return true;
          return false;
        },
        3000));
  }
  std::fprintf(stderr, "  A->B direct via %s (%s, %u ms) after %llu ms\n", info.path.c_str(),
               info.path_type.c_str(), info.rtt_ms, (unsigned long long)ElapsedMs(t0));

  const auto before_a = a.overlay().counters();
  const auto before_b = b.overlay().counters();
  SendBurst(a, vb, 100, 50, 1000);
  SendBurst(b, va, 200, 50, 1346, Channel::kQos);
  CHECK(b.WaitCountFrom(va, 5 + 50, 3000));
  CHECK(a.WaitCountFrom(vb, 50, 3000));
  const auto after_a = a.overlay().counters();
  const auto after_b = b.overlay().counters();
  CHECK(after_a.data_sent_direct - before_a.data_sent_direct == 50);
  CHECK(after_a.data_sent_relay == before_a.data_sent_relay);
  CHECK(after_b.data_recv_direct - before_b.data_recv_direct == 50);
  CHECK(after_b.data_sent_direct - before_b.data_sent_direct == 50);

  // Contents and channels intact.
  bool intact = true;
  for (const auto& r : a.received()) {
    if (r.vip != vb) continue;
    const uint32_t seq = (uint32_t{r.data[0]} << 24) | (uint32_t{r.data[1]} << 16) |
                         (uint32_t{r.data[2]} << 8) | r.data[3];
    intact = intact && r.channel == Channel::kQos && r.data == Payload(seq, 1346);
  }
  CHECK(intact);

  // A handler may call Send: echo round trip.
  b.set_echo(true);
  const size_t had = a.CountFrom(vb);
  SendBurst(a, vb, 300, 10, 64);
  CHECK(a.WaitCountFrom(vb, had + 10, 2000));
  b.set_echo(false);
  const json st = json::parse(a.overlay().StatusJson(), nullptr, false);
  REQUIRE(st.is_object());
  CHECK(st["peers"].size() == 1);
  CHECK(st["peers"][0]["path_type"] == "lan");
}

TEST_CASE("[server] overlay: force_relay exchanges through the relay") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  a.config().force_relay = true;
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  const uint32_t va = a.vip(), vb = b.vip();
  SendBurst(a, vb, 0, 20, 1346);
  REQUIRE(b.WaitCountFrom(va, 20, 3000));
  SendBurst(b, va, 0, 20, 700);
  REQUIRE(a.WaitCountFrom(vb, 20, 3000));
  SleepMs(1500);  // B keeps probing A's candidates; A must never answer directly
  CHECK(!a.IsDirectTo(vb));
  CHECK(!b.IsDirectTo(va));
  const auto ca = a.overlay().counters();
  const auto cb = b.overlay().counters();
  CHECK(ca.data_sent_relay == 20);
  CHECK(ca.data_sent_direct == 0);
  CHECK(ca.probes_sent == 0);
  CHECK(ca.data_recv_relay == 20);
  CHECK(cb.data_sent_direct == 0);
  CHECK(cb.probe_acks_recv == 0);
}

TEST_CASE("[server] overlay: direct path falls back to the relay after 8 s of silence") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  const uint32_t va = a.vip(), vb = b.vip();
  SendBurst(a, vb, 0, 1, 10);
  REQUIRE(WaitFor([&] { return a.IsDirectTo(vb) && b.IsDirectTo(va); }, 4000));
  // Cut every direct datagram on both ends; the relay still works.
  a.overlay().SetTestBlockDirect(true);
  b.overlay().SetTestBlockDirect(true);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(WaitFor([&] { return !a.IsDirectTo(vb) && !b.IsDirectTo(va); }, 11000));
  const uint64_t took = ElapsedMs(t0);
  std::fprintf(stderr, "  fell back to relay after %llu ms\n", (unsigned long long)took);
  CHECK(took >= 7000);
  const size_t had = b.CountFrom(va);
  SendBurst(a, vb, 10, 10, 500);
  CHECK(b.WaitCountFrom(va, had + 10, 2000));
  CHECK(a.overlay().counters().relay_fallbacks >= 1);
  CHECK(a.lost().empty());
  CHECK(b.lost().empty());
  // Unblock: probing (every 200 ms after the fallback) restores the direct path.
  a.overlay().SetTestBlockDirect(false);
  b.overlay().SetTestBlockDirect(false);
  CHECK(WaitFor([&] { return a.IsDirectTo(vb) && b.IsDirectTo(va); }, 4000));
}

TEST_CASE("[server] overlay: REHELLO recovery after a server restart") {
  if (!TestServer::CanRestart()) SKIP("needs AC6NET_TEST_SERVER_BIN (server restart)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  a.config().force_relay = true;  // all peer traffic depends on the server's pair records
  b.config().force_relay = true;
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  const uint32_t va = a.vip(), vb = b.vip();
  const PeerId pa = a.overlay().registration().peer_id;
  SendBurst(a, vb, 0, 3, 100);
  REQUIRE(b.WaitCountFrom(va, 3, 3000));

  for (const bool graceful : {false, true}) {
    // SIGKILL: no notice, so the next C2S draws REHELLO. SIGTERM: the server's shutdown NOTICE
    // makes clients re-register on their own.
    const uint64_t rehellos = a.overlay().counters().rehello_recv;
    SleepMs(1200);  // past the server's 500 ms accounts.json debounce
    REQUIRE(server.Restart(graceful));
    const auto t0 = std::chrono::steady_clock::now();
    const RpcResult r = a.overlay().Call("whoami", "{}", 6000);
    CHECK(r.transport_ok);
    CHECK(r.ok);
    std::fprintf(stderr, "  %s restart: whoami ok after %llu ms\n",
                 graceful ? "graceful" : "hard", (unsigned long long)ElapsedMs(t0));
    if (!graceful) CHECK(a.overlay().counters().rehello_recv > rehellos);
    CHECK(a.overlay().registration().peer_id == pa);
    CHECK(a.vip() == va);
    CHECK(a.overlay().state() == NetState::kOnline);

    // Relay authorisation is re-established once both ends have re-registered.
    const size_t had = b.CountFrom(va);
    const bool flowed = WaitFor(
        [&] {
          SendBurst(a, vb, 1000, 1, 100, Channel::kDatagram, 0);
          return b.CountFrom(va) > had;
        },
        12000);
    CHECK(flowed);
    std::fprintf(stderr, "  relay flowing again %llu ms after the restart\n",
                 (unsigned long long)ElapsedMs(t0));
    CHECK(a.lost().empty());
    CHECK(b.lost().empty());
  }
}

TEST_CASE("[server] overlay: RPC and datagrams under 20% injected loss") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  a.overlay().SetTestLoss(0.2, 0.2);
  b.overlay().SetTestLoss(0.2, 0.2);
  REQUIRE(a.Start(10000));
  REQUIRE(b.Start(10000));
  const uint32_t va = a.vip(), vb = b.vip();
  int ok = 0;
  for (int i = 0; i < 10; ++i) ok += a.overlay().Call("whoami", "{}").ok;
  CHECK(ok == 10);
  // A fragmented request under loss: fragments accumulate across retransmissions.
  json props = json::array();
  for (int i = 0; i < 3; ++i) {
    props.push_back({{"id", 0x40000200 + i}, {"t", 6}, {"v", std::string(1200, 'A')}});
  }
  const json args = {{"title", "4E4D07D1"}, {"compat", TestClient::Compat()}, {"flags", 0x0F},
                     {"public_slots", 4},   {"private_slots", 0},            {"contexts", json::array()},
                     {"properties", props}};
  const RpcResult created = a.overlay().Call("session.create", args.dump(), 8000);
  CHECK(created.ok);

  // Datagrams: introduction and probing still complete; most datagrams arrive.
  REQUIRE(WaitFor(
      [&] {
        SendBurst(a, vb, 0, 1, 50, Channel::kDatagram, 0);
        return b.CountFrom(va) > 0;
      },
      6000));
  CHECK(WaitFor([&] { return a.IsDirectTo(vb); }, 6000));
  const size_t had = b.CountFrom(va);
  SendBurst(a, vb, 1, 100, 300);
  SleepMs(500);
  const size_t got = b.CountFrom(va) - had;
  std::fprintf(stderr, "  %zu/100 datagrams through 20%%+20%% loss; rpc retransmits %llu\n", got,
               (unsigned long long)a.overlay().counters().rpc_retransmits);
  CHECK(got >= 45);
  CHECK(got <= 100);
  CHECK(a.overlay().counters().test_dropped > 0);
  CHECK(a.overlay().counters().rpc_retransmits > 0);
  CHECK(a.lost().empty());
}

TEST_CASE("[server] overlay: BYE and silence are reported as peer lost") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  TestClient c("Bird", kClientPortBase + 2, server);
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  REQUIRE(c.Start());
  const uint32_t vb = b.vip(), vc = c.vip();
  SendBurst(a, vb, 0, 2, 10);
  SendBurst(a, vc, 0, 2, 10);
  REQUIRE(WaitFor([&] { return a.Peer(vb, nullptr) && a.Peer(vc, nullptr); }, 3000));

  // BYE: immediate.
  const auto t0 = std::chrono::steady_clock::now();
  b.Stop();
  REQUIRE(WaitFor([&] { return a.lost().size() == 1; }, 2000));
  CHECK(a.lost()[0] == vb);
  CHECK(ElapsedMs(t0) < 1000);
  CHECK(!a.Peer(vb, nullptr));

  // Silence: C goes dark (all datagrams dropped), reported after ~15 s.
  c.overlay().SetTestLoss(1.0, 1.0);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(WaitFor([&] { return a.lost().size() == 2; }, 18000));
  const uint64_t took = ElapsedMs(t1);
  std::fprintf(stderr, "  silent peer lost after %llu ms\n", (unsigned long long)took);
  CHECK(a.lost()[1] == vc);
  CHECK(took >= 14000);
  CHECK(took <= 16500);
}

TEST_CASE("[server] overlay: HELLO is retransmitted until a late server answers") {
  if (!TestServer::CanRestart()) SKIP("needs AC6NET_TEST_SERVER_BIN (controls the server)");
  TestServer server;
  REQUIRE(server.Start());  // creates the key; stop it so the client starts with no server
  const std::string key = server.key();
  server.Stop();
  TestClient a("Talisman", kClientPortBase, server);
  REQUIRE(a.overlay().Start(a.config(), a.secret_key()));
  SleepMs(2500);  // HELLOs at 0, 1 s, 3 s (then 7 s, 12 s, ...)
  CHECK(a.overlay().state() == NetState::kConnecting);
  CHECK(a.overlay().counters().hello_sent >= 2);
  REQUIRE(server.Restart());
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(a.overlay().WaitOnline(6000));
  std::fprintf(stderr, "  registered %llu ms after the server came up (hellos %llu)\n",
               (unsigned long long)ElapsedMs(t0),
               (unsigned long long)a.overlay().counters().hello_sent);
  CHECK(a.overlay().state() == NetState::kOnline);
  CHECK(a.overlay().Call("whoami", "{}").ok);
}

TEST_CASE("[server] overlay: offline after 30 s without the server, online on the next ACK") {
  if (!TestServer::CanRestart()) SKIP("needs AC6NET_TEST_SERVER_BIN (controls the server)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  REQUIRE(a.Start());
  const auto t0 = std::chrono::steady_clock::now();  // HELLO_ACK: the last authenticated S2C
  SleepMs(1200);  // past the server's accounts.json debounce
  server.Stop(/*graceful=*/false);
  REQUIRE(WaitFor([&] { return a.overlay().state() == NetState::kOffline; }, 33000));
  const uint64_t took = ElapsedMs(t0);
  std::fprintf(stderr, "  offline %llu ms after the last S2C\n", (unsigned long long)took);
  CHECK(took >= 29900);
  CHECK(took <= 31000);
  CHECK(a.overlay().Call("whoami", "{}").err == "offline");
  const auto states = a.states();
  REQUIRE(states.size() >= 2);
  CHECK(states.back() == NetState::kOffline);
  CHECK(a.overlay().registration().valid);  // identity is kept while offline
  REQUIRE(server.Restart());
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(WaitFor([&] { return a.overlay().state() == NetState::kOnline; }, 7000));
  std::fprintf(stderr, "  online again %llu ms after the server returned\n",
               (unsigned long long)ElapsedMs(t1));
  CHECK(a.states().back() == NetState::kOnline);
  CHECK(a.overlay().Call("whoami", "{}").ok);
}

TEST_CASE("[server] overlay: packets from a not-yet-introduced peer are buffered, then delivered") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  a.config().force_relay = true;  // B's first datagrams then reach A only through the relay
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  const uint32_t va = a.vip(), vb = b.vip();
  // A cannot hear the server's INTRO(B) for a moment, but RELAYED packets still arrive.
  a.overlay().SetTestDropServerControl(true);
  SendBurst(b, va, 0, 5, 300, Channel::kDatagram, 0);
  REQUIRE(WaitFor([&] { return a.overlay().counters().unknown_buffered >= 5; }, 2000));
  CHECK(a.CountFrom(vb) == 0);
  a.overlay().SetTestDropServerControl(false);
  // INTRO(B) arrives (server resend or A's own INTRO_REQ by peer id): the buffer is replayed.
  const bool delivered = a.WaitCountFrom(vb, 5, 3000);
  if (!delivered) std::fprintf(stderr, "  %s\n", a.overlay().StatusJson().c_str());
  REQUIRE(delivered);
  CHECK(a.overlay().counters().unknown_dropped == 0);
  bool in_order = true;
  const auto got = a.received();
  for (size_t i = 0; i < got.size(); ++i) in_order = in_order && got[i].data == Payload(i, 300);
  CHECK(in_order);
}

TEST_CASE("[server] overlay: concurrent Send/Call/status while the peer restarts") {
  if (!TestServer::Available()) SKIP("no ac6lobby (set AC6NET_TEST_SERVER_BIN)");
  TestServer server;
  REQUIRE(server.Start());
  TestClient a("Talisman", kClientPortBase, server);
  TestClient b("Pixy", kClientPortBase + 1, server);
  REQUIRE(a.Start());
  REQUIRE(b.Start());
  b.set_echo(true);
  const uint32_t vb = b.vip();
  std::atomic<bool> stop{false};
  std::atomic<int> sends_ok{0}, calls_ok{0}, calls{0}, status_reads{0};
  std::mutex err_mu;
  std::string first_err;
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      for (uint32_t i = 0; !stop; ++i) {
        const auto p = Payload(static_cast<uint32_t>(t) << 24 | i, 64 + (i % 1200));
        sends_ok += a.overlay().Send(vb, t == 2 ? Channel::kQos : Channel::kDatagram, p.data(),
                                     p.size());
        SleepMs(1);
      }
    });
  }
  threads.emplace_back([&] {
    while (!stop) {
      ++calls;
      const RpcResult r = a.overlay().Call("whoami", "{}", 3000);
      calls_ok += r.ok;
      if (!r.ok) {
        std::lock_guard<std::mutex> lock(err_mu);
        if (first_err.empty()) first_err = r.err + ": " + r.msg;
      }
      SleepMs(25);  // the server allows 50 RPCs/s per client
    }
  });
  threads.emplace_back([&] {
    while (!stop) {
      const json st = json::parse(a.overlay().StatusJson(), nullptr, false);
      if (st.is_object()) ++status_reads;
      a.overlay().Peers();
      a.overlay().counters();
      a.overlay().registration();
      a.overlay().ServerRttMs();
      a.vip();
    }
  });
  SleepMs(800);
  const size_t echoes_before = a.CountFrom(vb);
  // B restarts (BYE, new boot): A drops the record and re-introduces on its next Send.
  b.Stop();
  REQUIRE(b.Start());
  b.set_echo(true);
  SleepMs(3000);  // covers a 1 s INTRO_FAIL backoff if A asked while B was between boots
  stop = true;
  for (auto& th : threads) th.join();
  std::fprintf(stderr, "  sends %d, calls %d/%d, status reads %d, echoes %zu -> %zu\n",
               sends_ok.load(), calls_ok.load(), calls.load(), status_reads.load(), echoes_before,
               a.CountFrom(vb));
  CHECK(echoes_before > 50);
  CHECK(a.CountFrom(vb) > echoes_before + 50);  // traffic resumed with B's new boot
  CHECK(calls_ok.load() == calls.load());
  if (!first_err.empty()) std::fprintf(stderr, "  first RPC error: %s\n", first_err.c_str());
  CHECK(status_reads.load() > 10);
  CHECK(a.lost().size() >= 1);
  OverlayPeerInfo info;
  REQUIRE(a.Peer(vb, &info));
  CHECK(info.peer_id == b.overlay().registration().peer_id);
}
