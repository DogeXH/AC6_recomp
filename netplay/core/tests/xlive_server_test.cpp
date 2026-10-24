// Integration test: the ranked session lifecycle through two in-process Netplay instances and
// a real ac6lobby (ARCHITECTURE.md section 9.2). It needs the whole core (overlay, vnet) and a
// running server, so it compiles only in the merged tree and is skipped unless
// AC6NET_TEST_SERVER="host:port" and AC6NET_TEST_SERVER_KEY="<64 hex>" are set.
// Client UDP ports: 47831 and 47832 (the xlive lane's test range).
#if __has_include("overlay.h")

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "ac6net/netplay.h"
#include "ac6net/xlive.h"
#include "mini_test.h"

using namespace ac6net;
using namespace ac6net::xlive;

namespace {

Property ServerInt(uint32_t id, DataType type, int64_t v) {
  Property p;
  p.id = id;
  p.value.type = type;
  p.value.i = v;
  return p;
}

Config ServerConfig(const char* server, const char* key, const std::string& gamertag, uint16_t port,
                    const std::filesystem::path& dir) {
  Config c;
  c.enabled = true;
  c.server = server;
  c.server_key_hex = key;
  c.gamertag = gamertag;
  c.data_dir = dir.string();
  c.port = port;
  c.title_id = "4E4D07D1";
  c.compat_key = ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, kProtocolVersion, "60");
  return c;
}

const SearchResult* FindResult(const std::vector<SearchResult>& results, const XnKid& kid) {
  for (const auto& r : results)
    if (r.info.session_id == kid) return &r;
  return nullptr;
}

const StatsColumn* FindColumn(const StatsRow& row, uint16_t id) {
  for (const auto& c : row.columns)
    if (c.column_id == id) return &c;
  return nullptr;
}

}  // namespace

TEST_CASE("xlive [server]: ranked lifecycle through a real lobby") {
  const char* server = mini_test::Env("AC6NET_TEST_SERVER");
  const char* key = mini_test::Env("AC6NET_TEST_SERVER_KEY");
  if (!server || !key) SKIP("AC6NET_TEST_SERVER / AC6NET_TEST_SERVER_KEY not set");

  const auto root =
      std::filesystem::temp_directory_path() / ("ac6net-xlive-" + std::to_string(mini_test::Pid()));
  std::filesystem::create_directories(root / "a");
  std::filesystem::create_directories(root / "b");
  // A per-run game mode keeps sessions from earlier runs out of the search.
  const uint32_t mode = 1000 + std::random_device{}() % 100000;

  {
    Netplay a, b;
    REQUIRE(a.Start(ServerConfig(server, key, "XliveHost", 47831, root / "a")));
    REQUIRE(b.Start(ServerConfig(server, key, "XliveJoin", 47832, root / "b")));
    REQUIRE(a.WaitRegistered(5000) && a.state() == NetState::kOnline);
    REQUIRE(b.WaitRegistered(5000) && b.state() == NetState::kOnline);
    XLive& xa = a.xlive();
    XLive& xb = b.xlive();
    REQUIRE(xa.SignedInToLive() && xb.SignedInToLive());
    const Xuid ax = xa.OnlineXuid(), bx = xb.OnlineXuid();
    CHECK((ax >> 48) == 0x0009 && (bx >> 48) == 0x0009 && ax != bx);

    // Host: what AC6 sets before a ranked XSessionCreate (C7).
    xa.UserSetContext(0, kContextGameType, 0);
    xa.UserSetContext(0, kContextGameMode, mode);
    xa.UserSetContext(0, kContextPresence, 3);
    xa.UserSetProperty(0, ServerInt(0x1000000C, DataType::kInt32, 2));
    xa.UserSetProperty(0, ServerInt(0x1000000D, DataType::kInt32, 0));
    xa.SessionHandleCreated(0x100);
    SessionInfo info;
    uint64_t nonce = 0;
    REQUIRE(xa.SessionCreate(0x100, 0x53F, 16, 0, 0, &info, &nonce) == hr::kOk);
    CHECK(info.session_id.b[0] == 0x80);
    REQUIRE(xa.SessionJoinLocal(0x100, {0}, {false}) == hr::kOk);

    // Joiner: AC6's search (proc 1, 50 results, 1 user).
    std::vector<SearchResult> results;
    REQUIRE(xb.SessionSearch(1, 0, 50, 1, {ServerInt(0x1000000C, DataType::kInt32, 2)},
                             {{kContextGameType, 0}, {kContextGameMode, mode}},
                             &results) == hr::kOk);
    const SearchResult* found = FindResult(results, info.session_id);
    REQUIRE(found);
    CHECK(found->info.key == info.key && found->info.host == a.LocalXnAddr());
    CHECK(found->filled_public == 1 && found->open_public == 15);
    REQUIRE(found->contexts.size() == 2);
    CHECK(found->contexts[1].id == kContextGameMode && found->contexts[1].value == mode);

    xb.SessionHandleCreated(0x200);
    SessionInfo binfo = found->info;
    uint64_t bnonce = 0;
    REQUIRE(xb.SessionCreate(0x200, 0x53E, 16, 0, 0, &binfo, &bnonce) == hr::kOk);
    CHECK(bnonce == nonce);
    REQUIRE(xb.SessionJoinLocal(0x200, {0}, {false}) == hr::kOk);
    REQUIRE(xa.SessionJoinRemote(0x100, {bx}, {false}) == hr::kOk);
    REQUIRE(xb.SessionJoinRemote(0x200, {ax}, {false}) == hr::kOk);

    std::vector<Registrant> regs;
    REQUIRE(xa.SessionArbitrationRegister(0x100, 0, nonce, 300, &regs) == hr::kOk);
    CHECK(regs.size() == 2);
    REQUIRE(xb.SessionArbitrationRegister(0x200, 0, bnonce, 300, &regs) == hr::kOk);
    REQUIRE(regs.size() == 2);
    CHECK(regs[0].machine_id == MachineIdFromXnAddr(a.LocalXnAddr()));
    CHECK(regs[1].machine_id == MachineIdFromXnAddr(b.LocalXnAddr()));

    REQUIRE(xa.SessionStart(0x100, 0) == hr::kOk);
    REQUIRE(xb.SessionStart(0x200, 0) == hr::kOk);

    // Ranked board 86 (SB_WW): score 0x20000006 -> column 3, rating 0x20000008 -> 65534,
    // nation 0x10000009 -> column 2 (notes/ac6_spa_summary.json).
    for (Xuid x : {ax, bx}) {
      const int64_t score = x == ax ? 1234 : 99;
      REQUIRE(xa.SessionWriteStats(0x100, x,
                                   {{86,
                                     {ServerInt(0x20000006, DataType::kInt64, score),
                                      ServerInt(0x20000008, DataType::kInt64, score / 10),
                                      ServerInt(0x10000009, DataType::kInt32, 4)}}}) == hr::kOk);
    }
    REQUIRE(xa.SessionEnd(0x100) == hr::kOk);
    REQUIRE(xb.SessionEnd(0x200) == hr::kOk);

    std::vector<StatsView> views;
    REQUIRE(xb.StatsRead({ax, bx}, {{86, {2, 3, 1}}}, &views) == hr::kOk);
    REQUIRE(views.size() == 1 && views[0].rows.size() == 2);
    const StatsRow& ra = views[0].rows[0];
    CHECK(ra.xuid == ax && ra.rank >= 1 && ra.rating == 123);
    const StatsColumn* score = FindColumn(ra, 3);
    REQUIRE(score);
    CHECK(score->value.type == DataType::kInt64 && score->value.i == 1234);
    const StatsColumn* nation = FindColumn(ra, 2);
    REQUIRE(nation);
    CHECK(nation->value.type == DataType::kInt32 && nation->value.i == 4);

    StatsView view;
    REQUIRE(xa.StatsEnumerate({86, {2, 3, 1}}, StatsPivot::kXuid, ax, 10, &view) == hr::kOk);
    bool has_ax = false;
    for (const auto& row : view.rows) has_ax = has_ax || row.xuid == ax;
    CHECK(has_ax);

    REQUIRE(xb.SessionDelete(0x200) == hr::kOk);
    REQUIRE(xa.SessionDelete(0x100) == hr::kOk);
    REQUIRE(xb.SessionSearch(1, 0, 50, 1, {}, {{kContextGameType, 0}, {kContextGameMode, mode}},
                             &results) == hr::kOk);
    CHECK(!FindResult(results, info.session_id));
    xa.SessionHandleClosed(0x100);
    xb.SessionHandleClosed(0x200);
    a.Stop();
    b.Stop();
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

#endif  // __has_include("overlay.h")
