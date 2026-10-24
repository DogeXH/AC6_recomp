// In-memory lobby and XLiveBackend for the xlive unit tests.
//
// FakeLobby implements enough of PROTOCOL.md section 3 (sessions, search, arbitration, stats)
// for several XLive instances to run the whole session lifecycle against each other. Each
// FakeBackend is one "console" with its own identity.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ac6net/config.h"
#include "ac6net/netplay.h"
#include "ac6net/types.h"
#include "xlive_backend.h"

// The overlay lane provides log.cpp/netplay.cpp. Until they are in the tree,
// xlive_standalone_stubs.cpp supplies the few functions xlive needs and captures traces.
#if !__has_include("overlay.h")
#define AC6NET_XLIVE_STANDALONE 1
#endif

namespace xlive_test {

using ac6net::PeerId;
using ac6net::Xuid;
using nlohmann::json;

struct FakeClient {
  PeerId peer_id = 0;
  Xuid xuid = 0;
  uint32_t vip = 0;
  std::string gamertag;
};

uint64_t MachineIdOf(uint32_t vip);

class FakeLobby {
 public:
  struct CallRecord {
    PeerId from = 0;
    std::string method;
    json args;
    std::chrono::steady_clock::time_point at;
    std::thread::id thread;
  };

  void Register(const FakeClient& client);
  ac6net::RpcResult Handle(const FakeClient& who, const std::string& method,
                           const std::string& args_json);

  // Inspection.
  std::vector<CallRecord> Calls() const;
  std::vector<CallRecord> Calls(const std::string& method) const;
  size_t Count(const std::string& method) const;
  void ClearCalls();
  std::optional<json> SessionDoc(uint64_t session_id) const;
  size_t SessionCount() const;
  // Last stored value of a stats column, or null.
  json StatsValue(uint32_t view_id, Xuid xuid, uint32_t column) const;

  // Failure injection (one-shot per call to the method).
  void FailNext(const std::string& method, const std::string& err);  // ok:false
  void DropNext(const std::string& method);                          // transport timeout
  // Replaces every response to `method` until cleared (empty function).
  void Override(const std::string& method, std::function<ac6net::RpcResult()> fn);

 private:
  struct Member {
    Xuid xuid = 0;
    bool is_private = false;
  };
  struct Session {
    uint64_t id = 0;
    std::string key_hex;
    uint64_t nonce = 0;
    uint32_t flags = 0;
    std::string state = "lobby";
    uint32_t public_slots = 0, private_slots = 0;
    PeerId host = 0;
    std::string title, compat;
    json contexts = json::array();
    json properties = json::array();
    std::vector<Member> members;
    uint64_t seq = 0;
  };

  json Doc(const Session& s) const;
  ac6net::RpcResult Dispatch(const FakeClient& who, const std::string& method, const json& a);

  mutable std::mutex mu_;
  std::map<PeerId, FakeClient> clients_;
  std::map<uint64_t, Session> sessions_;
  std::map<std::pair<uint32_t, Xuid>, std::map<uint32_t, json>> stats_;
  std::vector<CallRecord> calls_;
  std::map<std::string, std::string> fail_next_;
  std::map<std::string, int> drop_next_;
  std::map<std::string, std::function<ac6net::RpcResult()>> overrides_;
  uint64_t seq_ = 0;
  std::mt19937_64 rng_{42};
};

class FakeBackend final : public ac6net::xlive::XLiveBackend {
 public:
  FakeBackend(FakeLobby* lobby, FakeClient me);

  // XLiveBackend
  ac6net::RpcResult Call(const std::string& method, const std::string& args_json,
                         uint32_t timeout_ms) override;
  ac6net::Identity identity() const override;
  ac6net::NetState state() const override;
  const ac6net::Config& config() const override { return config_; }
  ac6net::XnAddr LocalXnAddr() const override;
  int XnAddrToInAddr(const ac6net::XnAddr& addr, const ac6net::XnKid* kid,
                     uint32_t* out_vip) override;
  void Random(uint8_t* out, size_t len) override;

  // Test controls.
  void SetState(ac6net::NetState state);
  void SetIdentityValid(bool valid);
  void SetLatency(std::chrono::milliseconds latency);  // delay before each RPC
  const FakeClient& me() const { return me_; }
  std::vector<std::pair<uint32_t, PeerId>> Registered() const;
  size_t CallCount() const;

 private:
  FakeLobby* lobby_;
  FakeClient me_;
  ac6net::Config config_;
  mutable std::mutex mu_;
  ac6net::NetState state_ = ac6net::NetState::kOnline;
  bool identity_valid_ = true;
  std::vector<std::pair<uint32_t, PeerId>> registered_;
  size_t calls_ = 0;
  std::chrono::milliseconds latency_{0};
  std::mt19937_64 rng_;
};

// Trace capture (only in the standalone build; empty otherwise).
std::vector<std::string> TraceLines();
void ClearTrace();

}  // namespace xlive_test
