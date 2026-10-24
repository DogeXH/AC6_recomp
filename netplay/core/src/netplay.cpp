// ac6net - Netplay: lifecycle, identity, XNet address mapping, QoS and RPC front end.
//
// Netplay::Impl owns the Overlay (transport) and the subsystems that ride on it:
//   overlay channels 1-3 -> vnet::VNet::OnLinkData, channel 4 -> QosService::OnLinkData,
//   peer lost -> both, network-thread tick -> VNet::Tick + QosService::Tick.
#include "ac6net/netplay.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "ac6net/log.h"
#include "ac6net/vnet.h"
#include "ac6net/xlive.h"
#include "crypto.h"
#include "identity.h"
#include "nlohmann/json.hpp"
#include "overlay.h"
#include "qos.h"

namespace ac6net {

using json = nlohmann::json;

namespace {

constexpr uint32_t kIntroWaitMs = 1000;  // InAddrToXnAddr for an unknown vIP
constexpr uint32_t kNominalBps = 10'000'000;

const char* StateName(NetState s) {
  switch (s) {
    case NetState::kDisabled: return "disabled";
    case NetState::kConnecting: return "connecting";
    case NetState::kOnline: return "online";
    case NetState::kOffline: return "offline";
  }
  return "?";
}

std::string Hex16(uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
  return HexEncode(b, sizeof(b));
}

std::string ToLower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// Loopback XNADDR reported while there is no identity (ARCHITECTURE.md 5.4: offline).
XnAddr LoopbackXnAddr() {
  XnAddr a;
  a.ina = kLoopback;
  a.ab_enet = {0x00, 0x22, 0x48, 0x00, 0x00, 0x01};
  return a;
}

}  // namespace

struct Netplay::Impl {
  // Declared first so it is destroyed last: vnet and qos keep a pointer to it.
  Overlay overlay;
  std::unique_ptr<vnet::VNet> vnet;
  std::unique_ptr<QosService> qos;
  std::unique_ptr<xlive::XLive> xlive;

  std::mutex lifecycle_mu;  // serialises Start/Stop
  bool started = false;
  bool owns_trace = false;
  Config config;
  IdentityFile id_file;

  mutable std::mutex mu;  // guards everything below
  Identity identity;
  StateCallback state_cb;
  std::unordered_map<uint32_t, PeerId> vip_peer;  // learned from XNADDRs
  std::unordered_map<uint32_t, XnKid> vip_kid;    // session XNKID seen with that vIP

  void OnRegistered(const OverlayRegistration& reg);
};

void Netplay::Impl::OnRegistered(const OverlayRegistration& reg) {
  IdentityFile to_save;
  bool save = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    identity.valid = true;
    identity.peer_id = reg.peer_id;
    identity.xuid = reg.xuid;
    identity.vip = reg.vip;
    identity.gamertag = reg.gamertag;
    const std::string key = ToLower(config.server_key_hex);
    if (!id_file.has_registration || id_file.server_key_hex != key ||
        id_file.peer_id != reg.peer_id || id_file.xuid != reg.xuid || id_file.vip != reg.vip ||
        id_file.gamertag != reg.gamertag) {
      id_file.has_registration = true;
      id_file.server_key_hex = key;
      id_file.peer_id = reg.peer_id;
      id_file.xuid = reg.xuid;
      id_file.vip = reg.vip;
      id_file.gamertag = reg.gamertag;
      to_save = id_file;
      save = true;
    }
  }
  if (save) {
    std::string error;
    if (!SaveIdentity(config.data_dir, to_save, &error)) {
      LogF(LogLevel::kWarn, "netplay: cannot save identity: %s", error.c_str());
    }
    crypto::Wipe(to_save.sk.data(), to_save.sk.size());
  }
}

Netplay::Netplay() : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  m.vnet = std::make_unique<vnet::VNet>(&m.overlay);
  m.qos = std::make_unique<QosService>(&m.overlay);
  // Constructed after impl_ is set: XLive may call back into this Netplay.
  m.xlive = std::make_unique<xlive::XLive>(this);

  m.overlay.SetDataHandler([&m](uint32_t vip, Channel ch, const uint8_t* data, size_t len) {
    if (ch == Channel::kQos) {
      m.qos->OnLinkData(vip, data, len);
    } else {
      m.vnet->OnLinkData(vip, static_cast<uint8_t>(ch), data, len);
    }
  });
  m.overlay.SetPeerLostHandler([&m](uint32_t vip) {
    m.vnet->OnPeerLost(vip);
    m.qos->OnPeerLost(vip);
  });
  m.overlay.SetTickHandler([&m](uint64_t now_ms) {
    m.vnet->Tick(now_ms);
    m.qos->Tick(now_ms);
  });
  m.overlay.SetStateHandler([&m](NetState s) {
    StateCallback cb;
    {
      std::lock_guard<std::mutex> lock(m.mu);
      cb = m.state_cb;
    }
    if (cb) cb(s);
  });
  m.overlay.SetRegisteredHandler([&m](const OverlayRegistration& reg) { m.OnRegistered(reg); });
}

Netplay::~Netplay() { Stop(); }

Netplay& Netplay::Get() {
  static Netplay instance;
  return instance;
}

bool Netplay::Start(const Config& config) {
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lifecycle(m.lifecycle_mu);
  if (m.started) {
    LogF(LogLevel::kWarn, "netplay: Start called twice; ignoring");
    return false;
  }
  if (!config.enabled) return false;
  uint8_t key[32];
  if (!HexDecode(config.server_key_hex, key, sizeof(key))) {
    LogF(LogLevel::kError, "netplay: server_key must be 64 hex characters; netplay disabled");
    return false;
  }
  m.config = config;
  m.owns_trace = false;
  if (!config.trace_path.empty()) {
    if (SetTracePath(config.trace_path)) {
      m.owns_trace = true;
    } else {
      LogF(LogLevel::kWarn, "netplay: cannot open trace file %s", config.trace_path.c_str());
    }
  }

  std::string error;
  IdentityFile id;
  bool may_save = true;
  if (!LoadOrCreateIdentity(config.data_dir, &id, &error, &may_save)) {
    LogF(LogLevel::kWarn, "netplay: identity not persisted (%s); using it for this run only",
         error.c_str());
  }
  if (!may_save) m.config.data_dir.clear();  // never overwrite the identity file we could not read
  OverlayRegistration cached;
  Identity identity;
  if (id.RegistrationMatches(config.server_key_hex)) {
    cached.valid = true;
    cached.peer_id = id.peer_id;
    cached.xuid = id.xuid;
    cached.vip = id.vip;
    cached.gamertag = id.gamertag;
    identity = {true, id.peer_id, id.xuid, id.vip, id.gamertag};
  }
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.id_file = id;
    m.identity = identity;
    m.vip_peer.clear();
    m.vip_kid.clear();
  }
  const bool ok = m.overlay.Start(config, id.sk, cached.valid ? &cached : nullptr);
  crypto::Wipe(id.sk.data(), id.sk.size());
  if (!ok) {
    if (m.owns_trace) SetTracePath("");
    m.owns_trace = false;
    return false;
  }
  m.started = true;
  Trace("netplay_start",
        "\"cached_identity\":" + std::string(identity.valid ? "true" : "false") +
            ",\"force_relay\":" + (config.force_relay ? "true" : "false") +
            ",\"port\":" + std::to_string(m.overlay.local_port()) +
            // Wall clock at this trace's t_ms, so traces from two machines can be aligned.
            ",\"unix_ms\":" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count()));
  return true;
}

void Netplay::Stop() {
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lifecycle(m.lifecycle_mu);
  if (!m.started) return;
  m.vnet->CloseAll();  // resets go out while the overlay can still send them
  m.overlay.Stop();
  Trace("netplay_stop");
  if (m.owns_trace) SetTracePath("");
  m.owns_trace = false;
  m.started = false;
  std::lock_guard<std::mutex> lock(m.mu);
  crypto::Wipe(m.id_file.sk.data(), m.id_file.sk.size());
}

bool Netplay::enabled() const { return impl_->overlay.running(); }

NetState Netplay::state() const { return impl_->overlay.state(); }

bool Netplay::WaitRegistered(uint32_t timeout_ms) {
  Impl& m = *impl_;
  if (!m.overlay.running()) return false;
  const NetState s = m.overlay.state();
  if (identity().valid && (s == NetState::kConnecting || s == NetState::kOnline)) return true;
  return m.overlay.WaitOnline(timeout_ms);
}

Identity Netplay::identity() const {
  // The overlay's registration is updated under its lock before WaitOnline() wakes, so a caller
  // returning from WaitRegistered() always sees it; Impl::identity (the cached copy, updated by
  // the registered handler a moment later) is only the fallback before the first Start().
  const OverlayRegistration reg = impl_->overlay.registration();
  if (reg.valid) return Identity{true, reg.peer_id, reg.xuid, reg.vip, reg.gamertag};
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->identity;
}

const Config& Netplay::config() const { return impl_->config; }

void Netplay::SetStateCallback(StateCallback cb) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->state_cb = std::move(cb);
}

XnAddr Netplay::LocalXnAddr() const {
  const Identity id = identity();
  return id.valid ? MakeXnAddr(id.vip, id.peer_id) : LoopbackXnAddr();
}

int Netplay::XnAddrToInAddr(const XnAddr& addr, const XnKid* kid, uint32_t* out_vip) {
  if (!out_vip) return wsa::kFault;
  Impl& m = *impl_;
  const Identity me = identity();
  uint32_t vip = 0;
  PeerId peer_id = 0;
  if (!ParseXnAddr(addr, &vip, &peer_id)) {
    // Our own loopback XNADDR (no identity yet) maps to loopback.
    if (addr == LoopbackXnAddr() || (addr.ina == kLoopback && addr.ina_online == 0)) {
      *out_vip = me.valid ? me.vip : kLoopback;
      return wsa::kOk;
    }
    return wsa::kInval;
  }
  if (me.valid && vip == me.vip) {
    *out_vip = vip;
    return wsa::kOk;
  }
  {
    std::lock_guard<std::mutex> lock(m.mu);
    if (peer_id) m.vip_peer[vip] = peer_id;
    if (kid && !kid->IsZero()) m.vip_kid[vip] = *kid;
  }
  *out_vip = vip;
  return wsa::kOk;
}

int Netplay::InAddrToXnAddr(uint32_t vip, XnAddr* out_addr, XnKid* out_kid) {
  if (!out_addr) return wsa::kFault;
  Impl& m = *impl_;
  if (out_kid) *out_kid = XnKid{};
  const Identity me = identity();
  if (vip == kLoopback || (me.valid && vip == me.vip)) {
    *out_addr = LocalXnAddr();
    return wsa::kOk;
  }
  if (!IsVip(vip)) return wsa::kInval;
  PeerId peer_id = 0;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    if (auto it = m.vip_peer.find(vip); it != m.vip_peer.end()) {
      peer_id = it->second;
      found = true;
    }
  }
  if (!found) found = m.overlay.PeerIdForVip(vip, &peer_id);
  if (!found) found = m.overlay.ResolveVip(vip, kIntroWaitMs, &peer_id);
  if (!found) return wsa::kInval;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.vip_peer[vip] = peer_id;
    if (out_kid) {
      if (auto it = m.vip_kid.find(vip); it != m.vip_kid.end()) *out_kid = it->second;
    }
  }
  *out_addr = MakeXnAddr(vip, peer_id);
  return wsa::kOk;
}

void Netplay::Random(uint8_t* out, size_t len) { crypto::RandomBytes(out, len); }

void Netplay::QosListen(const XnKid& kid, const uint8_t* data, size_t len, uint32_t bps,
                        uint32_t flags) {
  impl_->qos->Listen(kid, data, len, bps, flags);
}

uint64_t Netplay::QosLookup(const std::vector<XnAddr>& addrs, const std::vector<XnKid>& kids,
                            uint32_t probes, QosCallback cb) {
  std::vector<uint32_t> vips;
  vips.reserve(addrs.size());
  for (size_t i = 0; i < addrs.size(); ++i) {
    uint32_t vip = 0;
    // Records vip -> peer_id like XNetXnAddrToInAddr; anything unresolvable becomes 0.
    if (XnAddrToInAddr(addrs[i], i < kids.size() ? &kids[i] : nullptr, &vip) != wsa::kOk) {
      vip = 0;
    }
    vips.push_back(vip);
  }
  return impl_->qos->Lookup(vips, kids, probes, std::move(cb));
}

void Netplay::QosCancel(uint64_t lookup_id) { impl_->qos->Cancel(lookup_id); }

bool Netplay::QosServiceLookup(uint32_t* rtt_ms, uint32_t* up_bps, uint32_t* dn_bps) {
  if (impl_->overlay.state() != NetState::kOnline) return false;
  const uint32_t rtt = impl_->overlay.ServerRttMs();
  if (rtt_ms) *rtt_ms = rtt ? rtt : 1;
  if (up_bps) *up_bps = kNominalBps;
  if (dn_bps) *dn_bps = kNominalBps;
  return true;
}

RpcResult Netplay::Call(const std::string& method, const std::string& args_json,
                        uint32_t timeout_ms) {
  return impl_->overlay.Call(method, args_json, timeout_ms);
}

vnet::VNet& Netplay::vnet() { return *impl_->vnet; }

xlive::XLive& Netplay::xlive() { return *impl_->xlive; }

Overlay& Netplay::overlay() { return impl_->overlay; }

QosService& Netplay::qos() { return *impl_->qos; }

std::string Netplay::StatusJson() const {
  const Impl& m = *impl_;
  const Identity id = identity();
  json doc = json::object();
  doc["enabled"] = m.overlay.running();
  doc["state"] = StateName(m.overlay.state());
  json ident = {{"valid", id.valid}};
  if (id.valid) {
    ident["peer_id"] = Hex16(id.peer_id);
    ident["xuid"] = Hex16(id.xuid);
    ident["vip"] = Ipv4ToString(id.vip);
    ident["gamertag"] = id.gamertag;
  }
  doc["identity"] = std::move(ident);
  json overlay = json::parse(m.overlay.StatusJson(), nullptr, /*allow_exceptions=*/false);
  doc["overlay"] = overlay.is_discarded() ? json::object() : std::move(overlay);
  const vnet::Stats s = m.vnet->stats();
  doc["vnet"] = {
      {"dgrams_sent", s.dgrams_sent},       {"dgrams_recv", s.dgrams_recv},
      {"dgrams_dropped", s.dgrams_dropped}, {"stream_bytes_sent", s.stream_bytes_sent},
      {"stream_bytes_recv", s.stream_bytes_recv}, {"sockets_open", s.sockets_open},
      {"streams_open", s.streams_open},
  };
  return doc.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace ac6net
