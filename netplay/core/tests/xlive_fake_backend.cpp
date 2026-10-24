// In-memory lobby and XLiveBackend for the xlive unit tests (see xlive_fake_backend.h).
#include "xlive_fake_backend.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <set>

namespace xlive_test {

using ac6net::RpcResult;

namespace {

std::string Hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64, v);
  return buf;
}

uint64_t ParseHex16(const json& v) {
  const std::string s = v.get<std::string>();
  if (s.size() != 16) throw std::runtime_error("bad hex");
  return std::stoull(s, nullptr, 16);
}

RpcResult Ok(json r) {
  RpcResult res;
  res.transport_ok = true;
  res.ok = true;
  res.result_json = r.dump();
  return res;
}

RpcResult Err(const std::string& code, const std::string& msg = {}) {
  RpcResult res;
  res.transport_ok = true;
  res.ok = false;
  res.err = code;
  res.msg = msg;
  return res;
}

std::string VipText(uint32_t vip) {
  return std::to_string(vip >> 24) + "." + std::to_string((vip >> 16) & 0xFF) + "." +
         std::to_string((vip >> 8) & 0xFF) + "." + std::to_string(vip & 0xFF);
}

}  // namespace

uint64_t MachineIdOf(uint32_t vip) {
  return 0xFA00000000000000ull | (0x002248ull << 24) | (vip & 0xFFFFFF);
}

// --- FakeLobby ---

void FakeLobby::Register(const FakeClient& client) {
  std::lock_guard lk(mu_);
  clients_[client.peer_id] = client;
}

RpcResult FakeLobby::Handle(const FakeClient& who, const std::string& method,
                            const std::string& args_json) {
  std::lock_guard lk(mu_);
  json args = json::parse(args_json, nullptr, false);
  calls_.push_back(
      {who.peer_id, method, args, std::chrono::steady_clock::now(), std::this_thread::get_id()});
  if (auto it = drop_next_.find(method); it != drop_next_.end() && it->second > 0) {
    --it->second;
    RpcResult r;
    r.err = "timeout";
    return r;
  }
  if (auto it = fail_next_.find(method); it != fail_next_.end()) {
    const std::string err = it->second;
    fail_next_.erase(it);
    return Err(err, "injected");
  }
  if (auto it = overrides_.find(method); it != overrides_.end() && it->second) {
    return it->second();
  }
  if (args.is_discarded() || !args.is_object()) return Err("bad_request", "not an object");
  try {
    return Dispatch(who, method, args);
  } catch (const std::exception& e) {
    return Err("bad_request", e.what());
  }
}

json FakeLobby::Doc(const Session& s) const {
  const FakeClient& host = clients_.at(s.host);
  uint32_t filled_public = 0, filled_private = 0;
  json members = json::array();
  for (const auto& m : s.members) {
    (m.is_private ? filled_private : filled_public)++;
    members.push_back({{"xuid", Hex16(m.xuid)}, {"private", m.is_private}});
  }
  return {{"session_id", Hex16(s.id)},
          {"key", s.key_hex},
          {"title", s.title},
          {"flags", s.flags},
          {"state", s.state},
          {"public_slots", s.public_slots},
          {"private_slots", s.private_slots},
          {"filled_public", filled_public},
          {"filled_private", filled_private},
          {"host",
           {{"peer_id", Hex16(host.peer_id)},
            {"vip", VipText(host.vip)},
            {"xuid", Hex16(host.xuid)},
            {"gamertag", host.gamertag}}},
          {"contexts", s.contexts},
          {"properties", s.properties},
          {"members", members}};
}

RpcResult FakeLobby::Dispatch(const FakeClient& who, const std::string& method, const json& a) {
  const auto session_of = [&](bool host_only) -> Session* {
    const uint64_t id = ParseHex16(a.at("session_id"));
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return nullptr;
    if (host_only && it->second.host != who.peer_id) throw std::logic_error("not_host");
    return &it->second;
  };
  const auto guarded = [&](bool host_only, auto&& fn) -> RpcResult {
    try {
      Session* s = session_of(host_only);
      if (!s) return Err("not_found");
      return fn(*s);
    } catch (const std::logic_error& e) {
      if (std::string(e.what()) == "not_host") return Err("not_host");
      throw;
    }
  };

  if (method == "session.create") {
    Session s;
    s.id = 0x8000000000000000ull | (rng_() & 0x00FFFFFFFFFFFFFFull);
    s.key_hex = Hex16(rng_()) + Hex16(rng_());
    s.flags = a.at("flags").get<uint32_t>();
    s.public_slots = a.at("public_slots").get<uint32_t>();
    s.private_slots = a.at("private_slots").get<uint32_t>();
    s.title = a.at("title").get<std::string>();
    s.compat = a.at("compat").get<std::string>();
    s.contexts = a.at("contexts");
    s.properties = a.at("properties");
    s.host = who.peer_id;
    s.seq = ++seq_;
    s.nonce = rng_() | 1;
    sessions_[s.id] = s;
    return Ok({{"session_id", Hex16(s.id)}, {"key", s.key_hex}, {"nonce", Hex16(s.nonce)}});
  }
  if (method == "session.get") {
    return guarded(false, [&](Session& s) {
      json doc = Doc(s);
      doc["nonce"] = Hex16(s.nonce);  // the host's arbitration nonce, for joiners
      return Ok(doc);
    });
  }
  if (method == "session.attrs") {
    return guarded(true, [&](Session& s) {
      s.contexts = a.at("contexts");
      s.properties = a.at("properties");
      return Ok(json::object());
    });
  }
  if (method == "session.modify") {
    return guarded(true, [&](Session& s) {
      s.flags = a.at("flags").get<uint32_t>();
      s.public_slots = a.at("public_slots").get<uint32_t>();
      s.private_slots = a.at("private_slots").get<uint32_t>();
      return Ok(json::object());
    });
  }
  if (method == "session.join") {
    return guarded(false, [&](Session& s) {
      const bool host = s.host == who.peer_id;
      std::vector<Member> add;
      for (const auto& m : a.at("members")) {
        Member mem{ParseHex16(m.at("xuid")), m.at("private").get<bool>()};
        if (!host && mem.xuid != who.xuid) return Err("not_host", "non-host joined another xuid");
        const bool present = std::any_of(s.members.begin(), s.members.end(),
                                         [&](const Member& x) { return x.xuid == mem.xuid; });
        if (!present) add.push_back(mem);
      }
      uint32_t pub = 0, priv = 0;
      for (const auto& m : s.members) (m.is_private ? priv : pub)++;
      for (const auto& m : add) (m.is_private ? priv : pub)++;
      if (pub > s.public_slots || priv > s.private_slots) return Err("full");
      s.members.insert(s.members.end(), add.begin(), add.end());
      return Ok({{"filled_public", pub}, {"filled_private", priv}});
    });
  }
  if (method == "session.leave") {
    return guarded(false, [&](Session& s) {
      const bool host = s.host == who.peer_id;
      for (const auto& x : a.at("xuids")) {
        const Xuid xuid = ParseHex16(x);
        if (!host && xuid != who.xuid) return Err("not_host");
        std::erase_if(s.members, [&](const Member& m) { return m.xuid == xuid; });
      }
      return Ok(json::object());
    });
  }
  if (method == "session.start") {
    return guarded(true, [&](Session& s) {
      if (s.state != "lobby") return Err("state");
      s.state = "ingame";
      return Ok(json::object());
    });
  }
  if (method == "session.end") {
    return guarded(true, [&](Session& s) {
      if (s.state != "ingame") return Err("state");
      s.state = "lobby";
      return Ok(json::object());
    });
  }
  if (method == "session.delete") {
    return guarded(true, [&](Session& s) {
      sessions_.erase(s.id);
      return Ok(json::object());
    });
  }
  if (method == "session.search") {
    const std::string title = a.at("title").get<std::string>();
    const std::string compat = a.at("compat").get<std::string>();
    const uint32_t max_results = a.at("max_results").get<uint32_t>();
    const uint32_t num_users = a.at("num_users").get<uint32_t>();
    std::vector<const Session*> found;
    for (const auto& [id, s] : sessions_) {
      if (s.title != title || s.compat != compat || s.host == who.peer_id) continue;
      if (!(s.flags & 0x8)) continue;
      if (s.state != "lobby" && (s.flags & 0x400)) continue;
      uint32_t pub = 0;
      for (const auto& m : s.members) pub += m.is_private ? 0 : 1;
      if (s.public_slots < pub + num_users) continue;
      bool match = true;
      for (const auto& c : a.at("contexts")) {
        const uint32_t cid = c.at(0).get<uint32_t>(), cv = c.at(1).get<uint32_t>();
        bool hit = false;
        for (const auto& sc : s.contexts)
          if (sc.at(0).get<uint32_t>() == cid && sc.at(1).get<uint32_t>() == cv) hit = true;
        match = match && hit;
      }
      if (match) found.push_back(&s);
    }
    std::sort(found.begin(), found.end(),
              [](const Session* x, const Session* y) { return x->seq > y->seq; });
    json results = json::array();
    for (const Session* s : found) {
      if (results.size() >= max_results) break;
      results.push_back(Doc(*s));
    }
    return Ok({{"results", results}});
  }
  if (method == "session.arbitrate") {
    return guarded(false, [&](Session& s) {
      if (ParseHex16(a.at("nonce")) != s.nonce) return Err("bad_request", "nonce");
      // Members grouped by machine, in join order.
      std::vector<std::pair<uint64_t, std::vector<Xuid>>> machines;
      for (const auto& m : s.members) {
        uint64_t machine = 0;
        for (const auto& [pid, c] : clients_)
          if (c.xuid == m.xuid) machine = MachineIdOf(c.vip);
        auto it = std::find_if(machines.begin(), machines.end(),
                               [&](const auto& p) { return p.first == machine; });
        if (it == machines.end()) {
          machines.push_back({machine, {m.xuid}});
        } else {
          it->second.push_back(m.xuid);
        }
      }
      json regs = json::array();
      for (const auto& [machine, xuids] : machines) {
        json jx = json::array();
        for (Xuid x : xuids) jx.push_back(Hex16(x));
        regs.push_back({{"machine_id", Hex16(machine)}, {"trust", 1}, {"xuids", jx}});
      }
      return Ok({{"registrants", regs}});
    });
  }
  if (method == "stats.write") {
    const Xuid xuid = ParseHex16(a.at("xuid"));
    for (const auto& v : a.at("views")) {
      const uint32_t view_id = v.at("view_id").get<uint32_t>();
      for (const auto& p : v.at("props")) stats_[{view_id, xuid}][p.at("id").get<uint32_t>()] = p;
    }
    return Ok(json::object());
  }
  if (method == "stats.read") {
    // Like a sparse server: rows only for players with stats, and only the columns it has.
    json views = json::array();
    for (const auto& spec : a.at("specs")) {
      const uint32_t view_id = spec.at("view_id").get<uint32_t>();
      json rows = json::array();
      uint32_t total = 0;
      for (const auto& [key, cols] : stats_) total += key.first == view_id ? 1 : 0;
      for (const auto& x : a.at("xuids")) {
        const Xuid xuid = ParseHex16(x);
        auto it = stats_.find({view_id, xuid});
        if (it == stats_.end()) continue;
        json jc = json::array();
        for (const auto& c : spec.at("columns")) {
          auto ci = it->second.find(c.get<uint32_t>());
          if (ci != it->second.end()) jc.push_back(ci->second);
        }
        std::string gt;
        for (const auto& [pid, c] : clients_)
          if (c.xuid == xuid) gt = c.gamertag;
        rows.push_back(
            {{"xuid", Hex16(xuid)}, {"gamertag", gt}, {"rank", 1}, {"rating", 0}, {"columns", jc}});
      }
      views.push_back({{"view_id", view_id}, {"total_rows", total}, {"rows", rows}});
    }
    return Ok({{"views", views}});
  }
  if (method == "stats.enumerate") {
    const uint32_t view_id = a.at("view_id").get<uint32_t>();
    const uint32_t num_rows = a.at("num_rows").get<uint32_t>();
    const std::string pivot = a.at("pivot").get<std::string>();
    std::vector<Xuid> board;
    for (const auto& [key, cols] : stats_)
      if (key.first == view_id) board.push_back(key.second);
    json rows = json::array();
    bool on_board = true;
    if (pivot == "xuid") {
      const Xuid x = ParseHex16(a.at("value"));
      on_board = std::find(board.begin(), board.end(), x) != board.end();
    }
    if (on_board) {
      uint32_t rank = 0;
      for (Xuid x : board) {
        if (rows.size() >= num_rows) break;
        json jc = json::array();
        const auto& cols = stats_.at({view_id, x});
        for (const auto& c : a.at("columns")) {
          auto ci = cols.find(c.get<uint32_t>());
          if (ci != cols.end()) jc.push_back(ci->second);
        }
        rows.push_back({{"xuid", Hex16(x)}, {"rank", ++rank}, {"rating", 0}, {"columns", jc}});
      }
    }
    return Ok(
        {{"views",
          json::array({{{"view_id", view_id}, {"total_rows", board.size()}, {"rows", rows}}})}});
  }
  return Err("bad_request", "unknown method " + method);
}

std::vector<FakeLobby::CallRecord> FakeLobby::Calls() const {
  std::lock_guard lk(mu_);
  return calls_;
}

std::vector<FakeLobby::CallRecord> FakeLobby::Calls(const std::string& method) const {
  std::lock_guard lk(mu_);
  std::vector<CallRecord> out;
  for (const auto& c : calls_)
    if (c.method == method) out.push_back(c);
  return out;
}

size_t FakeLobby::Count(const std::string& method) const { return Calls(method).size(); }

void FakeLobby::ClearCalls() {
  std::lock_guard lk(mu_);
  calls_.clear();
}

std::optional<json> FakeLobby::SessionDoc(uint64_t session_id) const {
  std::lock_guard lk(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return std::nullopt;
  return Doc(it->second);
}

size_t FakeLobby::SessionCount() const {
  std::lock_guard lk(mu_);
  return sessions_.size();
}

json FakeLobby::StatsValue(uint32_t view_id, Xuid xuid, uint32_t column) const {
  std::lock_guard lk(mu_);
  auto it = stats_.find({view_id, xuid});
  if (it == stats_.end()) return nullptr;
  auto ci = it->second.find(column);
  return ci == it->second.end() ? json(nullptr) : ci->second.at("v");
}

void FakeLobby::FailNext(const std::string& method, const std::string& err) {
  std::lock_guard lk(mu_);
  fail_next_[method] = err;
}

void FakeLobby::DropNext(const std::string& method) {
  std::lock_guard lk(mu_);
  ++drop_next_[method];
}

void FakeLobby::Override(const std::string& method, std::function<RpcResult()> fn) {
  std::lock_guard lk(mu_);
  overrides_[method] = std::move(fn);
}

// --- FakeBackend ---

FakeBackend::FakeBackend(FakeLobby* lobby, FakeClient me)
    : lobby_(lobby), me_(std::move(me)), rng_(me_.peer_id) {
  config_.enabled = true;
  config_.title_id = "4E4D07D1";
  config_.compat_key = "0123456789abcdef0123456789abcdef";
  config_.gamertag = me_.gamertag;
  lobby_->Register(me_);
}

RpcResult FakeBackend::Call(const std::string& method, const std::string& args_json,
                            uint32_t timeout_ms) {
  std::chrono::milliseconds latency{0};
  {
    std::lock_guard lk(mu_);
    ++calls_;
    if (state_ != ac6net::NetState::kOnline && state_ != ac6net::NetState::kConnecting) {
      RpcResult r;
      r.err = "offline";
      return r;
    }
    latency = latency_;
  }
  if (latency.count() > 0) std::this_thread::sleep_for(latency);
  return lobby_->Handle(me_, method, args_json);
}

ac6net::Identity FakeBackend::identity() const {
  std::lock_guard lk(mu_);
  ac6net::Identity id;
  if (!identity_valid_) return id;
  id.valid = true;
  id.peer_id = me_.peer_id;
  id.xuid = me_.xuid;
  id.vip = me_.vip;
  id.gamertag = me_.gamertag;
  return id;
}

ac6net::NetState FakeBackend::state() const {
  std::lock_guard lk(mu_);
  return state_;
}

ac6net::XnAddr FakeBackend::LocalXnAddr() const { return ac6net::MakeXnAddr(me_.vip, me_.peer_id); }

int FakeBackend::XnAddrToInAddr(const ac6net::XnAddr& addr, const ac6net::XnKid* kid,
                                uint32_t* out_vip) {
  uint32_t vip = 0;
  PeerId peer = 0;
  if (!ac6net::ParseXnAddr(addr, &vip, &peer)) return ac6net::wsa::kInval;
  std::lock_guard lk(mu_);
  registered_.push_back({vip, peer});
  if (out_vip) *out_vip = vip;
  return ac6net::wsa::kOk;
}

void FakeBackend::Random(uint8_t* out, size_t len) {
  std::lock_guard lk(mu_);
  for (size_t i = 0; i < len; ++i) out[i] = uint8_t(rng_());
}

void FakeBackend::SetState(ac6net::NetState state) {
  std::lock_guard lk(mu_);
  state_ = state;
}

void FakeBackend::SetLatency(std::chrono::milliseconds latency) {
  std::lock_guard lk(mu_);
  latency_ = latency;
}

void FakeBackend::SetIdentityValid(bool valid) {
  std::lock_guard lk(mu_);
  identity_valid_ = valid;
}

std::vector<std::pair<uint32_t, PeerId>> FakeBackend::Registered() const {
  std::lock_guard lk(mu_);
  return registered_;
}

size_t FakeBackend::CallCount() const {
  std::lock_guard lk(mu_);
  return calls_;
}

}  // namespace xlive_test
