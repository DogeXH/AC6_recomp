// ac6net - lifecycle, identity, XNet address mapping, QoS and lobby RPC.
//
// One process-wide instance (Netplay::Get()). Tests may create extra instances directly to run
// several "consoles" in one process; everything instance-scoped hangs off a Netplay object.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ac6net/config.h"
#include "ac6net/types.h"

namespace ac6net {

namespace vnet {
class VNet;
}
namespace xlive {
class XLive;
}

class Overlay;   // src/overlay.h
class QosService;  // src/qos.h

// Virtual address range: 10.64.0.0/10.
inline constexpr uint32_t kVipBase = 0x0A400000;
inline constexpr uint32_t kVipMask = 0xFFC00000;
inline bool IsVip(uint32_t ip) { return (ip & kVipMask) == kVipBase; }
inline constexpr uint32_t kLoopback = 0x7F000001;

// XNADDR synthesis for a (vip, peer_id). See ARCHITECTURE.md section 3.
XnAddr MakeXnAddr(uint32_t vip, PeerId peer_id);
// Extracts vip (from ina_online, else ina) and peer_id (abOnline.xbox_id). Returns false if the
// address is not one of ours (vip outside the range).
bool ParseXnAddr(const XnAddr& addr, uint32_t* vip, PeerId* peer_id);
// Machine ID used for arbitration: 0xFA00000000000000 | MAC48 (MAC = ab_enet).
uint64_t MachineIdFromXnAddr(const XnAddr& addr);
// BLAKE2b-128 hex of the inputs (ARCHITECTURE.md section 8).
std::string ComputeCompatKey(uint32_t title_id, uint32_t media_id, uint32_t xex_version,
                             uint32_t protocol_version, const std::string& fps_mode);

inline constexpr uint32_t kProtocolVersion = 1;

struct Identity {
  bool valid = false;  // false until registered at least once (or loaded from cache)
  PeerId peer_id = 0;
  Xuid xuid = 0;       // online XUID (0x0009...)
  uint32_t vip = 0;    // host order
  std::string gamertag;
};

enum class NetState {
  kDisabled,    // netplay off
  kConnecting,  // socket open, HELLO in flight (identity may be cached)
  kOnline,      // registered with the server
  kOffline,     // server unreachable for > 30 s (identity still valid)
};

struct QosTargetResult {
  bool contacted = false;      // at least one probe answered
  bool data_received = false;  // the target is listening on that XNKID and returned data
  bool complete = false;       // all probes done (or timed out)
  uint16_t rtt_min_ms = 0;
  uint16_t rtt_med_ms = 0;
  uint32_t up_bps = 0;
  uint32_t dn_bps = 0;
  uint16_t probes_sent = 0;
  uint16_t probes_recv = 0;
  std::vector<uint8_t> data;
};

// Called on the network thread whenever a target's result changes; `all_done` is true on the
// final call for the lookup (every target complete).
using QosCallback = std::function<void(size_t index, const QosTargetResult& result, bool all_done)>;

struct RpcResult {
  bool transport_ok = false;  // false: timeout / not connected
  bool ok = false;            // server returned ok:true
  std::string err;            // server error code (PROTOCOL.md section 3) or "timeout"/"offline"
  std::string msg;
  std::string result_json;    // the "r" object, serialized
};

// Live-state change notifications for the glue (connection established/lost).
using StateCallback = std::function<void(NetState new_state)>;

class Netplay {
 public:
  Netplay();
  ~Netplay();
  Netplay(const Netplay&) = delete;
  Netplay& operator=(const Netplay&) = delete;

  // Process-wide instance used by the SDK glue.
  static Netplay& Get();

  // Loads/creates identity, opens the UDP socket, starts the network thread, sends HELLO.
  // Returns false (and stays disabled) if config.enabled is false or setup fails.
  bool Start(const Config& config);
  void Stop();

  bool enabled() const;
  NetState state() const;
  // Blocks until registered (kOnline) or timeout. Returns immediately true if a cached identity
  // exists and the state is kConnecting or kOnline.
  bool WaitRegistered(uint32_t timeout_ms);
  Identity identity() const;
  const Config& config() const;
  void SetStateCallback(StateCallback cb);

  // --- XNet ---
  // Own XNADDR (MakeXnAddr(identity)). If no identity, a loopback XNADDR (ina = 127.0.0.1).
  XnAddr LocalXnAddr() const;
  // Returns wsa::kOk and the vIP; registers vip -> peer_id. Own address -> own vip.
  int XnAddrToInAddr(const XnAddr& addr, const XnKid* kid, uint32_t* out_vip);
  // vIP -> XNADDR (own vip or 127.0.0.1 -> own XNADDR). May block up to 1 s for an
  // introduction if the vip is unknown. `out_kid` (optional) receives the current session XNKID
  // known for that peer, or zero.
  int InAddrToXnAddr(uint32_t vip, XnAddr* out_addr, XnKid* out_kid);
  // CSPRNG.
  void Random(uint8_t* out, size_t len);

  // --- QoS (see ARCHITECTURE.md 5.4) ---
  // flags are the guest XNET_QOS_LISTEN_* flags: 0x01 ENABLE, 0x02 DISABLE, 0x04 SET_DATA,
  // 0x08 SET_BITSPERSEC, 0x10 RELEASE.
  void QosListen(const XnKid& kid, const uint8_t* data, size_t len, uint32_t bps, uint32_t flags);
  // Starts a lookup; returns an id (> 0) or 0 on failure. Probes run over the overlay; targets
  // that cannot be resolved complete immediately with contacted = false.
  uint64_t QosLookup(const std::vector<XnAddr>& addrs, const std::vector<XnKid>& kids,
                     uint32_t probes, QosCallback cb);
  void QosCancel(uint64_t lookup_id);
  // Server-measured RTT and nominal bandwidth for XNetQosServiceLookup.
  bool QosServiceLookup(uint32_t* rtt_ms, uint32_t* up_bps, uint32_t* dn_bps);

  // --- Lobby RPC (blocking; never call from the network thread) ---
  RpcResult Call(const std::string& method, const std::string& args_json,
                 uint32_t timeout_ms = 6000);

  // --- Subsystems ---
  vnet::VNet& vnet();
  xlive::XLive& xlive();

  // Diagnostics JSON (state, identity, peers and their paths, counters).
  std::string StatusJson() const;

  // Internal: owned subsystems (defined in src/).
  Overlay& overlay();
  QosService& qos();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ac6net
