// Parser robustness: authenticated-but-malformed server messages and peer packets, delivered by
// overlay_fake_server.h, must be rejected without crashing, leaking or misdelivering anything.
#include <atomic>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include "ac6net/netplay.h"
#include "crypto.h"
#include "mini_test.h"
#include "nlohmann/json.hpp"
#include "overlay.h"
#include "overlay_fake_server.h"
#include "overlay_internal.h"
#include "overlay_test_util.h"

using namespace ac6net;
using namespace overlay_test;
using json = nlohmann::json;

namespace {

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> b) { return std::vector<uint8_t>(b); }

std::vector<uint8_t> WithU32(uint8_t kind, uint32_t v, const std::string& tail = {}) {
  std::vector<uint8_t> out = {kind};
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

std::vector<uint8_t> RpcResp(uint32_t req_id, uint8_t idx, uint8_t cnt, const std::string& body) {
  std::vector<uint8_t> out = WithU32(0x20, req_id);
  out.push_back(idx);
  out.push_back(cnt);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

struct FakePeer {
  crypto::SecretKey sk{};
  crypto::PublicKey pk{};
  BootId boot{};
  PeerId id = FakeServer::kPeerId;
  uint32_t vip = FakeServer::kVip;
  crypto::Key send_key{};
  uint64_t ctr = 0;

  void Init(const crypto::PublicKey& client_pk, const BootId& client_boot) {
    crypto::GenerateKeypair(&sk, &pk);
    crypto::RandomBytes(boot.data(), boot.size());
    crypto::Key dh;
    crypto::Dh(sk, client_pk, &dh);
    send_key = crypto::Kdf(dh, "AC6NET-P2P-1", {pk, client_pk, boot, client_boot});
  }
  std::string IntroJson() const {
    return json{{"peer_id", U64ToHex(id)},
                {"vip", Ipv4ToString(vip)},
                {"pk", HexEncode(pk.data(), pk.size())},
                {"boot", HexEncode(boot.data(), boot.size())},
                {"xuid", "0009000000000099"},
                {"gamertag", "Fuzzer"},
                {"cands", json::array()}}
        .dump();
  }
  // RELAYED(A6 07 src || P2P(A6 10 src ctr || AEAD(inner))).
  std::vector<uint8_t> Relayed(const std::vector<uint8_t>& inner, uint64_t use_ctr = 0) {
    const uint64_t c = use_ctr ? use_ctr : ++ctr;
    std::vector<uint8_t> pkt(10 + 18 + inner.size() + 16);
    pkt[0] = 0xA6;
    pkt[1] = 0x07;
    for (int i = 0; i < 8; ++i) pkt[2 + i] = static_cast<uint8_t>(id >> (8 * i));
    uint8_t* p2p = pkt.data() + 10;
    p2p[0] = 0xA6;
    p2p[1] = 0x10;
    std::memcpy(p2p + 2, pkt.data() + 2, 8);
    for (int i = 0; i < 8; ++i) p2p[10 + i] = static_cast<uint8_t>(c >> (8 * i));
    crypto::Seal(send_key, c, p2p, 18, inner.data(), inner.size(), p2p + 18);
    return pkt;
  }
};

}  // namespace

TEST_CASE("overlay fuzz: authenticated malformed server messages and peer packets") {
  FakeServer fs;
  REQUIRE(fs.Start());
  Overlay ov;
  std::mutex mu;
  std::vector<std::vector<uint8_t>> delivered;
  std::vector<uint32_t> lost;
  ov.SetDataHandler([&](uint32_t vip, Channel ch, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lock(mu);
    delivered.emplace_back(d, d + n);
  });
  ov.SetPeerLostHandler([&](uint32_t vip) {
    std::lock_guard<std::mutex> lock(mu);
    lost.push_back(vip);
  });
  crypto::SecretKey sk;
  crypto::PublicKey pk;
  crypto::GenerateKeypair(&sk, &pk);
  Config cfg;
  cfg.enabled = true;
  cfg.server = fs.addr();
  cfg.server_key_hex = fs.key_hex();
  cfg.port = kClientPortBase;
  cfg.gamertag = "Fuzz";
  REQUIRE(ov.Start(cfg, sk));
  REQUIRE(ov.WaitOnline(3000));
  REQUIRE(ov.LocalVip() == 0x0A400001);
  const auto base = ov.counters();

  uint32_t rng = 777;
  auto next = [&] {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
  };
  // 1. Every S2C kind (and unknown ones) with random bodies.
  for (uint8_t kind : {0x00, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x7F, 0xFF}) {
    for (int k = 0; k < 40; ++k) {
      std::vector<uint8_t> inner(1 + next() % 80);
      for (auto& b : inner) b = static_cast<uint8_t>(next());
      inner[0] = kind;
      fs.SendS2C(inner);
    }
  }
  // 2. RPC_RESP framing errors with no call in flight.
  fs.SendS2C(RpcResp(1, 0, 0, "{}"));
  fs.SendS2C(RpcResp(1, 3, 2, "{}"));
  fs.SendS2C(RpcResp(1, 0, 33, "{}"));
  fs.SendS2C(Bytes({0x20, 1, 0}));
  // 3. KEEPALIVE_ACK / NOTICE / INTRO_FAIL junk.
  for (const char* body : {"", "{", "[]", "{\"public\":\"x\"}", "{\"public\":[1,2]}",
                           "{\"public\":[\"1.2.3.4\",70000]}", "{\"public\":[\"1.2.3.4\",-1]}",
                           "{\"public\":[\"1.2.3.4\"]}", "null", "\"str\""}) {
    std::vector<uint8_t> inner = {0x21};
    inner.insert(inner.end(), body, body + std::strlen(body));
    fs.SendS2C(inner);
    inner[0] = 0x24;
    fs.SendS2C(inner);
  }
  fs.SendS2C(Bytes({0x24, '{', '"', 'c', 'o', 'd', 'e', '"', ':', '1', '}'}));
  fs.SendS2C(Bytes({0x23, 1, 2}));
  fs.SendS2C(Bytes({0x23, 9, 9, 9, 9, 1}));
  // 4. INTRO records that must all be rejected.
  FakePeer peer;
  peer.Init(fs.client_pk(), fs.client_boot());
  const json good = json::parse(peer.IntroJson());
  std::vector<std::string> bad_intros = {"", "{", "[]", "123", "{}"};
  for (const char* field : {"peer_id", "vip", "pk", "boot"}) {
    json j = good;
    j.erase(field);
    bad_intros.push_back(j.dump());
    j = good;
    j[field] = 12345;
    bad_intros.push_back(j.dump());
    j = good;
    j[field] = "zz";
    bad_intros.push_back(j.dump());
  }
  for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
           {"vip", "192.168.0.1"},
           {"vip", "10.64.0.1"},                 // our own vIP
           {"peer_id", "00000000000000aa"},      // our own peer id
           {"peer_id", "0000000000000000"},
           {"pk", std::string(64, '0')},         // low-order key: DH output is zero
           {"boot", "0123"}}) {
    json j = good;
    j[field] = value;
    bad_intros.push_back(j.dump());
  }
  bad_intros.push_back(std::string(1300, '['));  // deep nesting
  for (const auto& body : bad_intros) fs.SendS2C(WithU32(0x22, 0, body));
  SleepMs(200);
  CHECK(ov.Peers().empty());

  // 5. A valid INTRO whose candidate list is junk: the peer is created, junk is skipped.
  json j = good;
  j["cands"] = json::array({json::array(), json::array({1, 2, 3}), json::array({"x", 1, "lan"}),
                            json::array({"1.2.3.4", 0, "lan"}),
                            json::array({"1.2.3.4", 70000, "lan"}),
                            json::array({"1.2.3.4", 5, 7}), json::array({"1.2.3.4", 5, "weird"}),
                            json::array({"1.2.3.4", -5, "lan"}), "notarray", 17,
                            json::array({"127.0.0.1", 40000, "srflx"})});
  for (int i = 0; i < 40; ++i) j["cands"].push_back(json::array({"127.0.0.1", 41000 + i, "lan"}));
  fs.SendS2C(WithU32(0x22, 0, j.dump()));
  REQUIRE(WaitFor([&] { return ov.Peers().size() == 1; }, 1000));
  const auto peers = ov.Peers();
  CHECK(peers[0].peer_id == FakeServer::kPeerId);
  CHECK(peers[0].vip == FakeServer::kVip);
  CHECK(peers[0].cands.size() == 16);  // capped
  CHECK(peers[0].cands[0].addr == "127.0.0.1:40000");

  // 6. Authenticated P2P packets with malformed inner messages, via RELAYED.
  for (const auto& inner : std::vector<std::vector<uint8_t>>{
           {0x01, 1, 2, 3},                   // PROBE too short
           {0x02, 1, 2},                      // PROBE_ACK too short
           {0x04},                            // DATA without a channel
           {0x04, 0x00, 1, 2},                // channel 0
           {0x04, 0x05, 1, 2},                // channel 5
           {0x04, 0xFF},                      // channel 255
           {0x77, 1, 2, 3},                   // unknown kind
           {0x03, 9, 9, 9}}) {                // KEEPALIVE with trailing bytes (accepted)
    fs.SendRaw(peer.Relayed(inner));
  }
  // An empty inner message, a mismatched outer/inner source, and a truncated RELAYED.
  fs.SendRaw(peer.Relayed({}));
  auto mismatched = peer.Relayed({0x03});
  mismatched[2] ^= 1;
  fs.SendRaw(mismatched);
  auto truncated = peer.Relayed({0x04, 0x01, 'x'});
  truncated.resize(30);
  fs.SendRaw(truncated);
  // Tampered ciphertext.
  auto tampered = peer.Relayed({0x04, 0x01, 't'});
  tampered.back() ^= 0x80;
  fs.SendRaw(tampered);
  // Valid DATA, then the same packet again (replay), then valid DATA with an empty payload.
  const auto valid = peer.Relayed({0x04, 0x01, 'o', 'k'});
  fs.SendRaw(valid);
  fs.SendRaw(valid);
  fs.SendRaw(peer.Relayed({0x04, 0x02}));
  SleepMs(300);
  {
    std::lock_guard<std::mutex> lock(mu);
    REQUIRE(delivered.size() == 2);
    CHECK((delivered[0] == std::vector<uint8_t>{'o', 'k'}));
    CHECK(delivered[1].empty());
  }

  // 7. RPC responses: reassembly with a fragment-count change, garbage JSON, out-of-order
  // fragments, and missing/odd fields.
  auto next_rpc_req_id = [&](size_t seen) -> uint32_t {
    uint32_t id = 0;
    WaitFor(
        [&] {
          const auto inners = fs.c2s_inners();
          for (size_t i = seen; i < inners.size(); ++i) {
            if (inners[i].size() >= 7 && inners[i][0] == 0x10) {
              id = inners[i][1] | (inners[i][2] << 8) | (inners[i][3] << 16) |
                   (uint32_t{inners[i][4]} << 24);
              return true;
            }
          }
          return false;
        },
        2000);
    return id;
  };
  {
    const size_t seen = fs.c2s_count();
    auto fut = std::async(std::launch::async, [&] { return ov.Call("whoami", "{}", 3000); });
    const uint32_t id = next_rpc_req_id(seen);
    REQUIRE(id != 0);
    fs.SendS2C(RpcResp(id, 0, 2, "{\"id\":"));       // 1 of 2...
    fs.SendS2C(RpcResp(id, 0, 3, "not"));            // ...count changes: start over
    fs.SendS2C(RpcResp(id, 2, 3, "json"));
    fs.SendS2C(RpcResp(id, 1, 3, " at all "));
    const RpcResult r = fut.get();
    CHECK(r.transport_ok);
    CHECK(!r.ok);
    CHECK(r.err == "internal");
  }
  {
    const size_t seen = fs.c2s_count();
    auto fut = std::async(std::launch::async, [&] { return ov.Call("whoami", "{}", 3000); });
    const uint32_t id = next_rpc_req_id(seen);
    REQUIRE(id != 0);
    const std::string body = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"r\":{\"x\":[1,2]}}";
    const size_t third = body.size() / 3;
    fs.SendS2C(RpcResp(id, 2, 3, body.substr(2 * third)));
    fs.SendS2C(RpcResp(id, 0, 3, body.substr(0, third)));
    fs.SendS2C(RpcResp(id, 0, 3, "DUPLICATE IGNORED"));
    fs.SendS2C(RpcResp(id, 1, 3, body.substr(third, third)));
    const RpcResult r = fut.get();
    CHECK(r.transport_ok);
    CHECK(r.ok);
    CHECK(r.result_json == "{\"x\":[1,2]}");
  }
  {
    const size_t seen = fs.c2s_count();
    auto fut = std::async(std::launch::async, [&] { return ov.Call("whoami", "{}", 3000); });
    const uint32_t id = next_rpc_req_id(seen);
    REQUIRE(id != 0);
    fs.SendS2C(RpcResp(id, 0, 1, "{\"ok\":\"yes\",\"err\":5}"));  // wrong types
    const RpcResult r = fut.get();
    CHECK(r.transport_ok);
    CHECK(!r.ok);
    CHECK(r.err == "internal");
  }
  {
    const size_t seen = fs.c2s_count();
    auto fut = std::async(std::launch::async, [&] { return ov.Call("whoami", "{}", 3000); });
    const uint32_t id = next_rpc_req_id(seen);
    REQUIRE(id != 0);
    fs.SendS2C(RpcResp(id, 0, 1, "{\"ok\":false,\"err\":\"not_found\",\"msg\":\"nope\"}"));
    const RpcResult r = fut.get();
    CHECK(r.transport_ok);
    CHECK(!r.ok);
    CHECK(r.err == "not_found");
    CHECK(r.msg == "nope");
  }

  // 8. INTRO flood: the peer table is bounded.
  for (int i = 0; i < 300; ++i) {
    FakePeer p;
    p.Init(fs.client_pk(), fs.client_boot());
    p.id = 0x5000000000000000ull + static_cast<uint64_t>(i);
    p.vip = 0x0A410000 + static_cast<uint32_t>(i);
    fs.SendS2C(WithU32(0x22, 0, p.IntroJson()));
  }
  WaitFor([&] { return ov.Peers().size() >= 256; }, 2000);
  SleepMs(200);
  CHECK(ov.Peers().size() == 256);

  // 9. BYE from the first peer is reported as lost.
  fs.SendRaw(peer.Relayed({0x05}));
  CHECK(WaitFor(
      [&] {
        std::lock_guard<std::mutex> lock(mu);
        return lost.size() == 1 && lost[0] == FakeServer::kVip;
      },
      1000));

  CHECK(ov.running());
  const auto c = ov.counters();
  std::fprintf(stderr, "  malformed %llu replays %llu auth %llu intros %llu\n",
               (unsigned long long)(c.malformed - base.malformed),
               (unsigned long long)(c.replays - base.replays),
               (unsigned long long)(c.auth_failures - base.auth_failures),
               (unsigned long long)(c.intros_recv - base.intros_recv));
  CHECK(c.malformed - base.malformed >= 200);
  CHECK(c.replays - base.replays >= 1);
  CHECK(c.auth_failures - base.auth_failures >= 1);
  const json st = json::parse(ov.StatusJson(), nullptr, false);
  CHECK(st.is_object());
  ov.Stop();
}
