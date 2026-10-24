// ac6net helper tests: hex/IPv4 helpers, JSON escaping and tracing, identity.json, and the
// XNADDR / machine ID / compat key functions.
#include <sys/stat.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ac6net/log.h"
#include "ac6net/netplay.h"
#include "ac6net/types.h"
#include "identity.h"
#include "mini_test.h"
#include "nlohmann/json.hpp"
#include "overlay_test_util.h"

using namespace ac6net;
using json = nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("types: hex encode/decode") {
  const uint8_t b[] = {0x00, 0x01, 0xAB, 0xFF};
  CHECK(HexEncode(b, sizeof(b)) == "0001abff");
  CHECK(HexEncode(nullptr, 0).empty());
  uint8_t out[4] = {9, 9, 9, 9};
  CHECK(HexDecode("0001ABff", out, 4));
  CHECK(std::memcmp(out, b, 4) == 0);
  uint8_t keep[2] = {7, 7};
  CHECK(!HexDecode("0g", keep, 1));
  CHECK(!HexDecode("abc", keep, 2));    // odd length
  CHECK(!HexDecode("abcdef", keep, 2)); // wrong length
  CHECK(!HexDecode("zz00", keep, 2));
  CHECK(keep[0] == 7 && keep[1] == 7);  // untouched on failure
}

TEST_CASE("types: IPv4 to/from string") {
  CHECK(Ipv4ToString(0x0A400007) == "10.64.0.7");
  CHECK(Ipv4ToString(0) == "0.0.0.0");
  CHECK(Ipv4ToString(0xFFFFFFFF) == "255.255.255.255");
  uint32_t ip = 0;
  CHECK(Ipv4FromString("10.64.0.7", &ip) && ip == 0x0A400007);
  CHECK(Ipv4FromString("255.255.255.255", &ip) && ip == 0xFFFFFFFF);
  CHECK(Ipv4FromString("010.064.000.007", &ip) && ip == 0x0A400007);
  for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.4 ", " 1.2.3.4", "1..2.3",
                          "1.2.3.", "a.b.c.d", "1.2.3.1234", "-1.2.3.4", "1.2.3.4x"}) {
    CHECK(!Ipv4FromString(bad, &ip));
  }
}

TEST_CASE("log: JsonEscape") {
  CHECK(JsonEscape("plain") == "plain");
  CHECK(JsonEscape("a\"b\\c") == "a\\\"b\\\\c");
  CHECK(JsonEscape("\n\r\t\b\f") == "\\n\\r\\t\\b\\f");
  CHECK(JsonEscape(std::string_view("\x01\x1f\x7f", 3)) == "\\u0001\\u001f\\u007f");
  CHECK(JsonEscape("Ünïcode") == "Ünïcode");
  const json j = json::parse("\"" + JsonEscape("x\"\n\x02y") + "\"", nullptr, false);
  CHECK(j.is_string() && j.get<std::string>() == "x\"\n\x02y");
}

TEST_CASE("log: sink and thread-safe JSONL trace") {
  std::vector<std::string> lines;
  std::mutex mu;
  SetLogSink([&](LogLevel lvl, std::string_view msg) {
    std::lock_guard<std::mutex> lock(mu);
    lines.emplace_back(msg);
  });
  LogF(LogLevel::kInfo, "hello %d %s", 42, std::string(3000, 'x').c_str());  // > stack buffer
  SetLogSink(nullptr);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].size() == 9 + 3000);
  CHECK(lines[0].rfind("hello 42 ", 0) == 0);

  const std::string dir = overlay_test::MakeTempDir("trace");
  const std::string path = dir + "/trace.jsonl";
  CHECK(!TraceEnabled());
  Trace("ignored");
  REQUIRE(SetTracePath(path));
  CHECK(TraceEnabled());
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < 250; ++i) Trace("ev", "\"t\":" + std::to_string(t) + ",\"i\":" + std::to_string(i));
    });
  }
  for (auto& th : threads) th.join();
  Trace("bare");
  CHECK(SetTracePath(""));
  CHECK(!TraceEnabled());
  std::ifstream f(path);
  std::string line;
  int n = 0, bare = 0;
  bool all_valid = true;
  while (std::getline(f, line)) {
    const json j = json::parse(line, nullptr, false);
    all_valid = all_valid && j.is_object() && j["t_ms"].is_number_unsigned() && j["ev"].is_string();
    if (j.is_object() && j["ev"] == "bare") ++bare;
    ++n;
  }
  CHECK(all_valid);
  CHECK(n == 1001);
  CHECK(bare == 1);
  CHECK(!SetTracePath(dir + "/no/such/dir/trace.jsonl"));
  CHECK(!TraceEnabled());
}

TEST_CASE("identity: create, reload, registration cache, mode 0600, corrupt file") {
  const std::string dir = overlay_test::MakeTempDir("identity") + "/nested/netplay";
  IdentityFile id;
  std::string err;
  REQUIRE(LoadOrCreateIdentity(dir, &id, &err));
  CHECK(!id.has_registration);
  CHECK(id.pk == crypto::PublicKeyFromSecret(id.sk));
  const std::string path = dir + "/identity.json";
  REQUIRE(fs::exists(path));
#if !defined(_WIN32)  // POSIX permission bits; Windows has no 0600/0700 modes
  struct stat st {};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  REQUIRE(::stat(dir.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
#endif

  IdentityFile again;
  REQUIRE(LoadOrCreateIdentity(dir, &again, &err));
  CHECK(again.sk == id.sk);

  id.has_registration = true;
  id.server_key_hex = std::string(64, 'a');
  id.peer_id = 0x9f3c5a7b11223344;
  id.xuid = 0x0009a1b2c3d4e5f6;
  id.vip = 0x0A400007;
  id.gamertag = "Talisman \"T\"";
  REQUIRE(SaveIdentity(dir, id, &err));
#if !defined(_WIN32)
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
#endif
  IdentityFile loaded;
  REQUIRE(LoadOrCreateIdentity(dir, &loaded, &err));
  CHECK(loaded.sk == id.sk);
  CHECK(loaded.has_registration);
  CHECK(loaded.peer_id == id.peer_id);
  CHECK(loaded.xuid == id.xuid);
  CHECK(loaded.vip == id.vip);
  CHECK(loaded.gamertag == id.gamertag);
  CHECK(loaded.RegistrationMatches(std::string(64, 'A')));  // case-insensitive
  CHECK(!loaded.RegistrationMatches(std::string(64, 'b')));
  const json doc = json::parse(overlay_test::ReadFile(path), nullptr, false);
  REQUIRE(doc.is_object());
  CHECK(doc["registration"]["peer_id"] == "9f3c5a7b11223344");
  CHECK(doc["registration"]["vip"] == "10.64.0.7");

  // A malformed registration block is ignored; the key survives.
  {
    std::ofstream f(path, std::ios::trunc);
    f << "{\"v\":1,\"sk\":\"" << HexEncode(id.sk.data(), 32)
      << "\",\"registration\":{\"peer_id\":\"zz\"}}";
  }
  IdentityFile partial;
  REQUIRE(LoadOrCreateIdentity(dir, &partial, &err));
  CHECK(partial.sk == id.sk);
  CHECK(!partial.has_registration);

  // A corrupt file is moved aside and replaced by a new identity.
  {
    std::ofstream f(path, std::ios::trunc);
    f << "not json at all";
  }
  IdentityFile fresh;
  REQUIRE(LoadOrCreateIdentity(dir, &fresh, &err));
  CHECK(fresh.sk != id.sk);
  CHECK(fs::exists(path + ".corrupt"));

  // No data_dir: ephemeral identity, nothing written.
  IdentityFile eph;
  CHECK(LoadOrCreateIdentity("", &eph, &err));
  CHECK(eph.pk == crypto::PublicKeyFromSecret(eph.sk));
}

TEST_CASE("netplay: XNADDR synthesis and parsing (ARCHITECTURE.md 3)") {
  const uint32_t vip = 0x0A400107;  // 10.64.1.7
  const PeerId peer = 0x9f3c5a7b11223344;
  const XnAddr a = MakeXnAddr(vip, peer);
  CHECK(a.ina == vip);
  CHECK(a.ina_online == vip);
  CHECK(a.port_online == 1000);
  const std::array<uint8_t, 6> mac = {0x00, 0x22, 0x48, 0x40, 0x01, 0x07};
  CHECK(a.ab_enet == mac);
  // SGADDR: ina BE, spi 0, xbox_id BE, platform 2, reserved 0.
  const uint8_t sg[20] = {0x0A, 0x40, 0x01, 0x07, 0, 0, 0, 0, 0x9f, 0x3c,
                          0x5a, 0x7b, 0x11, 0x22, 0x33, 0x44, 2, 0, 0, 0};
  CHECK(std::memcmp(a.ab_online.data(), sg, 20) == 0);

  uint32_t v = 0;
  PeerId p = 0;
  REQUIRE(ParseXnAddr(a, &v, &p));
  CHECK(v == vip);
  CHECK(p == peer);
  XnAddr only_ina = a;
  only_ina.ina_online = 0;
  REQUIRE(ParseXnAddr(only_ina, &v, &p));
  CHECK(v == vip);
  XnAddr foreign = a;
  foreign.ina_online = 0xC0A80001;
  CHECK(!ParseXnAddr(foreign, &v, &p));
  CHECK(!ParseXnAddr(XnAddr{}, &v, &p));
  CHECK(MachineIdFromXnAddr(a) == 0xFA00002248400107ull);
  CHECK(IsVip(0x0A400001) && IsVip(0x0A7FFFFE) && !IsVip(0x0A3FFFFF) && !IsVip(0x0A800000));
}

TEST_CASE("netplay: compat key format and sensitivity") {
  const std::string k = ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, 1, "60");
  CHECK(k.size() == 32);
  CHECK(k.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(k == ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, 1, "60"));
  CHECK(k != ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, 1, "30"));
  CHECK(k != ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 9, 1, "60"));
  CHECK(k != ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, 2, "60"));
}
