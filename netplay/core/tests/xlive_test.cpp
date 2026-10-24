// Unit tests for the xlive layer: the wire codec, and XLive driven by an in-memory lobby
// (xlive_fake_backend.h). Run with: ac6net_tests xlive
#include "ac6net/xlive.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ac6net/netplay.h"
#include "mini_test.h"
#include "xlive_backend.h"
#include "xlive_codec.h"
#include "xlive_fake_backend.h"

using namespace ac6net;
using namespace ac6net::xlive;
using xlive_test::FakeBackend;
using xlive_test::FakeClient;
using xlive_test::FakeLobby;
using xlive_test::json;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kPlayerHost = 0x42F;  // AC6 Player Match host flags (C7)
constexpr uint32_t kPlayerJoin = 0x42E;
constexpr uint32_t kRankedHost = 0x53F;  // Ranked Match
constexpr uint32_t kRankedJoin = 0x53E;

FakeClient MakeClient(int n) {
  FakeClient c;
  c.peer_id = 0x1000 + uint64_t(n);
  c.xuid = 0x0009000000000000ull | uint64_t(n);
  c.vip = 0x0A400000u | uint32_t(n);
  c.gamertag = "Pilot" + std::to_string(n);
  return c;
}

struct Console {
  Console(FakeLobby* lobby, int n) : backend(lobby, MakeClient(n)), x(MakeXLiveForTest(&backend)) {}
  FakeBackend backend;
  std::unique_ptr<XLive> x;
  Xuid xuid() const { return backend.me().xuid; }
};

Property Int32(uint32_t id, int32_t v) {
  Property p;
  p.id = id;
  p.value.type = DataType::kInt32;
  p.value.i = v;
  return p;
}

Property Int64(uint32_t id, int64_t v) {
  Property p;
  p.id = id;
  p.value.type = DataType::kInt64;
  p.value.i = v;
  return p;
}

uint64_t KidU64(const XnKid& kid) {
  uint64_t v = 0;
  for (uint8_t b : kid.b) v = (v << 8) | b;
  return v;
}

bool WaitFor(const std::function<bool()>& pred, int timeout_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

[[maybe_unused]] bool TraceHas(const std::vector<std::string>& lines, const std::string& event,
                               const std::string& fragment = {}) {
  for (const auto& l : lines) {
    if (l.find("\"ev\":\"" + event + "\"") != std::string::npos &&
        (fragment.empty() || l.find(fragment) != std::string::npos)) {
      return true;
    }
  }
  return false;
}

ac6net::RpcResult RawOk(const std::string& result_json) {
  ac6net::RpcResult r;
  r.transport_ok = true;
  r.ok = true;
  r.result_json = result_json;
  return r;
}

// Creates a hosted session on `handle` and joins the local user.
SessionInfo Host(Console& c, SessionHandle handle, uint32_t flags, uint32_t slots,
                 uint64_t* nonce_out = nullptr) {
  c.x->SessionHandleCreated(handle);
  SessionInfo info;
  uint64_t nonce = 0;
  REQUIRE(c.x->SessionCreate(handle, flags, slots, 0, 0, &info, &nonce) == hr::kOk);
  REQUIRE(c.x->SessionJoinLocal(handle, {0}, {false}) == hr::kOk);
  if (nonce_out) *nonce_out = nonce;
  return info;
}

}  // namespace

// --- Codec ---

TEST_CASE("xlive codec: base64 round trip and strict decoding") {
  using codec::Base64Decode;
  using codec::Base64Encode;
  const auto enc = [](const std::string& s) {
    return Base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  CHECK(enc("") == "");
  CHECK(enc("f") == "Zg==");
  CHECK(enc("fo") == "Zm8=");
  CHECK(enc("foo") == "Zm9v");
  CHECK(enc("foobar") == "Zm9vYmFy");
  std::vector<uint8_t> out;
  REQUIRE(Base64Decode("Zm9vYmE=", &out));
  CHECK(std::string(out.begin(), out.end()) == "fooba");
  REQUIRE(Base64Decode("", &out));
  CHECK(out.empty());
  for (const char* bad : {"Zg=", "Zg=a", "Z===", "====", "Zm9v!A==", "Zm 9", "Zm9v\n", "=Zm9"}) {
    CHECK(!Base64Decode(bad, &out));
  }
  std::mt19937 rng(7);
  for (size_t len = 0; len < 64; ++len) {
    std::vector<uint8_t> data(len);
    for (auto& b : data) b = uint8_t(rng());
    REQUIRE(Base64Decode(Base64Encode(data.data(), data.size()), &out));
    CHECK(out == data);
  }
}

TEST_CASE("xlive codec: hex, IPv4 and ID helpers") {
  uint64_t v = 0;
  CHECK(codec::HexU64(0x0009000000000001ull) == "0009000000000001");
  CHECK(codec::ParseHexU64("80AbCdEf01234567", &v) && v == 0x80abcdef01234567ull);
  CHECK(!codec::ParseHexU64("80abcdef0123456", &v));
  CHECK(!codec::ParseHexU64("80abcdef012345678", &v));
  CHECK(!codec::ParseHexU64("80abcdef0123456g", &v));
  XnKid kid;
  CHECK(codec::KidFromHex("8001020304050607", &kid) && kid.b[0] == 0x80 && kid.b[7] == 7);
  CHECK(codec::KidToHex(kid) == "8001020304050607");
  XnKey key;
  CHECK(!codec::KeyFromHex("00", &key));
  uint32_t ip = 0;
  CHECK(codec::ParseIpv4("10.64.0.7", &ip) && ip == 0x0A400007);
  CHECK(codec::FormatIpv4(0x0A400007) == "10.64.0.7");
  for (const char* bad : {"", "1.2.3", "1.2.3.4.", "256.0.0.1", "1..2.3", "1.2.3.4x", "a.b.c.d",
                          "1.2.3.1234", "-1.2.3.4"}) {
    CHECK(!codec::ParseIpv4(bad, &ip));
  }
}

TEST_CASE("xlive codec: properties and contexts") {
  const auto round_trip = [](const Property& p) {
    json j;
    REQUIRE(codec::PropertyToJson(p, &j));
    Property back;
    REQUIRE(codec::PropertyFromJson(codec::Parse(codec::Dump(j)), &back));
    CHECK(back == p);
    return j;
  };
  CHECK(round_trip(Int32(0x1000000C, -5)).dump() == R"({"id":268435468,"t":1,"v":-5})");
  round_trip(Int64(0x20000001, (int64_t{1} << 62) + 1));  // exact int64
  Property d;
  d.id = 3;
  d.value.type = DataType::kDouble;
  d.value.d = 1.25;
  round_trip(d);
  d.value.type = DataType::kFloat;
  d.value.d = 0.5;
  round_trip(d);
  Property u;
  u.id = 0x40000001;
  u.value.type = DataType::kUnicode;
  u.value.bytes = {0x00, 'A', 0x00, 'B', 0x00, 0x00};  // UTF-16BE "AB" + terminator
  CHECK(round_trip(u)["v"] == "AEEAQgAA");
  u.value.type = DataType::kBinary;
  u.value.bytes = {1, 2, 3};
  round_trip(u);
  Property ctx;
  ctx.id = 0x800A;
  ctx.value.type = DataType::kContext;
  ctx.value.i = 0xFFFFFFFF;
  round_trip(ctx);
  Property dt;
  dt.id = 7;
  dt.value.type = DataType::kDateTime;
  dt.value.i = -1;
  round_trip(dt);

  Property null_value;
  json j;
  CHECK(!codec::PropertyToJson(null_value, &j));  // kNull cannot be sent

  // Unset columns from a server come back as kNull.
  Property out;
  REQUIRE(codec::PropertyFromJson(json::parse(R"({"id":2,"t":255})"), &out));
  CHECK(out.id == 2 && out.value.type == DataType::kNull);
  // An int32 sent as its unsigned reinterpretation is accepted and sign-extended.
  REQUIRE(codec::PropertyFromJson(json::parse(R"({"id":2,"t":1,"v":4294967295})"), &out));
  CHECK(out.value.i == -1);

  for (const char* bad :
       {R"({"t":1,"v":1})", R"({"id":1,"v":1})", R"({"id":1,"t":8,"v":1})", R"({"id":1,"t":1})",
        R"({"id":1,"t":1,"v":"1"})", R"({"id":1,"t":1,"v":4294967296})",
        R"({"id":1,"t":1,"v":-2147483649})", R"({"id":1,"t":1,"v":1.5})",
        R"({"id":1,"t":2,"v":9223372036854775808})", R"({"id":1,"t":4,"v":"Zg="})",
        R"({"id":1,"t":6,"v":7})", R"({"id":-1,"t":1,"v":1})", R"({"id":4294967296,"t":1,"v":1})",
        R"({"id":1,"t":0,"v":-1})", R"({"id":1,"t":3,"v":null})", R"([1,1,1])", R"("x")"}) {
    CHECK(!codec::PropertyFromJson(json::parse(bad), &out));
  }

  Context c;
  CHECK(codec::ContextFromJson(json::parse("[32778,1]"), &c) && c.id == 0x800A && c.value == 1);
  for (const char* bad : {"[1]", "[1,2,3]", "[-1,2]", "[1,4294967296]", "[\"1\",2]", "{}", "1"}) {
    CHECK(!codec::ContextFromJson(json::parse(bad), &c));
  }
  size_t skipped = 0;
  const auto ctxs = codec::ContextsFromJson(json::parse("[[1,2],[3],[4,5]]"), &skipped);
  CHECK(ctxs.size() == 2 && skipped == 1);
}

TEST_CASE("xlive codec: session documents") {
  const json good = json::parse(R"({
    "session_id":"80aabbccddeeff00","key":"000102030405060708090a0b0c0d0e0f","title":"4E4D07D1",
    "flags":1071,"state":"lobby","public_slots":16,"private_slots":0,"filled_public":1,
    "filled_private":0,
    "host":{"peer_id":"0000000000001001","vip":"10.64.0.1","xuid":"0009000000000001",
            "gamertag":"Pilot1"},
    "contexts":[[32778,1],[32779,0],"junk"],"properties":[{"id":268435468,"t":1,"v":0}],
    "members":[{"xuid":"0009000000000001","private":false},{"xuid":7}]})");
  codec::SessionDoc doc;
  REQUIRE(codec::ParseSessionDoc(good, &doc));
  CHECK(KidU64(doc.session_id) == 0x80aabbccddeeff00ull);
  CHECK(doc.has_key && doc.key.b[15] == 0x0f);
  CHECK(doc.flags == 1071 && doc.public_slots == 16 && doc.filled_public == 1);
  CHECK(doc.host_peer_id == 0x1001 && doc.host_vip == 0x0A400001 &&
        doc.host_xuid == 0x0009000000000001ull);
  CHECK(doc.contexts.size() == 2 && doc.properties.size() == 1 && doc.members.size() == 1);

  for (const char* field : {"session_id", "host"}) {
    json bad = good;
    bad.erase(field);
    CHECK(!codec::ParseSessionDoc(bad, &doc));
  }
  json bad = good;
  bad["host"]["vip"] = "10.64.0";
  CHECK(!codec::ParseSessionDoc(bad, &doc));
  bad = good;
  bad["session_id"] = 5;
  CHECK(!codec::ParseSessionDoc(bad, &doc));

  // Random corruption of the serialized document never crashes the parser.
  const std::string text = good.dump();
  std::mt19937 rng(11);
  for (int i = 0; i < 3000; ++i) {
    std::string m = text;
    const int edits = 1 + int(rng() % 4);
    for (int e = 0; e < edits; ++e) {
      const size_t pos = rng() % m.size();
      switch (rng() % 3) {
        case 0:
          m[pos] = char(rng());
          break;
        case 1:
          m.erase(pos, 1 + rng() % 8);
          break;
        default:
          m.insert(pos, 1, "{}[]\",:0e-"[rng() % 10]);
          break;
      }
      if (m.empty()) m = "{";
    }
    const json j = codec::Parse(m);
    if (!j.is_discarded()) codec::ParseSessionDoc(j, &doc);
  }
}

// --- Users ---

TEST_CASE("xlive: user identity, contexts and properties") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->SetOfflineXuid(0xE000000000000001ull);
  CHECK(a.x->OfflineXuid() == 0xE000000000000001ull);
  CHECK(a.x->SignedInToLive());
  CHECK(a.x->OnlineXuid() == a.xuid());
  CHECK(a.x->Gamertag() == "Pilot1");
  a.backend.SetState(NetState::kConnecting);
  CHECK(a.x->SignedInToLive());  // a cached identity counts while connecting
  a.backend.SetState(NetState::kOffline);
  CHECK(!a.x->SignedInToLive());
  a.backend.SetState(NetState::kOnline);
  a.backend.SetIdentityValid(false);
  CHECK(!a.x->SignedInToLive());
  CHECK(a.x->OnlineXuid() == 0);
  CHECK(a.x->Gamertag().empty());
  a.backend.SetIdentityValid(true);

  // AC6's contexts and properties (C7, C8, section 3 item 8).
  for (uint32_t id : {kContextGameType, kContextGameMode, kContextPresence, 8u, 9u})
    a.x->UserSetContext(0, id, id & 0xF);
  a.x->UserSetProperty(0, Int32(0x1000000C, 3));
  a.x->UserSetProperty(0, Int32(0x1000000D, 4));
  a.x->UserSetContext(0, kContextGameMode, 6);
  uint32_t v = 0;
  CHECK(a.x->UserGetContext(0, kContextGameMode, &v) && v == 6);
  CHECK(a.x->UserGetContext(0, 9, &v) && v == 9);
  CHECK(!a.x->UserGetContext(0, 0x1234, &v));
  Property p;
  CHECK(a.x->UserGetProperty(0, 0x1000000D, &p) && p == Int32(0x1000000D, 4));
  CHECK(!a.x->UserGetProperty(0, 0x1000000E, &p));
  // Only user index 0 exists.
  a.x->UserSetContext(1, kContextGameMode, 1);
  CHECK(!a.x->UserGetContext(1, kContextGameMode, &v));
  CHECK(a.backend.CallCount() == 0);  // nothing hosted: nothing pushed
}

// --- Sessions ---

TEST_CASE("xlive: session flags are validated like the XDK") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->SessionHandleCreated(1);
  using namespace sflag;
  const uint32_t bad[] = {
      kPlayerHost | 0x40,
      kPlayerHost | kSocialMatchmakingAllowed,
      kPlayerHost | 0x2000,
      kPlayerHost | 0x10000,
      kPlayerHost | 0x80000000u,
      kHost | kUsesStats | kUsesMatchmaking,        // matchmaking without peer network
      kHost | kUsesArbitration | kUsesPeerNetwork,  // arbitration without stats
      kHost,
      kHost | kUsesPresence,          // host without stats/mm/peer
      kUsesMatchmaking | kUsesStats,  // join flavour, same rule
  };
  for (uint32_t flags : bad) {
    SessionInfo info;
    uint64_t nonce = 0;
    CHECK(a.x->SessionCreate(1, flags, 4, 0, 0, &info, &nonce) == hr::kSessionInvalidFlags);
  }
  CHECK(a.backend.CallCount() == 0);
  SessionState st;
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(!st.created);

  // Valid combinations: player and ranked hosts, the 0x1000 bit, stats-only and no flags.
  SessionHandle h = 10;
  const uint32_t single_player_with_stats = 0x706;  // Xenia: a local host session
  for (uint32_t flags : {kPlayerHost, kRankedHost, kPlayerHost | 0x1000u, kUsesStats, 0u,
                         kHost | kUsesPeerNetwork, single_player_with_stats}) {
    a.x->SessionHandleCreated(h);
    SessionInfo info;
    uint64_t nonce = 0;
    CHECK(a.x->SessionCreate(h, flags, 4, 0, 0, &info, &nonce) == hr::kOk);
    ++h;
  }
  CHECK(lobby.Count("session.create") == 4);  // stats-only, flag-less and 0x706 stay local

  // Bad arguments.
  SessionInfo info;
  uint64_t nonce = 0;
  a.x->SessionHandleCreated(50);
  CHECK(a.x->SessionCreate(50, kPlayerHost, 4, 0, 1, &info, &nonce) == hr::kInvalidArg);
  CHECK(a.x->SessionCreate(50, kPlayerHost, 4, 0, 0, nullptr, &nonce) == hr::kInvalidArg);
  CHECK(a.x->SessionCreate(50, kPlayerHost, 4, 0, 0, &info, nullptr) == hr::kInvalidArg);
  CHECK(a.x->SessionCreate(99, kPlayerHost, 4, 0, 0, &info, &nonce) == hr::kInvalidArg);
  // A created session cannot be created again.
  CHECK(a.x->SessionCreate(10, kPlayerHost, 4, 0, 0, &info, &nonce) == hr::kSessionWrongState);
}

TEST_CASE("xlive: ranked lifecycle: create, search, join, arbitrate, start, stats, end, delete") {
  xlive_test::ClearTrace();
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);

  // AC6 sets these before XSessionCreate (C7): ranked = game type 0.
  a.x->UserSetContext(0, kContextGameType, 0);
  a.x->UserSetContext(0, kContextGameMode, 2);
  a.x->UserSetProperty(0, Int32(0x1000000C, 5));
  a.x->UserSetProperty(0, Int32(0x1000000D, 7));

  a.x->SessionHandleCreated(0x100);
  SessionInfo info;
  uint64_t nonce = 0;
  REQUIRE(a.x->SessionCreate(0x100, kRankedHost, 16, 0, 0, &info, &nonce) == hr::kOk);
  CHECK(info.session_id.b[0] == 0x80);
  CHECK(info.host == a.backend.LocalXnAddr());
  CHECK(nonce != 0);
  CHECK(!(info.key == XnKey{}));
  const uint64_t sid = KidU64(info.session_id);
  auto doc = lobby.SessionDoc(sid);
  REQUIRE(doc.has_value());
  CHECK((*doc)["flags"] == kRankedHost);
  CHECK((*doc)["contexts"] == json::parse("[[32778,0],[32779,2]]"));
  CHECK((*doc)["properties"].size() == 2);
  const auto create_args = lobby.Calls("session.create").at(0).args;
  CHECK(create_args["title"] == "4E4D07D1");
  CHECK(create_args["compat"] == a.backend.config().compat_key);
  CHECK(create_args["public_slots"] == 16);
  SessionState st;
  REQUIRE(a.x->GetSessionState(0x100, &st));
  CHECK(st.created && st.is_host && st.phase == SessionPhase::kLobby && st.nonce == nonce);
  CHECK(st.info.session_id == info.session_id && st.host_vip == a.backend.me().vip);
  REQUIRE(a.x->SessionJoinLocal(0x100, {0}, {false}) == hr::kOk);
  CHECK(lobby.Calls("session.join").at(0).args["members"] ==
        json::parse(R"([{"xuid":"0009000000000001","private":false}])"));

  // B searches the way AC6 does (C8): procedure 1, 50 results, 1 user.
  std::vector<SearchResult> results;
  REQUIRE(b.x->SessionSearch(1, 0, 50, 1, {Int32(0x1000000D, 7)},
                             {{kContextGameType, 0}, {kContextGameMode, 2}}, &results) == hr::kOk);
  REQUIRE(results.size() == 1);
  const SearchResult r = results[0];
  CHECK(r.info.session_id == info.session_id);
  CHECK(r.info.key == info.key);
  CHECK(r.info.host == a.backend.LocalXnAddr());
  CHECK(r.filled_public == 1 && r.open_public == 15 && r.open_private == 0 &&
        r.filled_private == 0);
  // Results carry the XLast query's return attributes: contexts 0x800A, 0x800B, no properties.
  REQUIRE(r.contexts.size() == 2);
  CHECK(r.contexts[0].id == kContextGameType && r.contexts[0].value == 0);
  CHECK(r.contexts[1].id == kContextGameMode && r.contexts[1].value == 2);
  CHECK(r.properties.empty());
  const auto search_args = lobby.Calls("session.search").at(0).args;
  CHECK(search_args["proc"] == 1 && search_args["max_results"] == 50 &&
        search_args["num_users"] == 1);
  CHECK(search_args["contexts"] == json::parse("[[32778,0],[32779,2]]"));
  CHECK(search_args["properties"] == json::parse(R"([{"id":268435469,"t":1,"v":7}])"));
  std::vector<SearchResult> none;
  CHECK(b.x->SessionSearch(1, 0, 50, 1, {}, {{kContextGameMode, 3}}, &none) == hr::kOk);
  CHECK(none.empty());
  CHECK(a.x->SessionSearch(1, 0, 50, 1, {}, {}, &none) == hr::kOk);
  CHECK(none.empty());  // the host never finds its own session

  // B joins with the search result's XSESSION_INFO (HOST clear).
  b.x->SessionHandleCreated(0x200);
  SessionInfo binfo = r.info;
  uint64_t bnonce = 0;
  REQUIRE(b.x->SessionCreate(0x200, kRankedJoin, 16, 0, 0, &binfo, &bnonce) == hr::kOk);
  CHECK(binfo.session_id == info.session_id && binfo.host == info.host && binfo.key == info.key);
  CHECK(bnonce == nonce);  // the host's nonce, from session.get
  const auto reg = b.backend.Registered();
  REQUIRE(reg.size() == 1);
  CHECK(reg[0].first == a.backend.me().vip && reg[0].second == a.backend.me().peer_id);
  CHECK(lobby.Count("session.get") == 1);
  REQUIRE(b.x->GetSessionState(0x200, &st));
  CHECK(st.created && !st.is_host && st.host_vip == a.backend.me().vip &&
        st.phase == SessionPhase::kLobby);
  REQUIRE(b.x->SessionJoinLocal(0x200, {0}, {false}) == hr::kOk);
  // Each side adds the other by XUID; only the host's call reaches the server.
  const size_t joins = lobby.Count("session.join");
  REQUIRE(a.x->SessionJoinRemote(0x100, {b.xuid()}, {false}) == hr::kOk);
  REQUIRE(b.x->SessionJoinRemote(0x200, {a.xuid()}, {false}) == hr::kOk);
  CHECK(lobby.Count("session.join") == joins + 1);
  CHECK((*lobby.SessionDoc(sid))["filled_public"] == 2);
  REQUIRE(b.x->GetSessionState(0x200, &st));
  REQUIRE(st.members.size() == 2);
  CHECK(st.members[0].xuid == b.xuid() && st.members[0].local);
  CHECK(st.members[1].xuid == a.xuid() && !st.members[1].local);
  // Joining twice is a no-op.
  REQUIRE(a.x->SessionJoinRemote(0x100, {b.xuid()}, {false}) == hr::kOk);
  CHECK(lobby.Count("session.join") == joins + 1);

  // Arbitrated sessions cannot start before registration.
  CHECK(a.x->SessionStart(0x100, 0) == hr::kSessionNotRegistered);
  std::vector<Registrant> regs;
  REQUIRE(a.x->SessionArbitrationRegister(0x100, 0, nonce, 300, &regs) == hr::kOk);
  REQUIRE(regs.size() == 2);
  CHECK(regs[0].machine_id == MachineIdFromXnAddr(a.backend.LocalXnAddr()));
  CHECK(regs[0].users == std::vector<Xuid>{a.xuid()} && regs[0].trustworthiness == 1);
  CHECK(regs[1].machine_id == MachineIdFromXnAddr(b.backend.LocalXnAddr()));
  CHECK(regs[1].users == std::vector<Xuid>{b.xuid()});
  CHECK(lobby.Calls("session.arbitrate").at(0).args["nonce"] == codec::HexU64(nonce));
  REQUIRE(b.x->SessionArbitrationRegister(0x200, 0, bnonce, 300, &regs) == hr::kOk);
  CHECK(regs.size() == 2);
  CHECK(b.x->SessionArbitrationRegister(0x200, 0, nonce ^ 1, 300, &regs) == hr::kInvalidArg);
  REQUIRE(a.x->GetSessionState(0x100, &st));
  CHECK(st.phase == SessionPhase::kRegistration);

  // Start: the host tells the server, the joiner only changes state.
  REQUIRE(a.x->SessionStart(0x100, 0) == hr::kOk);
  REQUIRE(b.x->SessionStart(0x200, 0) == hr::kOk);
  CHECK(lobby.Count("session.start") == 1);
  CHECK((*lobby.SessionDoc(sid))["state"] == "ingame");
  REQUIRE(b.x->GetSessionState(0x200, &st));
  CHECK(st.phase == SessionPhase::kInGame);
  // A started session with join-in-progress disabled is no longer listed; joins are refused.
  Console c(&lobby, 3);
  CHECK(c.x->SessionSearch(1, 0, 50, 1, {}, {}, &none) == hr::kOk);
  CHECK(none.empty());
  CHECK(a.x->SessionJoinRemote(0x100, {c.xuid()}, {false}) == hr::kSessionFull);

  // Stats: the last write per column wins; arbitrated non-hosts do not report.
  REQUIRE(a.x->SessionWriteStats(0x100, a.xuid(),
                                 {{1, {Int32(2, 10), Int64(3, int64_t{1} << 40)}}}) == hr::kOk);
  REQUIRE(a.x->SessionWriteStats(0x100, a.xuid(), {{1, {Int32(2, 12)}}}) == hr::kOk);
  REQUIRE(a.x->SessionWriteStats(0x100, b.xuid(), {{1, {Int32(2, 4)}}}) == hr::kOk);
  REQUIRE(b.x->SessionWriteStats(0x200, b.xuid(), {{1, {Int32(2, 99)}}}) == hr::kOk);
  CHECK(lobby.Count("stats.write") == 0);  // cached until XSessionEnd
  REQUIRE(a.x->SessionEnd(0x100) == hr::kOk);
  REQUIRE(b.x->SessionEnd(0x200) == hr::kOk);
  CHECK(lobby.Count("stats.write") == 2);  // one per XUID, from the host only
  CHECK(lobby.StatsValue(1, a.xuid(), 2) == 12);
  CHECK(lobby.StatsValue(1, a.xuid(), 3) == (int64_t{1} << 40));
  CHECK(lobby.StatsValue(1, b.xuid(), 2) == 4);
  CHECK(lobby.Count("session.end") == 1);
  CHECK((*lobby.SessionDoc(sid))["state"] == "lobby");
  REQUIRE(a.x->GetSessionState(0x100, &st));
  CHECK(st.phase == SessionPhase::kLobby);
  CHECK(a.x->SessionEnd(0x100) == hr::kSessionWrongState);  // not in game any more

  // Reads return one row per requested player, even for players without stats.
  std::vector<StatsView> views;
  const Xuid stranger = 0x0009000000000077ull;
  REQUIRE(b.x->StatsRead({a.xuid(), b.xuid(), stranger, a.xuid()}, {{1, {2, 3, 4}}}, &views) ==
          hr::kOk);
  REQUIRE(views.size() == 1);
  CHECK(views[0].view_id == 1 && views[0].total_rows == 2);
  REQUIRE(views[0].rows.size() == 3);
  const StatsRow& ra = views[0].rows[0];
  CHECK(ra.xuid == a.xuid() && ra.rank == 1 && ra.gamertag == "Pilot1");
  REQUIRE(ra.columns.size() == 3);
  CHECK(ra.columns[0].column_id == 2 && ra.columns[0].value.type == DataType::kInt32 &&
        ra.columns[0].value.i == 12);
  CHECK(ra.columns[1].value.type == DataType::kInt64 &&
        ra.columns[1].value.i == (int64_t{1} << 40));
  CHECK(ra.columns[2].column_id == 4 && ra.columns[2].value.type == DataType::kNull);
  const StatsRow& rs = views[0].rows[2];
  CHECK(rs.xuid == stranger && rs.rank == 0 && rs.rating == 0 && rs.columns.size() == 3);
  for (const auto& col : rs.columns) CHECK(col.value.type == DataType::kNull && col.value.i == 0);

  // Delete: the joiner leaves with its own XUID, the host deletes.
  REQUIRE(b.x->SessionDelete(0x200) == hr::kOk);
  REQUIRE(lobby.Count("session.leave") == 1);
  CHECK(lobby.Calls("session.leave").at(0).args["xuids"] == json::array({codec::HexU64(b.xuid())}));
  REQUIRE(a.x->SessionDelete(0x100) == hr::kOk);
  CHECK(lobby.Count("session.delete") == 1);
  CHECK(lobby.SessionCount() == 0);
  REQUIRE(a.x->GetSessionState(0x100, &st));
  CHECK(st.phase == SessionPhase::kDeleted && st.members.empty());
  CHECK(a.x->SessionDelete(0x100) == hr::kOk);  // idempotent, no second RPC
  CHECK(lobby.Count("session.delete") == 1);
  CHECK(a.x->SessionStart(0x100, 0) == hr::kSessionWrongState);

#if AC6NET_XLIVE_STANDALONE
  const auto trace = xlive_test::TraceLines();
  CHECK(TraceHas(trace, "xlive.session.create", "\"hr\":\"0x00000000\""));
  CHECK(TraceHas(trace, "xlive.session.search", "\"count\":1"));
  CHECK(TraceHas(trace, "xlive.session.search", "\"count\":0"));
  CHECK(TraceHas(trace, "xlive.session.join"));
  CHECK(TraceHas(trace, "xlive.session.arbitrate", "\"registrants\":2"));
  CHECK(TraceHas(trace, "xlive.session.start"));
  CHECK(TraceHas(trace, "xlive.stats.write"));
  CHECK(TraceHas(trace, "xlive.stats.flush", "\"written\":2"));
  CHECK(TraceHas(trace, "xlive.session.end"));
  CHECK(TraceHas(trace, "xlive.stats.read"));
  CHECK(TraceHas(trace, "xlive.session.delete"));
#endif
}

TEST_CASE("xlive: search results return only the query's attributes, in order") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  // Presence and title contexts are set too (section 3 item 8); the map orders them first.
  a.x->UserSetContext(0, kContextPresence, 2);
  a.x->UserSetContext(0, 8, 3);
  a.x->UserSetContext(0, 9, 4);
  a.x->UserSetContext(0, kContextGameMode, 5);
  a.x->UserSetContext(0, kContextGameType, 1);
  a.x->UserSetProperty(0, Int32(0x1000000D, 0));
  Host(a, 1, kPlayerHost, 16);
  std::vector<SearchResult> out;
  REQUIRE(b.x->SessionSearch(1, 0, 50, 1, {Int32(0x1000000D, 0)},
                             {{kContextGameType, 1}, {kContextGameMode, 5}}, &out) == hr::kOk);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].contexts.size() == 2);
  CHECK(out[0].contexts[0] == (Context{kContextGameType, 1}));
  CHECK(out[0].contexts[1] == (Context{kContextGameMode, 5}));
  CHECK(out[0].properties.empty());
  REQUIRE(b.x->SessionSearch(0, 0, 50, 1, {}, {}, &out) == hr::kOk);
  REQUIRE(out.size() == 1);
  CHECK(out[0].contexts.size() == 2);
}

TEST_CASE("xlive: player match: modify, slots and leave") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  a.x->UserSetContext(0, kContextGameType, 1);
  const SessionInfo info = Host(a, 1, kPlayerHost, 8);
  const uint64_t sid = KidU64(info.session_id);

  // Only modifiable flags may change; the host's modify reaches the server.
  CHECK(a.x->SessionModify(1, kPlayerHost | sflag::kInvitesDisabled, 6, 2) == hr::kOk);
  auto doc = lobby.SessionDoc(sid);
  CHECK((*doc)["flags"] == (kPlayerHost | sflag::kInvitesDisabled));
  CHECK((*doc)["public_slots"] == 6 && (*doc)["private_slots"] == 2);
  CHECK(a.x->SessionModify(1, kPlayerHost & ~sflag::kUsesMatchmaking, 6, 2) ==
        hr::kSessionInvalidFlags);
  CHECK(a.x->SessionModify(1, kPlayerHost & ~sflag::kHost, 6, 2) == hr::kSessionInvalidFlags);
  CHECK(lobby.Count("session.modify") == 1);
  SessionState st;
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(st.flags == (kPlayerHost | sflag::kInvitesDisabled) && st.public_slots == 6 &&
        st.private_slots == 2);

  // A joiner's modify and start stay local.
  b.x->SessionHandleCreated(2);
  SessionInfo binfo = info;
  uint64_t bnonce = 0;
  REQUIRE(b.x->SessionCreate(2, kPlayerJoin, 6, 2, 0, &binfo, &bnonce) == hr::kOk);
  REQUIRE(b.x->SessionJoinLocal(2, {0}, {true}) == hr::kOk);
  CHECK((*lobby.SessionDoc(sid))["filled_private"] == 1);
  CHECK(b.x->SessionModify(2, kPlayerJoin | sflag::kInvitesDisabled, 6, 2) == hr::kOk);
  CHECK(lobby.Count("session.modify") == 1);

  // Player matches start without arbitration.
  CHECK(a.x->SessionStart(1, 0) == hr::kOk);
  CHECK(b.x->SessionStart(2, 0) == hr::kOk);
  CHECK(lobby.Count("session.start") == 1);
  // Stats in a non-arbitrated session are reported by everyone.
  CHECK(b.x->SessionWriteStats(2, b.xuid(), {{3, {Int32(1, 5)}}}) == hr::kOk);
  CHECK(b.x->SessionEnd(2) == hr::kOk);
  CHECK(lobby.StatsValue(3, b.xuid(), 1) == 5);
  CHECK(a.x->SessionEnd(1) == hr::kOk);

  // Leaving a non-arbitrated session removes the member; a non-host only reports itself.
  REQUIRE(a.x->SessionJoinRemote(1, {b.xuid()}, {true}) == hr::kOk);
  REQUIRE(b.x->SessionJoinRemote(2, {a.xuid()}, {false}) == hr::kOk);
  const size_t leaves = lobby.Count("session.leave");
  REQUIRE(b.x->SessionLeaveRemote(2, {a.xuid()}) == hr::kOk);  // local bookkeeping only
  CHECK(lobby.Count("session.leave") == leaves);
  REQUIRE(b.x->SessionLeaveLocal(2, {0}) == hr::kOk);
  CHECK(lobby.Count("session.leave") == leaves + 1);
  REQUIRE(b.x->GetSessionState(2, &st));
  CHECK(st.members.empty());
  CHECK((*lobby.SessionDoc(sid))["filled_private"] == 0);
  // The host removes remote players on the server.
  REQUIRE(a.x->SessionJoinRemote(1, {0x0009000000000055ull}, {false}) == hr::kOk);
  REQUIRE(a.x->SessionLeaveRemote(1, {0x0009000000000055ull}) == hr::kOk);
  CHECK(lobby.Count("session.leave") == leaves + 2);
  CHECK((*lobby.SessionDoc(sid))["members"].size() == 1);
  CHECK(a.x->SessionLeaveLocal(1, {1}) == hr::kInvalidArg);
  CHECK(a.x->SessionJoinLocal(1, {}, {}) == hr::kInvalidArg);
}

TEST_CASE("xlive: arbitrated members that leave stay as zombies") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->UserSetContext(0, kContextGameType, 0);  // ranked
  Host(a, 1, kRankedHost, 4);
  REQUIRE(a.x->SessionJoinRemote(1, {0x0009000000000042ull}, {false}) == hr::kOk);
  REQUIRE(a.x->SessionLeaveRemote(1, {0x0009000000000042ull}) == hr::kOk);
  SessionState st;
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(st.members.size() == 2);  // kept for stats reporting (Xenia)
  CHECK(lobby.Count("session.leave") == 0);
  // Arbitration cannot be removed from a ranked session.
  REQUIRE(a.x->SessionModify(1, kRankedHost & ~sflag::kUsesArbitration, 4, 0) == hr::kOk);
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(st.flags & sflag::kUsesArbitration);

  // A standard session may drop arbitration before registration, but not after.
  a.x->UserSetContext(0, kContextGameType, 1);
  uint64_t nonce = 0;
  Host(a, 2, kRankedHost, 4, &nonce);
  REQUIRE(a.x->SessionModify(2, kRankedHost & ~sflag::kUsesArbitration, 4, 0) == hr::kOk);
  REQUIRE(a.x->GetSessionState(2, &st));
  CHECK(!(st.flags & sflag::kUsesArbitration));
  REQUIRE(a.x->SessionModify(2, kRankedHost, 4, 0) == hr::kOk);
  std::vector<Registrant> regs;
  REQUIRE(a.x->SessionArbitrationRegister(2, 0, nonce, 300, &regs) == hr::kOk);
  REQUIRE(a.x->SessionModify(2, kRankedHost & ~sflag::kUsesArbitration, 4, 0) == hr::kOk);
  REQUIRE(a.x->GetSessionState(2, &st));
  CHECK(st.flags & sflag::kUsesArbitration);
  CHECK((*lobby.SessionDoc([&] {
          uint64_t v = 0;
          for (uint8_t b : st.info.session_id.b) v = (v << 8) | b;
          return v;
        }()))["flags"] == kRankedHost);
}

TEST_CASE("xlive: offline: Live calls fail fast, local sessions still work") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->SetOfflineXuid(0xE000000000000001ull);
  a.backend.SetState(NetState::kOffline);
  CHECK(!a.x->SignedInToLive());

  a.x->SessionHandleCreated(1);
  SessionInfo info;
  uint64_t nonce = 0;
  CHECK(a.x->SessionCreate(1, kPlayerHost, 4, 0, 0, &info, &nonce) == hr::kSessionNotLoggedOn);
  info.session_id.b = {0x80, 1, 2, 3, 4, 5, 6, 7};
  CHECK(a.x->SessionCreate(1, kPlayerJoin, 4, 0, 0, &info, &nonce) == hr::kSessionNotLoggedOn);
  std::vector<SearchResult> results;
  CHECK(a.x->SessionSearch(1, 0, 50, 1, {}, {}, &results) == hr::kSessionNotLoggedOn);
  CHECK(a.backend.CallCount() == 0);

  // A session without Live features works offline.
  REQUIRE(a.x->SessionCreate(1, 0, 4, 0, 0, &info, &nonce) == hr::kOk);
  CHECK(info.session_id.b[0] == 0x00);
  CHECK(info.host == a.backend.LocalXnAddr());
  REQUIRE(a.x->SessionJoinLocal(1, {0}, {false}) == hr::kOk);
  REQUIRE(a.x->SessionStart(1, 0) == hr::kOk);
  REQUIRE(a.x->SessionEnd(1) == hr::kOk);
  std::vector<Registrant> regs;
  REQUIRE(a.x->SessionArbitrationRegister(1, 0, nonce, 300, &regs) == hr::kOk);
  REQUIRE(regs.size() == 1);
  CHECK(regs[0].machine_id == MachineIdFromXnAddr(a.backend.LocalXnAddr()));
  REQUIRE(a.x->SessionDelete(1) == hr::kOk);

  // So does a stats-only session; its flush waits for a connection.
  a.x->SessionHandleCreated(2);
  REQUIRE(a.x->SessionCreate(2, sflag::kUsesStats, 0, 0, 0, &info, &nonce) == hr::kOk);
  REQUIRE(a.x->SessionStart(2, 0) == hr::kOk);
  REQUIRE(a.x->SessionWriteStats(2, a.xuid(), {{1, {Int32(2, 1)}}}) == hr::kOk);
  CHECK(a.x->SessionFlushStats(2) == hr::kSessionNotLoggedOn);
  REQUIRE(a.x->SessionEnd(2) == hr::kOk);
  CHECK(a.backend.CallCount() == 0);

  // Without any identity the offline XUID is used for local members.
  a.backend.SetIdentityValid(false);
  a.x->SessionHandleCreated(3);
  REQUIRE(a.x->SessionCreate(3, 0, 4, 0, 0, &info, &nonce) == hr::kOk);
  REQUIRE(a.x->SessionJoinLocal(3, {0}, {false}) == hr::kOk);
  SessionState st;
  REQUIRE(a.x->GetSessionState(3, &st));
  REQUIRE(st.members.size() == 1);
  CHECK(st.members[0].xuid == 0xE000000000000001ull);

  // Stats reads still produce rows (and report that the user is not signed in).
  std::vector<StatsView> views;
  CHECK(a.x->StatsRead({a.xuid()}, {{1, {2}}}, &views) == hr::kNotLoggedOn);
  REQUIRE(views.size() == 1 && views[0].rows.size() == 1);
  CHECK(views[0].rows[0].columns.size() == 1);
  CHECK(a.backend.CallCount() == 0);
}

TEST_CASE("xlive: a stats-only session writes with the zero session id") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->SessionHandleCreated(1);
  SessionInfo info;
  uint64_t nonce = 0;
  REQUIRE(a.x->SessionCreate(1, sflag::kUsesStats, 0, 0, 0, &info, &nonce) == hr::kOk);
  CHECK(lobby.Count("session.create") == 0);
  REQUIRE(a.x->SessionStart(1, 0) == hr::kOk);
  REQUIRE(a.x->SessionWriteStats(1, a.xuid(), {{5, {Int64(0x20000006, 77)}}}) == hr::kOk);
  REQUIRE(a.x->SessionEnd(1) == hr::kOk);
  const auto writes = lobby.Calls("stats.write");
  REQUIRE(writes.size() == 1);
  CHECK(writes[0].args["session_id"] == "0000000000000000");
  CHECK(writes[0].args["views"] ==
        json::parse(R"([{"view_id":5,"props":[{"id":536870918,"t":2,"v":77}]}])"));
  CHECK(lobby.StatsValue(5, a.xuid(), 0x20000006) == 77);
}

TEST_CASE("xlive: a lost connection marks sessions stale") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  const SessionInfo info = Host(a, 1, kPlayerHost, 4);
  a.x->OnConnectionLost();
  const size_t calls = a.backend.CallCount();
  CHECK(a.x->SessionStart(1, 0) == hr::kSessionNotLoggedOn);
  CHECK(a.x->SessionJoinRemote(1, {0x0009000000000009ull}, {false}) == hr::kSessionNotLoggedOn);
  CHECK(a.x->SessionModify(1, kPlayerHost, 4, 0) == hr::kSessionNotLoggedOn);
  a.x->UserSetContext(0, kContextGameMode, 9);
  std::this_thread::sleep_for(700ms);
  CHECK(lobby.Count("session.attrs") == 0);  // stale sessions are not pushed
  CHECK(a.x->SessionDelete(1) == hr::kOk);
  CHECK(a.backend.CallCount() == calls);
  // After reconnecting, new sessions work again.
  Host(a, 2, kPlayerHost, 4);
  CHECK(lobby.Count("session.create") == 2);
  (void)info;
}

TEST_CASE("xlive: server errors map to HRESULTs") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  a.x->SessionHandleCreated(1);
  SessionInfo info;
  uint64_t nonce = 0;
  lobby.DropNext("session.create");
  CHECK(a.x->SessionCreate(1, kPlayerHost, 1, 0, 0, &info, &nonce) == hr::kFail);
  lobby.Override("session.create", [] { return RawOk(R"({"session_id":"zz","key":1})"); });
  CHECK(a.x->SessionCreate(1, kPlayerHost, 1, 0, 0, &info, &nonce) == hr::kFail);
  lobby.Override("session.create", [] { return RawOk("not json"); });
  CHECK(a.x->SessionCreate(1, kPlayerHost, 1, 0, 0, &info, &nonce) == hr::kFail);
  lobby.Override("session.create", nullptr);
  lobby.FailNext("session.create", "compat");
  CHECK(a.x->SessionCreate(1, kPlayerHost, 1, 0, 0, &info, &nonce) == hr::kFail);
  SessionState st;
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(!st.created);
  REQUIRE(a.x->SessionCreate(1, kPlayerHost, 1, 0, 0, &info, &nonce) == hr::kOk);
  REQUIRE(a.x->SessionJoinLocal(1, {0}, {false}) == hr::kOk);

  // Joining a session that does not exist.
  b.x->SessionHandleCreated(2);
  SessionInfo bogus = info;
  bogus.session_id.b[7] ^= 1;
  uint64_t bnonce = 0;
  CHECK(b.x->SessionCreate(2, kPlayerJoin, 1, 0, 0, &bogus, &bnonce) == hr::kSessionNotFound);
  // A session.get answer for a different session is rejected.
  const std::string real_doc = lobby.SessionDoc(KidU64(info.session_id))->dump();
  lobby.Override("session.get", [&] { return RawOk(real_doc); });
  CHECK(b.x->SessionCreate(2, kPlayerJoin, 1, 0, 0, &bogus, &bnonce) == hr::kFail);
  lobby.Override("session.get", nullptr);

  // The session is full: the joiner's own join fails and nothing is recorded locally.
  SessionInfo binfo = info;
  binfo.host = XnAddr{};  // a garbage host address is normalized from the server's document
  REQUIRE(b.x->SessionCreate(2, kPlayerJoin, 1, 0, 0, &binfo, &bnonce) == hr::kOk);
  CHECK(binfo.host == a.backend.LocalXnAddr());
  CHECK(b.x->SessionJoinLocal(2, {0}, {false}) == hr::kSessionFull);
  REQUIRE(b.x->GetSessionState(2, &st));
  CHECK(st.members.empty());

  // A lost session.end is resent before the next start, so the server leaves "ingame".
  REQUIRE(a.x->SessionStart(1, 0) == hr::kOk);
  lobby.DropNext("session.end");
  REQUIRE(a.x->SessionEnd(1) == hr::kOk);
  CHECK((*lobby.SessionDoc(KidU64(info.session_id)))["state"] == "ingame");
  const size_t ends = lobby.Count("session.end");
  REQUIRE(a.x->SessionStart(1, 0) == hr::kOk);
  CHECK(lobby.Count("session.end") == ends + 1);
  REQUIRE(a.x->SessionEnd(1) == hr::kOk);
  REQUIRE(a.x->SessionStart(1, 0) == hr::kOk);
  CHECK(lobby.Count("session.end") == ends + 2);  // no extra resend once delivered
  REQUIRE(a.x->SessionEnd(1) == hr::kOk);

  lobby.FailNext("session.start", "state");
  CHECK(a.x->SessionStart(1, 0) == hr::kSessionWrongState);
  REQUIRE(a.x->GetSessionState(1, &st));
  CHECK(st.phase == SessionPhase::kLobby);
  lobby.FailNext("session.start", "rate");
  CHECK(a.x->SessionStart(1, 0) == hr::kFail);
  lobby.FailNext("session.modify", "bad_request");
  CHECK(a.x->SessionModify(1, kPlayerHost, 2, 0) == hr::kInvalidArg);
  lobby.FailNext("session.search", "internal");
  std::vector<SearchResult> results;
  CHECK(b.x->SessionSearch(1, 0, 50, 1, {}, {}, &results) == hr::kFail);
  lobby.Override("session.search", [] { return RawOk(R"({"results":7})"); });
  CHECK(b.x->SessionSearch(1, 0, 50, 1, {}, {}, &results) == hr::kFail);
  lobby.Override("session.search", nullptr);
  CHECK(b.x->SessionSearch(1, 1, 50, 1, {}, {}, &results) == hr::kInvalidArg);
  CHECK(b.x->SessionSearch(1, 0, 50, 1, {}, {}, nullptr) == hr::kInvalidArg);

  // Calls on unknown handles.
  CHECK(a.x->SessionStart(77, 0) == hr::kInvalidArg);
  CHECK(a.x->SessionDelete(77) == hr::kInvalidArg);
  CHECK(a.x->SessionJoinRemote(77, {1}, {false}) == hr::kInvalidArg);
}

TEST_CASE("xlive: search results are validated and filtered client-side") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  const SessionInfo info = Host(a, 1, kPlayerHost, 4);
  json own_doc = *lobby.SessionDoc(KidU64(info.session_id));
  json foreign = own_doc;
  foreign["session_id"] = "80000000000000aa";
  foreign["host"]["peer_id"] = "0000000000002002";
  foreign["host"]["vip"] = "10.64.0.99";
  json bad_vip = foreign;
  bad_vip["host"]["vip"] = "192.168.1.5";  // outside 10.64/10
  json no_key = foreign;
  no_key.erase("key");
  json results = json::array({own_doc, foreign, bad_vip, no_key, 5, "x", json::object()});
  // a's view: its own session is excluded even if the server returns it.
  lobby.Override("session.search", [&] { return RawOk(json{{"results", results}}.dump()); });
  std::vector<SearchResult> out;
  REQUIRE(a.x->SessionSearch(1, 0, 50, 1, {}, {}, &out) == hr::kOk);
  REQUIRE(out.size() == 1);
  CHECK(KidU64(out[0].info.session_id) == 0x80000000000000aaull);
  CHECK(out[0].info.host == MakeXnAddr(0x0A400063, 0x2002));
  // Unknown procedures return the document's attributes unfiltered.
  REQUIRE(a.x->SessionSearch(7, 0, 50, 1, {}, {}, &out) == hr::kOk);
  REQUIRE(out.size() == 1);
  CHECK(out[0].contexts.size() == own_doc["contexts"].size());
  // max_results is honoured.
  REQUIRE(b.x->SessionSearch(1, 0, 1, 1, {}, {}, &out) == hr::kOk);
  CHECK(out.size() == 1);
  lobby.Override("session.search", nullptr);
}

TEST_CASE("xlive: closing a handle cleans up the server session in the background") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  const SessionInfo info = Host(a, 1, kPlayerHost, 4);
  b.x->SessionHandleCreated(2);
  SessionInfo binfo = info;
  uint64_t nonce = 0;
  REQUIRE(b.x->SessionCreate(2, kPlayerJoin, 4, 0, 0, &binfo, &nonce) == hr::kOk);
  REQUIRE(b.x->SessionJoinLocal(2, {0}, {false}) == hr::kOk);
  b.x->SessionHandleClosed(2);
  SessionState st;
  CHECK(!b.x->GetSessionState(2, &st));
  REQUIRE(WaitFor([&] { return lobby.Count("session.leave") == 1; }, 2000));
  a.x->SessionHandleClosed(1);
  REQUIRE(WaitFor([&] { return lobby.SessionCount() == 0; }, 2000));
  CHECK(lobby.Calls("session.delete").at(0).thread != std::this_thread::get_id());
  // Closing an unknown or never-created handle is harmless.
  a.x->SessionHandleClosed(1234);
  a.x->SessionHandleCreated(5);
  a.x->SessionHandleClosed(5);
}

TEST_CASE("xlive: leaderboard lookups by the offline XUID find the online row") {
  // AC6's online leaderboard centres "your rank" on the XUID it cached at profile sign-in,
  // which can be the offline (profile) XUID; the boards are keyed by online XUIDs.
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  const Xuid offline = 0xE000000000000001ull;
  a.x->SetOfflineXuid(offline);
  json write = json::parse(R"({"session_id":"8000000000000001","title":"4E4D07D1",
      "views":[{"view_id":117,"props":[{"id":3,"t":1,"v":15000}]}]})");
  write["xuid"] = codec::HexU64(a.xuid());
  lobby.Handle(a.backend.me(), "stats.write", write.dump());
  write["xuid"] = codec::HexU64(b.xuid());
  lobby.Handle(b.backend.me(), "stats.write", write.dump());
  const StatsSpec spec{117, {3}};
  StatsView view;
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, offline, 10, &view) == hr::kOk);
  CHECK(lobby.Calls("stats.enumerate").back().args["value"] == codec::HexU64(a.xuid()));
  REQUIRE(view.rows.size() == 2);
  // The player's own row carries the XUID the title asked with, and a real rank.
  int own = 0;
  for (const StatsRow& r : view.rows) {
    CHECK(r.xuid != a.xuid());
    if (r.xuid == offline) {
      ++own;
      CHECK(r.rank >= 1);
    }
  }
  CHECK(own == 1);
  // The online XUID still works and is left alone; other players' XUIDs are never mapped.
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kOk);
  CHECK(lobby.Calls("stats.enumerate").back().args["value"] == codec::HexU64(a.xuid()));
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, b.xuid(), 10, &view) == hr::kOk);
  CHECK(lobby.Calls("stats.enumerate").back().args["value"] == codec::HexU64(b.xuid()));
  // XUserReadStats by the offline XUID: queried by the online XUID, returned under the offline.
  std::vector<StatsView> views;
  REQUIRE(a.x->StatsRead({offline, b.xuid()}, {spec}, &views) == hr::kOk);
  const auto args = lobby.Calls("stats.read").back().args;
  CHECK(args["xuids"] == json::array({codec::HexU64(a.xuid()), codec::HexU64(b.xuid())}));
  REQUIRE(views.size() == 1 && views[0].rows.size() == 2);
  CHECK(views[0].rows[0].xuid == offline && views[0].rows[1].xuid == b.xuid());
  REQUIRE(views[0].rows[0].columns.size() == 1);
  CHECK(views[0].rows[0].columns[0].value.i == 15000);
  // Both of the player's XUIDs in one read map to one board row; each gets a full copy.
  REQUIRE(a.x->StatsRead({offline, a.xuid()}, {spec}, &views) == hr::kOk);
  REQUIRE(views.size() == 1 && views[0].rows.size() == 2);
  CHECK(views[0].rows[0].xuid == offline && views[0].rows[1].xuid == a.xuid());
  for (const StatsRow& r : views[0].rows) {
    REQUIRE(r.columns.size() == 1);
    CHECK(r.columns[0].value.i == 15000);
    CHECK(!r.gamertag.empty());
  }
}

TEST_CASE("xlive: stats enumerate always returns a row for the pivot player") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  const StatsSpec spec{1, {2, 3, 1}};  // what AC6 enumerates (C18)
  StatsView view;
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kOk);
  CHECK(lobby.Count("stats.enumerate") == 1);
  CHECK(view.view_id == 1);
  REQUIRE(view.rows.size() == 1);
  const StatsRow& row = view.rows[0];
  CHECK(row.xuid == a.xuid() && row.rank == 0 && row.rating == 0 && row.gamertag == "Pilot1");
  REQUIRE(row.columns.size() == 3);
  const uint16_t want[] = {2, 3, 1};
  for (size_t i = 0; i < 3; ++i) {
    CHECK(row.columns[i].column_id == want[i]);
    CHECK(row.columns[i].value.type == DataType::kNull && row.columns[i].value.i == 0);
  }
  const auto args = lobby.Calls("stats.enumerate").at(0).args;
  CHECK(args["pivot"] == "xuid" && args["value"] == codec::HexU64(a.xuid()) &&
        args["num_rows"] == 10 && args["columns"] == json::parse("[2,3,1]"));

  // System columns are row fields, never columns; duplicates are dropped.
  REQUIRE(a.x->StatsEnumerate({1, {0xFFFF, 2, 2, 0xFFFD, 0xFFFE}}, StatsPivot::kXuid, a.xuid(), 1,
                              &view) == hr::kOk);
  REQUIRE(view.rows.size() == 1);
  REQUIRE(view.rows[0].columns.size() == 1);
  CHECK(view.rows[0].columns[0].column_id == 2);

  // A rank pivot on an empty board gives an empty view, which is still a result.
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kRank, 1, 10, &view) == hr::kOk);
  CHECK(view.rows.empty() && view.view_id == 1);
  CHECK(lobby.Calls("stats.enumerate").back().args["value"] == 1);

  // With stats on the board the server's rows come back, columns in request order.
  json write = json::parse(R"({"session_id":"8000000000000001","title":"4E4D07D1",
      "views":[{"view_id":1,"props":[{"id":3,"t":1,"v":30}]}]})");
  write["xuid"] = codec::HexU64(b.xuid());
  lobby.Handle(b.backend.me(), "stats.write", write.dump());
  REQUIRE(b.x->StatsEnumerate(spec, StatsPivot::kXuid, b.xuid(), 10, &view) == hr::kOk);
  REQUIRE(view.rows.size() == 1);
  CHECK(view.rows[0].xuid == b.xuid() && view.rows[0].rank == 1 && view.total_rows == 1);
  REQUIRE(view.rows[0].columns.size() == 3);
  CHECK(view.rows[0].columns[0].value.type == DataType::kNull);
  CHECK(view.rows[0].columns[1].value.type == DataType::kInt32 &&
        view.rows[0].columns[1].value.i == 30);
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kRating, uint64_t(-5), 10, &view) == hr::kOk);
  CHECK(lobby.Calls("stats.enumerate").back().args["value"] == -5);
  CHECK(view.rows.size() == 1);

  // A server that echoes full property ids is matched on their low 16 bits.
  lobby.Override("stats.enumerate", [&] {
    json row = {
        {"xuid", codec::HexU64(b.xuid())},
        {"rank", 1},
        {"rating", 9},
        {"columns", json::parse(R"([{"id":536870915,"t":2,"v":9},{"id":1,"t":255,"v":0}])")}};
    return RawOk(json{
        {"views", json::array({{{"view_id", 1}, {"total_rows", 1}, {"rows", json::array({row})}}})}}
                     .dump());
  });
  REQUIRE(b.x->StatsEnumerate(spec, StatsPivot::kXuid, b.xuid(), 10, &view) == hr::kOk);
  lobby.Override("stats.enumerate", nullptr);
  REQUIRE(view.rows.size() == 1 && view.rows[0].columns.size() == 3);
  CHECK(view.rows[0].rating == 9);
  CHECK(view.rows[0].columns[1].column_id == 3 && view.rows[0].columns[1].value.i == 9 &&
        view.rows[0].columns[1].value.type == DataType::kInt64);
  CHECK(view.rows[0].columns[2].value.type == DataType::kNull);

  // The pivot player is not on a non-empty board: still a row for them.
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kOk);
  REQUIRE(view.rows.size() == 1);
  CHECK(view.rows[0].xuid == a.xuid() && view.rows[0].rank == 0);

  // Server trouble still yields the row.
  lobby.DropNext("stats.enumerate");
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kOk);
  CHECK(view.rows.size() == 1);
  lobby.Override("stats.enumerate",
                 [] { return RawOk(R"({"views":[{"view_id":1,"rows":[7,{"xuid":1}]}]})"); });
  REQUIRE(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kOk);
  CHECK(view.rows.size() == 1 && view.rows[0].xuid == a.xuid());
  lobby.Override("stats.enumerate", nullptr);
  a.backend.SetState(NetState::kOffline);
  CHECK(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 10, &view) == hr::kNotLoggedOn);
  CHECK(view.rows.size() == 1 && view.rows[0].columns.size() == 3);
  a.backend.SetState(NetState::kOnline);

  CHECK(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 0, &view) == hr::kInvalidArg);
  CHECK(a.x->StatsEnumerate(spec, StatsPivot::kXuid, a.xuid(), 1, nullptr) == hr::kInvalidArg);
  // stats.read with a broken response still returns a row per player.
  lobby.Override("stats.read", [] { return RawOk(R"({"views":"x"})"); });
  std::vector<StatsView> views;
  REQUIRE(a.x->StatsRead({a.xuid(), b.xuid()}, {{1, {2}}, {2, {}}}, &views) == hr::kOk);
  REQUIRE(views.size() == 2);
  CHECK(views[0].rows.size() == 2 && views[1].rows.size() == 2 && views[1].view_id == 2);
  CHECK(views[1].rows[0].columns.empty());
  lobby.Override("stats.read", nullptr);
}

TEST_CASE("xlive: attribute changes are pushed debounced from a worker thread") {
  FakeLobby lobby;
  Console a(&lobby, 1);
  a.x->UserSetContext(0, kContextGameMode, 1);  // not hosting yet: nothing to push
  const SessionInfo info = Host(a, 1, kPlayerHost, 4);
  std::this_thread::sleep_for(600ms);
  CHECK(lobby.Count("session.attrs") == 0);

  for (int i = 0; i < 4; ++i) {
    a.x->UserSetContext(0, kContextGameMode, 10 + uint32_t(i));
    std::this_thread::sleep_for(50ms);
  }
  a.x->UserSetProperty(0, Int32(0x1000000C, 3));
  const auto last_change = std::chrono::steady_clock::now();
  a.x->UserSetProperty(0, Int32(0x1000000C, 3));  // same value: not a change
  std::this_thread::sleep_for(250ms);
  CHECK(lobby.Count("session.attrs") == 0);
  REQUIRE(WaitFor([&] { return lobby.Count("session.attrs") >= 1; }, 2000));
  std::this_thread::sleep_for(700ms);
  const auto pushes = lobby.Calls("session.attrs");
  REQUIRE(pushes.size() == 1);  // the burst collapsed into one push
  CHECK(pushes[0].at - last_change >= 450ms);
  CHECK(pushes[0].thread != std::this_thread::get_id());  // never on the guest thread
  CHECK(pushes[0].args["session_id"] == codec::KidToHex(info.session_id));
  CHECK(pushes[0].args["contexts"] == json::parse("[[32779,13]]"));
  CHECK(pushes[0].args["properties"] == json::parse(R"([{"id":268435468,"t":1,"v":3}])"));
  CHECK((*lobby.SessionDoc(KidU64(info.session_id)))["contexts"] == json::parse("[[32779,13]]"));

  // A continuous stream of changes is still pushed within the 2 s cap.
  lobby.ClearCalls();
  bool pushed_during_stream = false;
  for (uint32_t i = 0; i < 26 && !pushed_during_stream; ++i) {
    a.x->UserSetContext(0, 9, i);
    std::this_thread::sleep_for(100ms);
    pushed_during_stream = lobby.Count("session.attrs") > 0;
  }
  CHECK(pushed_during_stream);

  // A transport failure is retried.
  std::this_thread::sleep_for(700ms);  // let a trailing push from the stream finish
  lobby.ClearCalls();
  lobby.DropNext("session.attrs");
  a.x->UserSetContext(0, 8, 1);
  REQUIRE(WaitFor([&] { return lobby.Count("session.attrs") >= 2; }, 4000));
  CHECK(lobby.Calls("session.attrs").back().args["contexts"].size() == 3);

  // Contexts changed while session.create is in flight are pushed once it completes.
  REQUIRE(a.x->SessionDelete(1) == hr::kOk);
  std::this_thread::sleep_for(100ms);
  lobby.ClearCalls();
  a.x->SessionHandleCreated(2);
  lobby.Override("session.create", [&] {
    a.x->UserSetContext(0, kContextPresence, 4);  // a guest thread changes presence meanwhile
    return RawOk(R"({"session_id":"80000000000000bb","key":"000102030405060708090a0b0c0d0e0f",)"
                 R"("nonce":"0000000000000001"})");
  });
  SessionInfo info2;
  uint64_t nonce2 = 0;
  REQUIRE(a.x->SessionCreate(2, kPlayerHost, 4, 0, 0, &info2, &nonce2) == hr::kOk);
  lobby.Override("session.create", nullptr);
  CHECK(nonce2 == 1 && KidU64(info2.session_id) == 0x80000000000000bbull);
  CHECK(lobby.Count("session.attrs") == 0);
  REQUIRE(WaitFor([&] { return lobby.Count("session.attrs") == 1; }, 2000));
  const auto late = lobby.Calls("session.attrs").at(0).args;
  CHECK(late["session_id"] == "80000000000000bb");
  CHECK(late["contexts"].dump().find("[32769,4]") != std::string::npos);
}

TEST_CASE("xlive: concurrent guest threads neither deadlock nor leak sessions") {
  FakeLobby lobby;
  Console a(&lobby, 1), b(&lobby, 2);
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (uint32_t t = 0; t < 4; ++t) {
    workers.emplace_back([&, t] {
      for (uint32_t i = 0; i < 40; ++i) {
        const SessionHandle h = 0x1000 + t * 100 + i;
        a.x->SessionHandleCreated(h);
        SessionInfo info;
        uint64_t nonce = 0;
        if (a.x->SessionCreate(h, kPlayerHost, 8, 0, 0, &info, &nonce) != hr::kOk ||
            a.x->SessionJoinLocal(h, {0}, {false}) != hr::kOk ||
            a.x->SessionStart(h, 0) != hr::kOk ||
            a.x->SessionWriteStats(h, a.xuid(), {{1, {Int32(2, int32_t(i))}}}) != hr::kOk ||
            a.x->SessionEnd(h) != hr::kOk) {
          ++failures;
        }
        if (i % 2) a.x->SessionDelete(h);  // odd: deleted; even: left to the handle close
        a.x->SessionHandleClosed(h);
      }
    });
  }
  std::thread setter([&] {
    for (uint32_t i = 0; !stop; ++i) {
      a.x->UserSetContext(0, 9, i % 17);
      std::this_thread::sleep_for(1ms);
    }
  });
  std::thread searcher([&] {
    std::vector<SearchResult> out;
    while (!stop) b.x->SessionSearch(1, 0, 50, 1, {}, {}, &out);
  });
  for (auto& w : workers) w.join();
  stop = true;
  setter.join();
  searcher.join();
  CHECK(failures == 0);
  REQUIRE(WaitFor([&] { return lobby.SessionCount() == 0; }, 5000));
  CHECK(lobby.Count("session.create") == 160);
  CHECK(lobby.Count("session.delete") == 160);

  // A handle closed while its create is still in flight: the server session is removed after
  // the create completes.
  a.backend.SetLatency(100ms);
  a.x->SessionHandleCreated(0x9000);
  std::thread creator([&] {
    SessionInfo info;
    uint64_t nonce = 0;
    a.x->SessionCreate(0x9000, kPlayerHost, 4, 0, 0, &info, &nonce);
  });
  std::this_thread::sleep_for(30ms);
  a.x->SessionHandleClosed(0x9000);
  creator.join();
  REQUIRE(WaitFor([&] { return lobby.Count("session.create") == 161 && lobby.SessionCount() == 0; },
                  3000));
  a.backend.SetLatency(0ms);
}
