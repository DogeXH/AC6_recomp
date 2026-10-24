// End-to-end test of the ac6net public API against a real ac6lobby server.
// Requires AC6NET_TEST_SERVER ("127.0.0.1:port") and AC6NET_TEST_SERVER_KEY (64 hex); otherwise
// the cases are skipped. tools/run_core_tests.sh starts a throwaway server and sets both.
//
// Mirrors AC6's online flow (re/xmsg_map_verify.md, re/transport_map.md):
//   host: SetContext(0x800A/0x800B) -> XSessionCreate(0x42F) -> QosListen(40-byte blob)
//         -> VDP:999 socket -> TCP listen 21000 / UDP 20000+i
//   join: SearchEx(proc 1) -> XSessionCreate(0x42E, result.info) -> XnAddrToInAddr(host)
//         -> QosLookup(1 host, 8 probes) -> VDP msg to host:999 -> TCP connect host:21000
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ac6net/netplay.h"
#include "ac6net/vnet.h"
#include "ac6net/xlive.h"
#include "mini_test.h"

using namespace ac6net;
namespace fs = std::filesystem;

namespace {

struct ServerEnv {
  std::string addr, key;
  bool ok = false;
};

ServerEnv GetServer() {
  ServerEnv e;
  const char* a = mini_test::Env("AC6NET_TEST_SERVER");
  const char* k = mini_test::Env("AC6NET_TEST_SERVER_KEY");
  if (a && k) {
    e.addr = a;
    e.key = k;
    e.ok = true;
  }
  return e;
}

std::string TempDir(const std::string& name) {
  auto p = fs::temp_directory_path() / ("ac6net_fullstack_" + name + "_" +
                                        std::to_string(mini_test::Pid()));
  fs::remove_all(p);
  fs::create_directories(p);
  return p.string();
}

std::unique_ptr<Netplay> StartConsole(const ServerEnv& env, const std::string& name,
                                      bool force_relay) {
  Config c;
  c.enabled = true;
  c.server = env.addr;
  c.server_key_hex = env.key;
  c.gamertag = name;
  c.data_dir = TempDir(name);
  c.port = 0;
  c.force_relay = force_relay;
  c.lan = true;
  c.compat_key = ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, kProtocolVersion, "test");
  auto np = std::make_unique<Netplay>();
  if (!np->Start(c)) return nullptr;
  if (!np->WaitRegistered(8000)) return nullptr;
  np->xlive().SetOfflineXuid(0xE000000000000001ull);
  return np;
}

bool WaitFor(const std::function<bool()>& pred, int timeout_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

void RunAc6Flow(bool joiner_force_relay) {
  ServerEnv env = GetServer();
  if (!env.ok) SKIP("AC6NET_TEST_SERVER not set");

  auto host = StartConsole(env, joiner_force_relay ? "HostR" : "Host", false);
  auto join = StartConsole(env, joiner_force_relay ? "JoinR" : "Join", joiner_force_relay);
  REQUIRE(host && join);
  REQUIRE(host->identity().valid && join->identity().valid);
  CHECK(host->identity().vip != join->identity().vip);
  CHECK(IsVip(host->identity().vip));
  CHECK((host->identity().xuid >> 48) == 0x0009);

  auto& hx = host->xlive();
  auto& jx = join->xlive();
  CHECK(hx.SignedInToLive());
  CHECK(jx.SignedInToLive());

  // --- Host: contexts, create (flags 0x42F: player match host) ---
  hx.UserSetContext(0, kContextGameType, 1);
  hx.UserSetContext(0, kContextGameMode, 0);
  Property p1;
  p1.id = 0x1000000D;
  p1.value.type = DataType::kInt32;
  p1.value.i = 0;  // EnableJoin: query 1 requires ENABLE_JOIN (0)
  hx.UserSetProperty(0, p1);
  const xlive::SessionHandle hh = 0xF1000001;
  hx.SessionHandleCreated(hh);
  SessionInfo hinfo{};
  uint64_t hnonce = 0;
  REQUIRE(hx.SessionCreate(hh, 0x42F, 16, 0, 0, &hinfo, &hnonce) == hr::kOk);
  CHECK(hinfo.session_id.b[0] == 0x80);
  CHECK(hinfo.host == host->LocalXnAddr());
  REQUIRE(hx.SessionJoinLocal(hh, {0}, {false}) == hr::kOk);

  // QoS blob as AC6 builds it (40 bytes; gamertag at +12, xuid at +32).
  std::vector<uint8_t> blob(40, 0);
  blob[0] = 0x80;
  std::memcpy(&blob[12], "Host", 4);
  host->QosListen(hinfo.session_id, blob.data(), blob.size(), 0, 0x0D);

  // --- Joiner: search (proc 1, contexts game type/mode) ---
  jx.UserSetContext(0, kContextGameType, 1);
  jx.UserSetContext(0, kContextGameMode, 0);
  std::vector<xlive::SearchResult> results;
  REQUIRE(jx.SessionSearch(1, 0, 50, 1, {}, {{kContextGameType, 1}, {kContextGameMode, 0}},
                           &results) == hr::kOk);
  REQUIRE(!results.empty());
  const xlive::SearchResult* found = nullptr;
  for (auto& r : results)
    if (r.info.session_id == hinfo.session_id) found = &r;
  REQUIRE(found);
  CHECK(found->filled_public == 1);
  CHECK(found->open_public == 15);

  // Wrong mode must not match.
  std::vector<xlive::SearchResult> none;
  REQUIRE(jx.SessionSearch(1, 0, 50, 1, {}, {{kContextGameType, 1}, {kContextGameMode, 2}},
                           &none) == hr::kOk);
  for (auto& r : none) CHECK(!(r.info.session_id == hinfo.session_id));

  // --- Joiner: create (join) + join local; host admits remote ---
  const xlive::SessionHandle jh = 0xF2000001;
  jx.SessionHandleCreated(jh);
  SessionInfo jinfo = found->info;
  uint64_t jnonce = 0;
  REQUIRE(jx.SessionCreate(jh, 0x42E, 16, 0, 0, &jinfo, &jnonce) == hr::kOk);
  REQUIRE(jx.SessionJoinLocal(jh, {0}, {false}) == hr::kOk);
  REQUIRE(hx.SessionJoinRemote(hh, {join->identity().xuid}, {false}) == hr::kOk);

  // --- XNet: the joiner resolves the host XNADDR to the host vIP ---
  uint32_t host_vip = 0;
  REQUIRE(join->XnAddrToInAddr(found->info.host, &found->info.session_id, &host_vip) == wsa::kOk);
  CHECK(host_vip == host->identity().vip);
  uint32_t self_vip = 0;
  REQUIRE(host->XnAddrToInAddr(host->LocalXnAddr(), &hinfo.session_id, &self_vip) == wsa::kOk);
  CHECK(self_vip == host->identity().vip);
  XnAddr back{};
  REQUIRE(host->InAddrToXnAddr(self_vip, &back, nullptr) == wsa::kOk);
  CHECK(back == host->LocalXnAddr());

  // --- QoS lookup of the host (1 target, 8 probes) ---
  std::mutex m;
  std::condition_variable cv;
  bool qos_done = false;
  QosTargetResult qres;
  uint64_t lid = join->QosLookup({found->info.host}, {found->info.session_id}, 8,
                                 [&](size_t, const QosTargetResult& r, bool all_done) {
                                   std::lock_guard<std::mutex> lk(m);
                                   qres = r;
                                   if (all_done) qos_done = true;
                                   cv.notify_all();
                                 });
  REQUIRE(lid != 0);
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(8), [&] { return qos_done; });
  }
  CHECK(qos_done);
  CHECK(qres.contacted);
  CHECK(qres.data_received);
  CHECK(qres.data == blob);
  CHECK(qres.up_bps >= 4000000);

  // --- T1: VDP lobby socket on 999, both sides; source port must be 999 ---
  auto& hv = host->vnet();
  auto& jv = join->vnet();
  int err = 0;
  auto hs = hv.Socket(vnet::kSockDgram, vnet::kProtoVdp, &err);
  auto js = jv.Socket(vnet::kSockDgram, vnet::kProtoVdp, &err);
  REQUIRE(hs > 0 && js > 0);
  REQUIRE(hv.Bind(hs, {0, 999}, &err) == 0);
  REQUIRE(jv.Bind(js, {0, 999}, &err) == 0);
  hv.SetNonBlocking(hs, true, &err);
  jv.SetNonBlocking(js, true, &err);
  const char msg3[] = "\x00\x52join-request";
  REQUIRE(jv.SendTo(js, msg3, sizeof(msg3), 0, {host_vip, 999}, &err) == (int)sizeof(msg3));
  char buf[2048];
  vnet::Endpoint from{};
  int n = -1;
  CHECK(WaitFor([&] {
    n = hv.RecvFrom(hs, buf, sizeof(buf), 0, &from, &err);
    return n > 0;
  }, 5000));
  CHECK(n == (int)sizeof(msg3));
  CHECK(from.ip == join->identity().vip);
  CHECK(from.port == 999);
  REQUIRE(hv.SendTo(hs, "accept", 6, 0, from, &err) == 6);
  CHECK(WaitFor([&] {
    n = jv.RecvFrom(js, buf, sizeof(buf), 0, &from, &err);
    return n > 0;
  }, 5000));
  CHECK(n == 6);
  CHECK(from.ip == host_vip && from.port == 999);

  // --- T2: TCP 21000 (host listen, client nonblocking connect) ---
  auto ls = hv.Socket(vnet::kSockStream, 0, &err);
  REQUIRE(hv.Bind(ls, {0, 21000}, &err) == 0);
  REQUIRE(hv.Listen(ls, 16, &err) == 0);
  hv.SetNonBlocking(ls, true, &err);
  auto cs = jv.Socket(vnet::kSockStream, 0, &err);
  jv.SetNonBlocking(cs, true, &err);
  CHECK(jv.Connect(cs, {host_vip, 21000}, &err) == -1);
  CHECK(err == wsa::kWouldBlock);
  vnet::SelectSets ws;
  ws.write = {cs};
  CHECK(jv.Select(&ws, 5'000'000, &err) == 1);
  vnet::SocketId as = -1;
  vnet::Endpoint peer{};
  CHECK(WaitFor([&] {
    as = hv.Accept(ls, &peer, &err);
    return as > 0;
  }, 5000));
  REQUIRE(as > 0);
  CHECK(peer.ip == join->identity().vip);
  // Game framing: [u16 LE len][payload]
  std::vector<uint8_t> frame = {5, 0, 'h', 'e', 'l', 'l', 'o'};
  REQUIRE(jv.Send(cs, frame.data(), frame.size(), 0, &err) == (int)frame.size());
  vnet::SelectSets rs;
  rs.read = {as};
  CHECK(hv.Select(&rs, 5'000'000, &err) == 1);
  uint8_t b0 = 0, b1 = 0;
  CHECK(hv.Recv(as, &b0, 1, 0, &err) == 1);
  CHECK(hv.Recv(as, &b1, 1, 0, &err) == 1);  // never WOULDBLOCK mid-frame
  CHECK(hv.Recv(as, buf, 5, 0, &err) == 5);
  CHECK(std::memcmp(buf, "hello", 5) == 0);

  // --- Arbitration (ranked path) ---
  std::vector<xlive::Registrant> hreg, jreg;
  CHECK(hx.SessionArbitrationRegister(hh, 0, hnonce, 300, &hreg) == hr::kOk);
  CHECK(jx.SessionArbitrationRegister(jh, 0, hnonce, 300, &jreg) == hr::kOk);
  size_t users = 0;
  for (auto& r : hreg) users += r.users.size();
  CHECK(users == 2);

  // --- Start / stats / end ---
  CHECK(hx.SessionStart(hh, 0) == hr::kOk);
  CHECK(jx.SessionStart(jh, 0) == hr::kOk);
  xlive::StatsWriteView v;
  v.view_id = 1;
  Property col;
  col.id = 2;
  col.value.type = DataType::kInt64;
  col.value.i = 1234;
  v.properties = {col};
  CHECK(hx.SessionWriteStats(hh, host->identity().xuid, {v}) == hr::kOk);
  CHECK(hx.SessionEnd(hh) == hr::kOk);
  CHECK(jx.SessionEnd(jh) == hr::kOk);
  std::vector<xlive::StatsView> views;
  CHECK(jx.StatsRead({host->identity().xuid}, {{1, {2}}}, &views) == hr::kOk);
  REQUIRE(!views.empty() && !views[0].rows.empty());
  CHECK(views[0].rows[0].columns.size() == 1);
  if (!views[0].rows[0].columns.empty()) CHECK(views[0].rows[0].columns[0].value.i == 1234);

  // --- Teardown ---
  CHECK(jx.SessionDelete(jh) == hr::kOk);
  CHECK(hx.SessionDelete(hh) == hr::kOk);
  std::vector<xlive::SearchResult> after;
  jx.SessionSearch(1, 0, 50, 1, {}, {{kContextGameType, 1}, {kContextGameMode, 0}}, &after);
  for (auto& r : after) CHECK(!(r.info.session_id == hinfo.session_id));
  join->Stop();
  host->Stop();
}

}  // namespace

TEST_CASE("[server] fullstack: AC6 host/search/join/QoS/VDP/TCP/arbitration/stats (direct)") {
  RunAc6Flow(false);
}

TEST_CASE("[server] fullstack: same flow with the joiner forced through the relay") {
  RunAc6Flow(true);
}
