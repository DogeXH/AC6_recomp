// Platform-semantics tests for behaviour that differs on Windows (Winsock, UTF-16 paths, file
// sharing). They run on every platform; the Windows-only checks are marked. On POSIX most of them
// pin down the behaviour the Windows code must match.
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ac6net/log.h"
#include "identity.h"
#include "mini_test.h"
#include "overlay_net.h"
#include "overlay_test_util.h"
#include "platform.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace ac6net;
namespace fs = std::filesystem;

namespace {

// "ac6net-<u-umlaut>-<Tokyo in kanji>", spelled as UTF-8 bytes so the source encoding does not
// matter.
const std::string kUtf8Name = "ac6net-\xC3\xBC-\xE6\x9D\xB1\xE4\xBA\xAC";

// The same name built without platform.h (Windows: a UTF-16 literal), to check the helpers.
fs::path NativeName() {
#if defined(_WIN32)
  return fs::path(L"ac6net-ü-東京");
#else
  return fs::path(kUtf8Name);
#endif
}

net::Addr Loopback4(uint16_t port) {
  net::Addr a;
  net::Addr::FromIpPort("127.0.0.1", port, &a);
  return a;
}

// Waits up to `ms` for `s` to become readable.
bool WaitReadable(net::UdpSocket& s, int ms) {
  net::UdpSocket* socks[1] = {&s};
  bool readable[1] = {false};
  return net::PollReadable(socks, 1, ms, readable) && readable[0];
}

}  // namespace

TEST_CASE("platform: UTF-8 paths round-trip (Windows: not the ANSI code page)") {
  const fs::path p = platform::PathFromUtf8(kUtf8Name);
  CHECK(p == NativeName());
  CHECK(platform::PathToUtf8(p) == kUtf8Name);
  CHECK(platform::PathToUtf8(platform::PathFromUtf8("plain/ascii")) == "plain/ascii");
  CHECK(platform::PathFromUtf8("").empty());
#if defined(_WIN32)
  // Not UTF-8 (a lone 0xE9 is cp1252 e-acute): taken as ANSI text instead of failing.
  CHECK(!platform::PathFromUtf8("caf\xE9").empty());
#endif
}

TEST_CASE("identity: data_dir with non-ASCII characters is created under its real name") {
  const std::string base = overlay_test::MakeTempDir("utf8");
  const std::string dir = base + "/" + kUtf8Name;
  IdentityFile id;
  std::string err;
  REQUIRE(LoadOrCreateIdentity(dir, &id, &err));
  // The directory on disk carries the intended name, not a code-page mangling of its bytes.
  std::error_code ec;
  CHECK(fs::exists(fs::path(base) / NativeName() / "identity.json", ec));
  IdentityFile again;
  REQUIRE(LoadOrCreateIdentity(dir, &again, &err));
  CHECK(again.sk == id.sk);
  // Trace files take UTF-8 paths too.
  REQUIRE(SetTracePath(dir + "/trace.jsonl"));
  Trace("utf8_path");
  CHECK(SetTracePath(""));
  CHECK(fs::exists(fs::path(base) / NativeName() / "trace.jsonl", ec));
  fs::remove_all(fs::path(base), ec);
}

#if defined(_WIN32)
TEST_CASE("identity (Windows): a locked identity.json is kept, not replaced") {
  const std::string dir = overlay_test::MakeTempDir("locked");
  IdentityFile id;
  std::string err;
  REQUIRE(LoadOrCreateIdentity(dir, &id, &err));
  const fs::path path = platform::PathFromUtf8(dir) / "identity.json";
  const std::string before = overlay_test::ReadFile(platform::PathToUtf8(path));
  REQUIRE(!before.empty());
  // Hold the file the way a scanner without read sharing would.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  IdentityFile tmp;
  bool may_save = true;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!LoadOrCreateIdentity(dir, &tmp, &err, &may_save));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CloseHandle(h);
  CHECK(!may_save);
  CHECK(tmp.sk != id.sk);  // a throwaway identity for this run
  CHECK(waited >= std::chrono::milliseconds(500));  // it waited for the lock first
  CHECK(!fs::exists(fs::path(path).concat(".corrupt")));
  CHECK(overlay_test::ReadFile(platform::PathToUtf8(path)) == before);
  IdentityFile after;
  REQUIRE(LoadOrCreateIdentity(dir, &after, &err, &may_save));
  CHECK(may_save);
  CHECK(after.sk == id.sk);
}

TEST_CASE("identity (Windows): a lock released within a second is waited out") {
  const std::string dir = overlay_test::MakeTempDir("brieflock");
  IdentityFile id;
  std::string err;
  REQUIRE(LoadOrCreateIdentity(dir, &id, &err));
  const fs::path path = platform::PathFromUtf8(dir) / "identity.json";
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  std::thread release([h] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CloseHandle(h);
  });
  IdentityFile again;
  bool may_save = false;
  CHECK(LoadOrCreateIdentity(dir, &again, &err, &may_save));
  release.join();
  CHECK(may_save);
  CHECK(again.sk == id.sk);
}
#endif

TEST_CASE("net: UDP socket basics (bind, exclusive port, non-inheritable, v6-only)") {
  net::UdpSocket a;
  std::string err;
  REQUIRE(a.Open(AF_INET, 0, &err));
  REQUIRE(a.port() != 0);
  // A second socket on the same port must fail on every platform.
  net::UdpSocket b;
  CHECK(!b.Open(AF_INET, a.port(), &err));
  CHECK(err.find("bind failed") != std::string::npos);
#if defined(_WIN32)
  CHECK(err.find("netplay_port") != std::string::npos);  // the player-facing hint
#endif
#if defined(_WIN32)
  DWORD flags = 0;
  REQUIRE(GetHandleInformation(reinterpret_cast<HANDLE>(a.handle()), &flags));
  CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
  BOOL exclusive = FALSE;
  int len = sizeof(exclusive);
  if (getsockopt(a.handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive),
                 &len) == 0) {
    CHECK(exclusive);
  } else {
    std::fprintf(stderr, "  note: getsockopt(SO_EXCLUSIVEADDRUSE) unsupported here (%d)\n",
                 WSAGetLastError());
  }
#endif
  net::UdpSocket v6;
  if (v6.Open(AF_INET6, 0, &err)) {
    int on = 0;
    socklen_t olen = sizeof(on);
    REQUIRE(getsockopt(v6.handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&on),
                       &olen) == 0);
    CHECK(on != 0);
  }
}

TEST_CASE("net: a send to a closed port does not break the next receive (WSAECONNRESET)") {
  net::UdpSocket a, closed, c;
  std::string err;
  REQUIRE(a.Open(AF_INET, 0, &err));
  REQUIRE(closed.Open(AF_INET, 0, &err));
  REQUIRE(c.Open(AF_INET, 0, &err));
  const uint16_t dead_port = closed.port();
  closed.Close();
  const uint8_t ping[4] = {1, 2, 3, 4};
  // Windows without SIO_UDP_CONNRESET=FALSE: the ICMP port unreachable for this send makes a's
  // next recvfrom fail with WSAECONNRESET.
  CHECK(a.SendTo(Loopback4(dead_port), ping, sizeof(ping)));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const uint8_t hello[5] = {'h', 'e', 'l', 'l', 'o'};
  REQUIRE(c.SendTo(Loopback4(a.port()), hello, sizeof(hello)));
  REQUIRE(WaitReadable(a, 1000));
  uint8_t buf[64];
  net::Addr from;
  int n = a.RecvFrom(buf, sizeof(buf), &from);
  CHECK(n == 5);  // the datagram, not an error
  CHECK(n == 5 && std::memcmp(buf, hello, 5) == 0);
  CHECK(from.Port() == c.port());
  CHECK(a.RecvFrom(buf, sizeof(buf), &from) == 0);  // drained: "nothing pending"
}

TEST_CASE("net: an oversized datagram is truncated to the buffer (Windows: WSAEMSGSIZE)") {
  net::UdpSocket a, c;
  std::string err;
  REQUIRE(a.Open(AF_INET, 0, &err));
  REQUIRE(c.Open(AF_INET, 0, &err));
  std::vector<uint8_t> big(1500);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i);
  REQUIRE(c.SendTo(Loopback4(a.port()), big.data(), big.size()));
  const uint8_t tail_dgram[3] = {7, 8, 9};
  REQUIRE(c.SendTo(Loopback4(a.port()), tail_dgram, sizeof(tail_dgram)));
  REQUIRE(WaitReadable(a, 1000));
  uint8_t buf[256];
  net::Addr from;
  const int n = a.RecvFrom(buf, sizeof(buf), &from);
  CHECK(n == static_cast<int>(sizeof(buf)));
  CHECK(std::memcmp(buf, big.data(), sizeof(buf)) == 0);
  // The rest of the long datagram is discarded; the next one arrives intact.
  REQUIRE(WaitReadable(a, 1000));
  CHECK(a.RecvFrom(buf, sizeof(buf), &from) == 3);
  CHECK(buf[0] == 7 && buf[2] == 9);
}

TEST_CASE("net: PollReadable honours short timeouts (timer resolution)") {
  net::UdpSocket a;
  std::string err;
  REQUIRE(a.Open(AF_INET, 0, &err));
  const net::ScopedTimerResolution res;
  // 20 waits of 2 ms: about 40 ms with a 1 ms timer, about 310 ms on the 15.6 ms default tick.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) CHECK(!WaitReadable(a, 2));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  std::fprintf(stderr, "  20 x 2 ms poll: %lld ms\n", static_cast<long long>(ms));
#if defined(_WIN32)
  // Wine's WSAPoll adds ~10 ms per call whatever the timer resolution (measured 220-270 ms
  // here with Wine 8 on macOS), so only real Windows is held to the bound.
  if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
    std::fprintf(stderr, "  note: Wine detected; timing bound not checked\n");
    return;
  }
#endif
  CHECK(ms < 200);
}

TEST_CASE("net: local addresses list IPv4 before IPv6, no loopback or link-local") {
  const std::vector<net::Addr> all = net::LocalAddresses(true, true);
  bool seen_v6 = false;
  bool order_ok = true;
  for (const net::Addr& a : all) {
    CHECK(!a.IsLoopback());
    CHECK(a.Port() == 0);
    if (a.IsV6()) seen_v6 = true;
#if defined(_WIN32)
    if (a.IsV4() && seen_v6) order_ok = false;
#endif
    const std::string ip = a.Ip();
    CHECK(ip.rfind("169.254.", 0) != 0);
    CHECK(ip.rfind("fe80:", 0) != 0);
  }
  CHECK(order_ok);
  std::fprintf(stderr, "  %zu local address(es)\n", all.size());
}

TEST_CASE("net: host:port parsing and resolution") {
  net::Addr a;
  std::string err;
  REQUIRE(net::ResolveHostPort("127.0.0.1:47800", &a, &err));
  CHECK(a.ToString() == "127.0.0.1:47800");
  REQUIRE(net::ResolveHostPort("[::1]:5", &a, &err));
  CHECK(a.IsV6() && a.Port() == 5);
  REQUIRE(net::ResolveHostPort("localhost:47800", &a, &err));
  CHECK(a.IsV4() && a.IsLoopback() && a.Port() == 47800);  // IPv4 preferred
  CHECK(!net::ResolveHostPort("127.0.0.1", &a, &err));
  CHECK(!net::ResolveHostPort("127.0.0.1:0", &a, &err));
  CHECK(!net::ResolveHostPort("127.0.0.1:65536", &a, &err));
  CHECK(!net::ResolveHostPort("::1:5", &a, &err));
  CHECK(!net::ResolveHostPort("no-such-host.invalid:47800", &a, &err));
}
