// Test helpers for the overlay tests: a real ac6lobby server process and an Overlay client that
// records what it receives.
//
// [server] tests run in one of two modes:
//   AC6NET_TEST_SERVER_BIN=<path to ac6lobby>    the tests start (and restart) their own server
//                                                on 127.0.0.1:47810 with a fresh data directory;
//   AC6NET_TEST_SERVER=host:port + AC6NET_TEST_SERVER_KEY=<hex>   an external server (tests that
//                                                need a restart are skipped).
// Otherwise [server] tests are skipped.
#pragma once

#include <chrono>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#define AC6NET_TEST_CAN_SPAWN 1
using ServerPid = pid_t;
#else
using ServerPid = long long;
// Windows: the self-started server (posix_spawn) is not ported; [server] tests need an external
// server there (AC6NET_TEST_SERVER + AC6NET_TEST_SERVER_KEY). Restart tests are skipped.
#define AC6NET_TEST_CAN_SPAWN 0
#endif

#include "ac6net/config.h"
#include "ac6net/netplay.h"
#include "crypto.h"
#include "mini_test.h"
#include "overlay.h"

#if AC6NET_TEST_CAN_SPAWN
extern char** environ;
#endif

namespace overlay_test {

inline constexpr uint16_t kServerPort = 47810;
inline constexpr uint16_t kClientPortBase = 47811;  // clients use 47811..47818

inline void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls `pred` every 10 ms until it is true or `timeout_ms` passes.
inline bool WaitFor(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    SleepMs(10);
  }
  return pred();
}

inline std::string MakeTempDir(const std::string& tag) {
  namespace fs = std::filesystem;
  const char* base = std::getenv("TMPDIR");
  fs::path root = base && *base ? fs::path(base) : fs::temp_directory_path();
  uint8_t r[6];
  ac6net::crypto::RandomBytes(r, sizeof(r));
  fs::path dir = root / ("ac6net-test-" + tag + "-" + ac6net::HexEncode(r, sizeof(r)));
  fs::create_directories(dir);
  return dir.string();
}

inline std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// A server process owned by the test (killed by its recorded PID only).
class TestServer {
 public:
  ~TestServer() { Stop(); }

  // True if [server] tests can run at all.
  static bool Available() {
    return (AC6NET_TEST_CAN_SPAWN && mini_test::Env("AC6NET_TEST_SERVER_BIN")) ||
           (mini_test::Env("AC6NET_TEST_SERVER") && mini_test::Env("AC6NET_TEST_SERVER_KEY"));
  }
  static bool CanRestart() {
    return AC6NET_TEST_CAN_SPAWN && mini_test::Env("AC6NET_TEST_SERVER_BIN") != nullptr;
  }

  bool Start() {
    const char* bin = AC6NET_TEST_CAN_SPAWN ? mini_test::Env("AC6NET_TEST_SERVER_BIN") : nullptr;
    if (bin) {
      bin_ = bin;
      if (data_dir_.empty()) data_dir_ = MakeTempDir("server");
      return Spawn();
    }
    addr_ = mini_test::Env("AC6NET_TEST_SERVER") ? mini_test::Env("AC6NET_TEST_SERVER") : "";
    key_ = mini_test::Env("AC6NET_TEST_SERVER_KEY") ? mini_test::Env("AC6NET_TEST_SERVER_KEY") : "";
    return !addr_.empty() && !key_.empty();
  }

  // graceful: SIGTERM (the server sends a shutdown NOTICE); otherwise SIGKILL (no notice).
  void Stop(bool graceful = true) {
    if (pid_ <= 0) return;
#if AC6NET_TEST_CAN_SPAWN
    ::kill(pid_, graceful ? SIGTERM : SIGKILL);
    int status = 0;
    for (int i = 0; i < 300; ++i) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      SleepMs(10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
#endif
    pid_ = -1;
  }

  bool Restart(bool graceful = true) {
    Stop(graceful);
    return Spawn();
  }

  const std::string& addr() const { return addr_; }
  std::string log_path() const { return data_dir_.empty() ? "" : data_dir_ + "/server.log"; }
  const std::string& key() const { return key_; }
  ServerPid pid() const { return pid_; }

 private:
  bool Spawn() {
#if !AC6NET_TEST_CAN_SPAWN
    std::fprintf(stderr, "  self-started test server is not supported on this platform\n");
    return false;
#else
    addr_ = "127.0.0.1:" + std::to_string(kServerPort);
    const std::string data_arg = data_dir_;
    std::vector<std::string> args = {bin_, "-data", data_arg, "-udp", addr_, "-http", "",
                                     "-log-level", "warn"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // Server output goes to <data>/server.log, never to our stdout/stderr (a server outliving
    // a crashed test would otherwise hold the test's output pipe open).
    const std::string log_path = data_dir_ + "/server.log";
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, bin_.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
      std::fprintf(stderr, "  cannot spawn %s\n", bin_.c_str());
      return false;
    }
    pid_ = pid;
    const std::string pub = data_dir_ + "/server.pub";
    const bool ok = WaitFor(
        [&] {
          int status = 0;
          if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return true;  // exited; reported below
          }
          return Trim(ReadFile(pub)).size() == 64;
        },
        5000);
    if (!ok || pid_ <= 0) {
      std::fprintf(stderr, "  server did not start (pid %d)\n", static_cast<int>(pid_));
      return false;
    }
    key_ = Trim(ReadFile(pub));
    SleepMs(150);  // let it bind the UDP socket
    return true;
#endif
  }

  std::string bin_;
  std::string data_dir_;
  std::string addr_;
  std::string key_;
  ServerPid pid_ = -1;
};

struct Received {
  uint32_t vip;
  ac6net::Channel channel;
  std::vector<uint8_t> data;
};

// An Overlay plus recording handlers.
class TestClient {
 public:
  TestClient(const std::string& gamertag, uint16_t port, const TestServer& server) {
    ac6net::crypto::PublicKey pk;
    ac6net::crypto::GenerateKeypair(&sk_, &pk);
    cfg_.enabled = true;
    cfg_.server = server.addr();
    cfg_.server_key_hex = server.key();
    cfg_.gamertag = gamertag;
    cfg_.port = port;
    cfg_.compat_key = Compat();
    overlay_.SetDataHandler([this](uint32_t vip, ac6net::Channel ch, const uint8_t* d, size_t n) {
      bool echo = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        received_.push_back({vip, ch, std::vector<uint8_t>(d, d + n)});
        echo = echo_;
      }
      // Handlers run without overlay locks, so they may call back into Send (vnet does).
      if (echo) overlay_.Send(vip, ch, d, n);
    });
    overlay_.SetPeerLostHandler([this](uint32_t vip) {
      std::lock_guard<std::mutex> lock(mu_);
      lost_.push_back(vip);
    });
    overlay_.SetStateHandler([this](ac6net::NetState s) {
      std::lock_guard<std::mutex> lock(mu_);
      states_.push_back(s);
    });
    overlay_.SetTickHandler([this](uint64_t) { ticks_.fetch_add(1); });
  }

  // Stop the network thread before the members its handlers use are destroyed.
  ~TestClient() { overlay_.Stop(); }

  static std::string Compat() {
    return ac6net::ComputeCompatKey(0x4E4D07D1, 0x531C30BE, 8, ac6net::kProtocolVersion, "30");
  }

  ac6net::Config& config() { return cfg_; }
  ac6net::Overlay& overlay() { return overlay_; }
  const ac6net::crypto::SecretKey& secret_key() const { return sk_; }

  bool Start(uint32_t wait_ms = 5000) {
    if (!overlay_.Start(cfg_, sk_)) return false;
    return overlay_.WaitOnline(wait_ms);
  }
  void Stop() { overlay_.Stop(); }
  uint32_t vip() const { return overlay_.LocalVip(); }
  void set_echo(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    echo_ = on;
  }

  size_t CountFrom(uint32_t vip) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = 0;
    for (const auto& r : received_)
      if (r.vip == vip) ++n;
    return n;
  }
  bool WaitCountFrom(uint32_t vip, size_t n, int timeout_ms) {
    return WaitFor([&] { return CountFrom(vip) >= n; }, timeout_ms);
  }
  std::vector<Received> received() {
    std::lock_guard<std::mutex> lock(mu_);
    return received_;
  }
  std::vector<uint32_t> lost() {
    std::lock_guard<std::mutex> lock(mu_);
    return lost_;
  }
  std::vector<ac6net::NetState> states() {
    std::lock_guard<std::mutex> lock(mu_);
    return states_;
  }
  uint64_t ticks() const { return ticks_.load(); }

  // The peer record for `vip`, if any.
  bool Peer(uint32_t vip, ac6net::OverlayPeerInfo* out) {
    for (const auto& p : overlay_.Peers()) {
      if (p.vip == vip) {
        if (out) *out = p;
        return true;
      }
    }
    return false;
  }
  bool IsDirectTo(uint32_t vip) {
    ac6net::OverlayPeerInfo p;
    return Peer(vip, &p) && p.direct;
  }

 private:
  ac6net::crypto::SecretKey sk_{};
  ac6net::Config cfg_;
  ac6net::Overlay overlay_;
  std::mutex mu_;
  std::vector<Received> received_;
  std::vector<uint32_t> lost_;
  std::vector<ac6net::NetState> states_;
  std::atomic<uint64_t> ticks_{0};
  bool echo_ = false;
};

inline std::vector<uint8_t> Payload(uint32_t seq, size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>((seq * 131 + i * 7) & 0xFF);
  if (len >= 4) {
    v[0] = static_cast<uint8_t>(seq >> 24);
    v[1] = static_cast<uint8_t>(seq >> 16);
    v[2] = static_cast<uint8_t>(seq >> 8);
    v[3] = static_cast<uint8_t>(seq);
  }
  return v;
}

}  // namespace overlay_test
