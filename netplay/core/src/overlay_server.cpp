// ac6net - overlay server channel (PROTOCOL.md 2.1-2.5): HELLO, C2S/S2C, keepalive, REHELLO,
// NOTICE, the RPC client (section 3) and introduction requests.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "ac6net/log.h"
#include "nlohmann/json.hpp"
#include "overlay.h"
#include "overlay_internal.h"
#include "platform.h"

namespace ac6net {

using json = nlohmann::json;
namespace lim = overlay_limits;

namespace {

constexpr uint64_t kLocalCandsRefreshMs = 30000;
constexpr size_t kMaxLocalCands = 8;

json ParseJson(const uint8_t* p, size_t n) {
  return json::parse(p, p + n, nullptr, /*allow_exceptions=*/false);
}

const std::string* JString(const json& obj, const char* key) {
  if (!obj.is_object()) return nullptr;
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return nullptr;
  return it->get_ptr<const std::string*>();
}

std::string JStringOr(const json& obj, const char* key, const std::string& fallback = {}) {
  const std::string* s = JString(obj, key);
  return s ? *s : fallback;
}

// ["ip", port] -> (ip, port). Returns false on any type or range error.
bool JIpPort(const json& v, std::string* ip, uint16_t* port) {
  if (!v.is_array() || v.size() < 2 || !v[0].is_string() || !v[1].is_number_unsigned()) {
    return false;
  }
  const uint64_t p = v[1].get<uint64_t>();
  if (p == 0 || p > 65535) return false;
  *ip = v[0].get<std::string>();
  *port = static_cast<uint16_t>(p);
  return true;
}

json AddrList(const std::vector<net::Addr>& addrs) {
  json arr = json::array();
  for (const auto& a : addrs) arr.push_back(json::array({a.Ip(), a.Port()}));
  return arr;
}

std::string Dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace

// --- Server address and timers ---

void Overlay::Impl::ResolveServer(uint64_t now) {
  net::Addr addr;
  std::string error;
  const bool ok = net::ResolveHostPort(cfg.server, &addr, &error);  // cfg is immutable here
  std::lock_guard<std::mutex> lock(mu);
  if (ok && addr.IsV6() && !sock6.valid()) {
    LogF(LogLevel::kError, "overlay: server %s is IPv6 but no IPv6 socket is open",
         cfg.server.c_str());
    next_resolve_ms = now + lim::kHelloMaxMs;
    return;
  }
  if (!ok) {
    LogF(LogLevel::kWarn, "overlay: %s; retrying in 5 s", error.c_str());
    next_resolve_ms = now + lim::kHelloMaxMs;
    return;
  }
  server_addr = addr;
  server_resolved = true;
  local_cands_valid = false;  // loopback candidate depends on the server address
  if (need_hello) next_hello_ms = now;
  LogF(LogLevel::kInfo, "overlay: server %s resolved to %s", cfg.server.c_str(),
       addr.ToString().c_str());
}

void Overlay::Impl::ScheduleHelloLocked(uint64_t now, uint64_t interval_ms) {
  // REHELLO and state changes are rate limited to one HELLO per second.
  const uint64_t earliest = last_hello_ms ? last_hello_ms + lim::kRehelloMinMs : now;
  const uint64_t when = std::max(now, earliest);
  if (!need_hello) {
    need_hello = true;
    hello_interval_ms = interval_ms;
    next_hello_ms = when;
  } else {
    next_hello_ms = std::min(next_hello_ms, when);
  }
}

void Overlay::Impl::ServerTimersLocked(uint64_t now, Events* ev) {
  // Offline after 30 s without any authenticated S2C. Keep trying with HELLO every 5 s and
  // re-resolve the server name in case its address changed.
  if ((state == NetState::kOnline || state == NetState::kConnecting) &&
      now - last_s2c_ms > lim::kServerOfflineMs) {
    SetStateLocked(NetState::kOffline, ev);
    ScheduleHelloLocked(now, lim::kHelloMaxMs);
    hello_interval_ms = lim::kHelloMaxMs;
    server_resolved = false;
    next_resolve_ms = now;
  }

  // RPC deadlines and retransmission.
  for (auto& [id, call] : rpc_calls) {
    if (call->done) continue;
    if (now >= call->deadline_ms) {
      RpcResult r;
      r.err = "timeout";
      r.msg = "no response from the lobby server";
      ++counters.rpc_timeouts;
      FinishCallLocked(*call, std::move(r));
      continue;
    }
    if (session_ok && !need_hello &&
        (!call->sent_once || now - call->last_sent_ms >= lim::kRpcRetransmitMs)) {
      if (call->sent_once) ++counters.rpc_retransmits;
      SendRpcFragmentsLocked(*call, now);
    }
  }

  if (!server_resolved) return;

  if (need_hello && now >= next_hello_ms) {
    SendHelloLocked(now);
    next_hello_ms = now + hello_interval_ms;
    hello_interval_ms = std::min(hello_interval_ms * 2, lim::kHelloMaxMs);
  }

  const uint64_t since_keepalive = now - last_keepalive_ms;
  if (session_ok && !need_hello &&
      (since_keepalive >= lim::kServerKeepaliveMs ||
       (check_server_soon && since_keepalive >= lim::kRelaySilenceCheckMs))) {
    SendKeepaliveLocked(now);
  }
  check_server_soon = false;
}

const std::vector<net::Addr>& Overlay::Impl::LocalCandidatesLocked(uint64_t now) {
  if (local_cands_valid && now - local_cands_ms < kLocalCandsRefreshMs) return local_cands;
  local_cands.clear();
  local_cands_ms = now;
  local_cands_valid = true;
  if (!cfg.lan || cfg.privacy) return local_cands;
  const uint16_t port = sock4.port();
  if (server_resolved && server_addr.IsLoopback()) {
    // Dev/test only: the server passes loopback candidates between loopback-registered peers.
    // [::1] also exercises the IPv6 socket on machines without global IPv6.
    net::Addr loop;
    if (net::Addr::FromIpPort("127.0.0.1", port, &loop)) local_cands.push_back(loop);
    if (sock6.valid() && net::Addr::FromIpPort("::1", port, &loop)) local_cands.push_back(loop);
  }
  // Windows: use the background scan (MaybeStartLocalScan) once it has run; until then, and on
  // POSIX (getifaddrs is cheap), enumerate here.
  const std::vector<net::Addr> scanned =
      local_scan_valid ? local_scan : net::LocalAddresses(true, sock6.valid());
  for (net::Addr a : scanned) {
    if (local_cands.size() >= kMaxLocalCands) break;
    net::Addr with_port;
    if (net::Addr::FromIpPort(a.Ip(), port, &with_port)) local_cands.push_back(with_port);
  }
  return local_cands;
}

void Overlay::Impl::MaybeStartLocalScan(uint64_t now) {
#if defined(_WIN32)
  if (!cfg.lan || cfg.privacy || scan_busy.load() || now < next_scan_ms) return;
  next_scan_ms = now + kLocalCandsRefreshMs;
  JoinLocalScan();  // the previous scan has finished (scan_busy is false)
  scan_busy = true;
  const bool want_v6 = sock6.valid();
  scan_thread = std::thread([this, want_v6] {
    platform::SetCurrentThreadName("ac6net-ifscan");
    std::vector<net::Addr> addrs = net::LocalAddresses(true, want_v6);
    {
      std::lock_guard<std::mutex> lock(mu);
      local_scan = std::move(addrs);
      local_scan_valid = true;
      local_cands_valid = false;  // rebuilt (cheaply) on the next HELLO / keepalive
    }
    scan_busy = false;
  });
#else
  (void)now;
#endif
}

void Overlay::Impl::JoinLocalScan() {
  if (scan_thread.joinable()) scan_thread.join();
}

// --- Sending ---

void Overlay::Impl::SendHelloLocked(uint64_t now) {
  json hello = {
      {"v", 1},
      {"client", cfg.client_version},
      {"gamertag", cfg.gamertag},
      {"title", cfg.title_id},
      {"compat", cfg.compat_key},
      {"local", AddrList(LocalCandidatesLocked(now))},
      {"privacy", cfg.privacy},
  };
  const std::string text = Dump(hello);
  if (wire::kHelloHeader + text.size() + crypto::kTagSize > wire::kMaxDatagram) {
    LogF(LogLevel::kError, "overlay: HELLO too large (%zu bytes)", text.size());
    return;
  }
  std::vector<uint8_t> pkt;
  pkt.reserve(wire::kHelloHeader + text.size() + crypto::kTagSize);
  wire::Writer w(&pkt);
  w.U8(wire::kMagic);
  w.U8(wire::kHello);
  w.Bytes(pk.data(), pk.size());
  w.Bytes(boot.data(), boot.size());
  const uint64_t ctr = ++c2s_ctr;
  w.U64(ctr);
  pkt.resize(wire::kHelloHeader + text.size() + crypto::kTagSize);
  crypto::Seal(c2s_key, ctr, pkt.data(), wire::kHelloHeader,
               reinterpret_cast<const uint8_t*>(text.data()), text.size(),
               pkt.data() + wire::kHelloHeader);
  SendRawLocked(server_addr, pkt.data(), pkt.size());
  ++counters.hello_sent;
  last_hello_ms = now;
}

bool Overlay::Impl::SendC2SLocked(uint8_t kind, const uint8_t* body, size_t len) {
  if (!session_ok || !server_resolved || !reg.valid) return false;
  const size_t inner_len = 1 + len;
  const size_t total = wire::kC2SHeader + inner_len + crypto::kTagSize;
  if (total > wire::kMaxDatagram) return false;
  uint8_t pkt[wire::kMaxDatagram];
  pkt[0] = wire::kMagic;
  pkt[1] = wire::kC2S;
  for (int i = 0; i < 8; ++i) pkt[2 + i] = static_cast<uint8_t>(reg.peer_id >> (8 * i));
  std::memcpy(pkt + 10, boot.data(), boot.size());
  const uint64_t ctr = ++c2s_ctr;
  for (int i = 0; i < 8; ++i) pkt[18 + i] = static_cast<uint8_t>(ctr >> (8 * i));
  uint8_t* inner = pkt + wire::kC2SHeader;
  inner[0] = kind;
  if (len) std::memcpy(inner + 1, body, len);
  crypto::Seal(c2s_key, ctr, pkt, wire::kC2SHeader, inner, inner_len, inner);
  return SendRawLocked(server_addr, pkt, total);
}

void Overlay::Impl::SendKeepaliveLocked(uint64_t now) {
  const std::string text = Dump(json{{"local", AddrList(LocalCandidatesLocked(now))}});
  SendC2SLocked(wire::kKeepalive, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  last_keepalive_ms = now;
  keepalive_sent_ms = now;
}

void Overlay::Impl::UpdateServerRttLocked(uint64_t sample_ms) {
  const uint32_t s = static_cast<uint32_t>(std::clamp<uint64_t>(sample_ms, 1, 60000));
  server_rtt_ms = server_rtt_ms == 0 ? s : (server_rtt_ms * 7 + s) / 8;
}

// --- Receiving ---

void Overlay::Impl::HandleHelloAckLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev) {
  if (n < wire::kHelloAckHeader + crypto::kTagSize) {
    ++counters.malformed;
    return;
  }
  BootId sb;
  std::memcpy(sb.data(), p + 2, sb.size());
  const uint64_t ctr = wire::LoadU64(p + 10);
  const bool same_boot = have_s_boot && sb == s_boot;
  const crypto::Key key =
      same_boot ? s2c_key : crypto::Kdf(server_dh, "AC6NET-S2C-1", {pk, server_pk, boot, sb});
  if (same_boot && !s2c_window.Check(ctr)) {
    ++counters.replays;
    return;
  }
  uint8_t plain[2048];
  const size_t plain_len = n - wire::kHelloAckHeader - crypto::kTagSize;
  if (!crypto::Open(key, ctr, p, wire::kHelloAckHeader, p + wire::kHelloAckHeader,
                    n - wire::kHelloAckHeader, plain)) {
    ++counters.auth_failures;
    return;
  }
  const json doc = ParseJson(plain, plain_len);
  OverlayRegistration nr;
  const std::string* peer_id = JString(doc, "peer_id");
  const std::string* xuid = JString(doc, "xuid");
  const std::string* vip = JString(doc, "vip");
  if (!peer_id || !U64FromHex(*peer_id, &nr.peer_id) || nr.peer_id == 0 || !xuid ||
      !U64FromHex(*xuid, &nr.xuid) || !vip || !Ipv4FromString(*vip, &nr.vip) || !IsVip(nr.vip)) {
    ++counters.malformed;
    LogF(LogLevel::kWarn, "overlay: HELLO_ACK with missing or invalid identity fields");
    return;
  }
  nr.valid = true;
  nr.gamertag = JStringOr(doc, "gamertag");
  nr.motd = JStringOr(doc, "motd");
  if (auto it = doc.find("public"); it != doc.end()) {
    JIpPort(*it, &nr.public_ip, &nr.public_port);
  }

  const bool server_restarted = have_s_boot && !same_boot;
  if (reg.valid && reg.peer_id != nr.peer_id && !peers.empty()) {
    // The server re-keyed us (it lost our account): every introduction was made under the old
    // identity and the peers know us by the old id. Start over.
    LogF(LogLevel::kWarn, "overlay: server assigned a new peer id; dropping %zu peer(s)",
         peers.size());
    std::vector<PeerId> ids;
    for (const auto& [id, peer] : peers) ids.push_back(id);
    for (PeerId id : ids) {
      const uint32_t vip = peers[id]->vip;
      RemovePeerLocked(id);
      if (vip) ev->lost.push_back(vip);
    }
  }
  if (!same_boot) {
    s_boot = sb;
    s2c_key = key;
    s2c_window.Reset();
    have_s_boot = true;
  }
  s2c_window.Accept(ctr);
  last_s2c_ms = now;
  ++counters.hello_acks;
  if (need_hello && last_hello_ms) UpdateServerRttLocked(now - last_hello_ms);

  const bool was_session = session_ok;
  const bool changed = !reg.valid || reg.peer_id != nr.peer_id || reg.xuid != nr.xuid ||
                       reg.vip != nr.vip || reg.gamertag != nr.gamertag;
  if (!changed && nr.public_ip.empty()) {
    nr.public_ip = reg.public_ip;
    nr.public_port = reg.public_port;
  }
  reg = nr;
  local_vip.store(reg.vip);
  session_ok = true;
  need_hello = false;
  hello_interval_ms = lim::kHelloFirstMs;
  online_this_start = true;
  SetStateLocked(NetState::kOnline, ev);
  state_cv.notify_all();
  if (changed || !was_session) ev->registrations.push_back(reg);
  if (was_session) return;

  LogF(LogLevel::kInfo, "overlay: registered as %s vip %s peer %s%s", reg.gamertag.c_str(),
       Ipv4ToString(reg.vip).c_str(), U64ToHex(reg.peer_id).c_str(),
       server_restarted ? " (server restarted)" : "");
  Trace("registered", "\"vip\":\"" + Ipv4ToString(reg.vip) + "\",\"peer_id\":\"" +
                          U64ToHex(reg.peer_id) + "\",\"gamertag\":\"" +
                          JsonEscape(reg.gamertag) + "\",\"server_restart\":" +
                          (server_restarted ? "true" : "false"));
  last_keepalive_ms = now;
  // Requests that waited for the session go out now.
  for (auto& [id, call] : rpc_calls)
    if (!call->done) SendRpcFragmentsLocked(*call, now);
  for (const auto& [id, req] : intro_reqs) SendIntroReqLocked(id, req);
  // The server forgot which pairs were introduced (relay authorisation): ask again.
  for (const auto& [id, peer] : peers) RequestIntroLocked(wire::kIntroByPeerId, id, now);
}

void Overlay::Impl::HandleS2CLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev) {
  if (n < wire::kS2CHeader + crypto::kTagSize + 1) {
    ++counters.malformed;
    return;
  }
  if (!have_s_boot || std::memcmp(p + 2, s_boot.data(), s_boot.size()) != 0) {
    ++counters.malformed;  // a stale server boot; a restarted server answers with REHELLO
    return;
  }
  const uint64_t ctr = wire::LoadU64(p + 10);
  if (!s2c_window.Check(ctr)) {
    ++counters.replays;
    return;
  }
  uint8_t plain[2048];
  const size_t plain_len = n - wire::kS2CHeader - crypto::kTagSize;
  if (!crypto::Open(s2c_key, ctr, p, wire::kS2CHeader, p + wire::kS2CHeader,
                    n - wire::kS2CHeader, plain)) {
    ++counters.auth_failures;
    return;
  }
  s2c_window.Accept(ctr);
  last_s2c_ms = now;
  if (state == NetState::kOffline && session_ok) SetStateLocked(NetState::kOnline, ev);

  wire::Reader r(plain, plain_len);
  uint8_t kind = 0;
  r.U8(&kind);
  switch (kind) {
    case wire::kRpcResp:
      HandleRpcRespLocked(r);
      break;
    case wire::kKeepaliveAck:
      HandleKeepaliveAckLocked(r.cursor(), r.remaining(), now);
      break;
    case wire::kIntro:
      HandleIntroLocked(r, now, ev);
      break;
    case wire::kIntroFail:
      HandleIntroFailLocked(r, now, ev);
      break;
    case wire::kNotice:
      HandleNoticeLocked(r.cursor(), r.remaining(), now);
      break;
    default:
      ++counters.malformed;
      break;
  }
}

void Overlay::Impl::HandleRehelloLocked(uint64_t now) {
  ++counters.rehello_recv;
  session_ok = false;
  ScheduleHelloLocked(now, lim::kHelloFirstMs);
  Trace("rehello");
}

void Overlay::Impl::HandleKeepaliveAckLocked(const uint8_t* p, size_t n, uint64_t now) {
  if (keepalive_sent_ms) {
    UpdateServerRttLocked(now - keepalive_sent_ms);
    keepalive_sent_ms = 0;
  }
  if (n == 0) return;
  const json doc = ParseJson(p, n);
  if (auto it = doc.is_object() ? doc.find("public") : doc.end(); it != doc.end()) {
    std::string ip;
    uint16_t port = 0;
    if (JIpPort(*it, &ip, &port)) {
      reg.public_ip = ip;
      reg.public_port = port;
    }
  }
}

void Overlay::Impl::HandleNoticeLocked(const uint8_t* p, size_t n, uint64_t now) {
  const json doc = ParseJson(p, n);
  const std::string code = JStringOr(doc, "code", "?");
  const std::string msg = JStringOr(doc, "msg");
  LogF(LogLevel::kWarn, "overlay: server notice [%s] %s", code.c_str(), msg.c_str());
  Trace("notice", "\"code\":\"" + JsonEscape(code) + "\",\"msg\":\"" + JsonEscape(msg) + "\"");
  if (code == "shutdown") {
    // The server is going away (likely restarting): its next boot will not know us, so start
    // re-registering now (1 s, 2 s, 4 s, then every 5 s) instead of waiting for a REHELLO.
    session_ok = false;
    last_hello_ms = std::max(last_hello_ms, now);  // first HELLO 1 s from now
    ScheduleHelloLocked(now, lim::kHelloFirstMs);
  }
}

// --- RPC ---

void Overlay::Impl::SendRpcFragmentsLocked(RpcCall& call, uint64_t now) {
  const auto count = static_cast<uint8_t>(call.fragments.size());
  std::vector<uint8_t> body;
  for (size_t i = 0; i < call.fragments.size(); ++i) {
    body.clear();
    wire::Writer w(&body);
    w.U32(call.req_id);
    w.U8(static_cast<uint8_t>(i));
    w.U8(count);
    w.Str(call.fragments[i]);
    SendC2SLocked(wire::kRpcReq, body.data(), body.size());
  }
  call.sent_once = true;
  call.last_sent_ms = now;
}

void Overlay::Impl::FinishCallLocked(RpcCall& call, RpcResult result) {
  if (call.done) return;
  call.done = true;
  call.result = std::move(result);
  call.cv.notify_all();
}

void Overlay::Impl::HandleRpcRespLocked(wire::Reader& r) {
  uint32_t req_id = 0;
  uint8_t idx = 0, count = 0;
  if (!r.U32(&req_id) || !r.U8(&idx) || !r.U8(&count) || count == 0 ||
      count > wire::kRpcMaxFragments || idx >= count) {
    ++counters.malformed;
    return;
  }
  auto it = rpc_calls.find(req_id);
  if (it == rpc_calls.end() || it->second->done) return;  // late duplicate
  RpcCall& call = *it->second;
  if (call.resp_count != count) {
    call.resp_count = count;
    call.resp_parts.assign(count, std::string());
    call.resp_have.assign(count, false);
    call.resp_received = 0;
  }
  if (call.resp_have[idx]) return;
  call.resp_have[idx] = true;
  call.resp_parts[idx].assign(reinterpret_cast<const char*>(r.cursor()), r.remaining());
  if (++call.resp_received < count) return;

  std::string text;
  for (const auto& part : call.resp_parts) text += part;
  RpcResult result;
  result.transport_ok = true;
  const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    result.err = "internal";
    result.msg = "malformed RPC response";
    ++counters.malformed;
  } else {
    auto ok_it = doc.find("ok");
    result.ok = ok_it != doc.end() && ok_it->is_boolean() && ok_it->get<bool>();
    result.err = JStringOr(doc, "err");
    result.msg = JStringOr(doc, "msg");
    if (auto r_it = doc.find("r"); r_it != doc.end()) {
      result.result_json = Dump(*r_it);
    } else if (result.ok) {
      result.result_json = "{}";
    }
    if (!result.ok && result.err.empty()) result.err = "internal";
  }
  FinishCallLocked(call, std::move(result));
}

RpcResult Overlay::Call(const std::string& method, const std::string& args_json,
                        uint32_t timeout_ms) {
  Impl& m = *impl_;
  RpcResult res;
  const std::string args = args_json.empty() ? std::string("{}") : args_json;
  if (method.empty() || !json::accept(args) || args.front() != '{') {
    res.transport_ok = false;
    res.err = "bad_request";
    res.msg = "method missing or arguments not a JSON object";
    return res;
  }
  std::unique_lock<std::mutex> lock(m.mu);
  if (m.OnNetworkThread()) {
    res.err = "internal";
    res.msg = "Call() on the network thread";
    return res;
  }
  if (!m.running || m.state != NetState::kOnline) {
    res.err = "offline";
    res.msg = "not connected to the lobby server";
    return res;
  }
  const uint32_t req_id = m.next_req_id;
  m.next_req_id = (m.next_req_id + 1) & 0x7FFFFFFFu;
  if (m.next_req_id == 0) m.next_req_id = 1;

  const std::string text = "{\"id\":" + std::to_string(req_id) + ",\"m\":\"" +
                           JsonEscape(method) + "\",\"a\":" + args + "}";
  if (text.size() > wire::kRpcFragment * wire::kRpcMaxFragments) {
    res.err = "bad_request";
    res.msg = "request too large";
    return res;
  }
  auto call = std::make_shared<RpcCall>();
  call->req_id = req_id;
  for (size_t off = 0; off < text.size(); off += wire::kRpcFragment) {
    call->fragments.push_back(text.substr(off, wire::kRpcFragment));
  }
  const uint64_t now = OverlayNowMs();
  call->deadline_ms = now + timeout_ms;
  m.rpc_calls[req_id] = call;
  ++m.counters.rpc_calls;
  if (m.session_ok && !m.need_hello) m.SendRpcFragmentsLocked(*call, now);

  // A little slack past the deadline: the network thread normally completes it as "timeout".
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms + 50);
  call->cv.wait_until(lock, until, [&] { return call->done; });
  if (!call->done) {
    ++m.counters.rpc_timeouts;
    RpcResult r;
    r.err = "timeout";
    r.msg = "no response from the lobby server";
    m.FinishCallLocked(*call, std::move(r));
  }
  res = call->result;
  m.rpc_calls.erase(req_id);
  return res;
}

// --- Introductions ---

bool Overlay::Impl::IntroInFlightLocked(uint8_t by, uint64_t value) const {
  for (const auto& [id, req] : intro_reqs)
    if (req.by == by && req.value == value) return true;
  return false;
}

bool Overlay::Impl::RequestIntroLocked(uint8_t by, uint64_t value, uint64_t now) {
  if (IntroInFlightLocked(by, value)) return true;
  if (intro_reqs.size() >= lim::kMaxIntroRequests) return false;
  if (auto b = intro_backoff.find({by, value}); b != intro_backoff.end()) {
    if (now < b->second) return false;
    intro_backoff.erase(b);
  }
  const uint32_t id = next_intro_id;
  next_intro_id = (next_intro_id + 1) & 0x7FFFFFFFu;
  if (next_intro_id == 0) next_intro_id = 1;
  IntroRequest req{by, value, now, now};
  intro_reqs[id] = req;
  ++counters.intro_reqs;
  SendIntroReqLocked(id, req);
  return true;
}

void Overlay::Impl::SendIntroReqLocked(uint32_t req_id, const IntroRequest& req) {
  if (!session_ok || need_hello) return;  // resent by IntroTimersLocked / on HELLO_ACK
  std::vector<uint8_t> body;
  wire::Writer w(&body);
  w.U32(req_id);
  w.U8(req.by);
  if (req.by == wire::kIntroByVip) {
    w.U32BE(static_cast<uint32_t>(req.value));
  } else {
    w.U64(req.value);
  }
  SendC2SLocked(wire::kIntroReq, body.data(), body.size());
}

void Overlay::Impl::FailIntroLocked(const IntroRequest& req, uint64_t now) {
  // A guest that keeps sending to a dead vIP must not turn every send into an INTRO_REQ.
  if (intro_backoff.size() < lim::kMaxIntroBackoff) {
    intro_backoff[{req.by, req.value}] = now + lim::kIntroFailBackoffMs;
  }
  if (req.by == wire::kIntroByVip) {
    auto it = pending.find(static_cast<uint32_t>(req.value));
    if (it != pending.end()) {
      counters.data_dropped += it->second.items.size();
      pending.erase(it);
    }
  } else {
    const auto before = unknown.size();
    std::erase_if(unknown, [&](const UnknownPacket& u) { return u.src == req.value; });
    counters.unknown_dropped += before - unknown.size();
  }
}

void Overlay::Impl::IntroTimersLocked(uint64_t now) {
  std::erase_if(intro_backoff, [now](const auto& e) { return now >= e.second; });
  bool changed = false;
  for (auto it = intro_reqs.begin(); it != intro_reqs.end();) {
    IntroRequest& req = it->second;
    if (now - req.first_ms >= lim::kIntroGiveUpMs) {
      ++counters.intro_fails;
      FailIntroLocked(req, now);
      it = intro_reqs.erase(it);
      changed = true;
      continue;
    }
    if (now - req.last_ms >= lim::kIntroRetryMs) {
      req.last_ms = now;
      SendIntroReqLocked(it->first, req);
    }
    ++it;
  }
  if (changed) intro_cv.notify_all();
}

void Overlay::Impl::HandleIntroFailLocked(wire::Reader& r, uint64_t now, Events* ev) {
  uint32_t req_id = 0;
  uint8_t reason = 0;
  if (!r.U32(&req_id) || !r.U8(&reason)) {
    ++counters.malformed;
    return;
  }
  auto it = intro_reqs.find(req_id);
  if (it == intro_reqs.end()) return;
  const IntroRequest req = it->second;
  intro_reqs.erase(it);
  ++counters.intro_fails;
  LogF(LogLevel::kInfo, "overlay: introduction to %s %s failed (reason %u)",
       req.by == wire::kIntroByVip ? "vip" : "peer",
       req.by == wire::kIntroByVip ? Ipv4ToString(static_cast<uint32_t>(req.value)).c_str()
                                   : U64ToHex(req.value).c_str(),
       reason);
  FailIntroLocked(req, now);
  // The server no longer knows this peer id (its account is gone): the record is stale.
  if (reason == 1 && req.by == wire::kIntroByPeerId) {
    if (Peer* peer = FindPeerLocked(req.value)) {
      const uint32_t vip = peer->vip;
      ++counters.peers_lost;
      RemovePeerLocked(req.value);
      if (vip) ev->lost.push_back(vip);
    }
  }
  intro_cv.notify_all();
}

void Overlay::Impl::HandleIntroLocked(wire::Reader& r, uint64_t now, Events* ev) {
  uint32_t req_id = 0;
  if (!r.U32(&req_id)) {
    ++counters.malformed;
    return;
  }
  const json doc = ParseJson(r.cursor(), r.remaining());
  PeerId peer_id = 0;
  Xuid xuid = 0;
  uint32_t vip = 0;
  crypto::PublicKey peer_pk{};
  BootId peer_boot{};
  const std::string* s_peer = JString(doc, "peer_id");
  const std::string* s_vip = JString(doc, "vip");
  const std::string* s_pk = JString(doc, "pk");
  const std::string* s_boot_hex = JString(doc, "boot");
  if (!s_peer || !U64FromHex(*s_peer, &peer_id) || peer_id == 0 || !s_vip ||
      !Ipv4FromString(*s_vip, &vip) || !IsVip(vip) || !s_pk ||
      !HexDecode(*s_pk, peer_pk.data(), peer_pk.size()) || !s_boot_hex ||
      !HexDecode(*s_boot_hex, peer_boot.data(), peer_boot.size())) {
    ++counters.malformed;
    LogF(LogLevel::kWarn, "overlay: INTRO with missing or invalid fields");
    return;
  }
  if (const std::string* s_xuid = JString(doc, "xuid")) U64FromHex(*s_xuid, &xuid);
  if (peer_id == reg.peer_id || vip == reg.vip) {
    ++counters.malformed;
    return;
  }
  Peer* peer = UpsertPeerLocked(peer_id, vip, xuid, JStringOr(doc, "gamertag"), peer_pk,
                                peer_boot, now, ev);
  if (!peer) return;

  size_t merged = 0;
  if (auto c_it = doc.find("cands"); c_it != doc.end() && c_it->is_array()) {
    for (const json& c : *c_it) {
      if (merged >= lim::kMaxCandidates) break;
      std::string ip;
      uint16_t port = 0;
      if (!JIpPort(c, &ip, &port) || c.size() < 3 || !c[2].is_string()) continue;
      const std::string& type_s = c[2].get_ref<const std::string&>();
      CandType type;
      if (type_s == "lan") type = CandType::kLan;
      else if (type_s == "v6") type = CandType::kV6;
      else if (type_s == "srflx") type = CandType::kSrflx;
      else if (type_s == "prflx") type = CandType::kPrflx;
      else continue;
      net::Addr addr;
      if (!net::Addr::FromIpPort(ip, port, &addr)) continue;
      if (addr.IsV6() && !sock6.valid()) continue;
      MergeCandidateLocked(*peer, addr, type);
      ++merged;
    }
  }
  ++counters.intros_recv;
  Trace("intro", "\"vip\":\"" + Ipv4ToString(vip) + "\",\"peer_id\":\"" + U64ToHex(peer_id) +
                     "\",\"cands\":" + std::to_string(peer->cands.size()) +
                     ",\"solicited\":" + (req_id ? "true" : "false"));

  for (auto it = intro_reqs.begin(); it != intro_reqs.end();) {
    const IntroRequest& q = it->second;
    const bool match = it->first == req_id || (q.by == wire::kIntroByVip && q.value == vip) ||
                       (q.by == wire::kIntroByPeerId && q.value == peer_id);
    it = match ? intro_reqs.erase(it) : std::next(it);
  }
  intro_cv.notify_all();

  peer->probe_phase_ms = now;
  if (direct_allowed) ProbeAllLocked(*peer, now);
  FlushPendingLocked(*peer, now);
  // `peer` may be removed while replaying (a buffered BYE); do not use it afterwards.
  ReplayUnknownLocked(peer_id, now, ev);
  // A C2S with a later ctr tells the server this INTRO arrived (it stops resending).
  SendKeepaliveLocked(now);
}

}  // namespace ac6net
