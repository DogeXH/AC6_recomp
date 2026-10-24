// ac6net - XGI-level Live emulation: users, contexts/properties, sessions, search, arbitration
// and stats, on top of an XLiveBackend (the lobby RPC plus identity/XNet helpers).
//
// Session semantics are ported from Xenia Canary's XSession
// (src/xenia/kernel/xsession.cc, Copyright 2026 Xenia Canary, BSD-3-Clause): the
// LOBBY -> REGISTRATION -> INGAME -> REPORTING -> LOBBY state machine, member and slot
// accounting, the modify rules, arbitrated "zombie" members, and the per-column stats cache
// that is flushed at XSessionEnd. Flag validation follows the XDK wrapper that AC6 links
// (re/xmsg_map_verify.md C7). Missing stats rows are synthesized the way Xenia's
// web services answer reads (UNSET columns, rank 0).
//
// Threading. Every public call is thread-safe. `mu` guards all state and is never held while
// calling the backend. Operations on one session are serialized by that session's `op` mutex,
// which is held across RPCs (lock order: op, then mu). An internal worker thread pushes
// debounced session.attrs updates and runs deferred cleanup, so guest threads never wait for
// those RPCs. The worker starts on first use.
#include "ac6net/xlive.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "ac6net/log.h"
#include "ac6net/netplay.h"
#include "platform.h"
#include "xlive_backend.h"
#include "xlive_codec.h"

namespace ac6net::xlive {

using codec::Json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kRpcTimeoutMs = 6000;
constexpr uint32_t kSearchTimeoutMs = 8000;
constexpr auto kAttrsDebounce = std::chrono::milliseconds(500);
constexpr auto kAttrsMaxDelay = std::chrono::milliseconds(2000);
constexpr auto kAttrsRetry = std::chrono::milliseconds(2000);

// XDK XSessionCreate validation (821FD4C0): these bits must be clear.
constexpr uint32_t kInvalidFlagBits = 0xFFFFE0C0;
// Flags XSessionModify may change (Xenia xsession.cc ModifySession).
constexpr uint32_t kModifiableFlags =
    sflag::kUsesArbitration | sflag::kInvitesDisabled | sflag::kJoinViaPresenceDisabled |
    sflag::kJoinInProgressDisabled | sflag::kJoinViaPresenceFriendsOnly;
constexpr uint32_t kSinglePlayerWithStats =
    sflag::kUsesPresence | sflag::kUsesStats | sflag::kInvitesDisabled |
    sflag::kJoinViaPresenceDisabled | sflag::kJoinInProgressDisabled;
constexpr uint32_t kLiveFlags =
    sflag::kUsesPresence | sflag::kUsesStats | sflag::kUsesMatchmaking | sflag::kUsesArbitration;

// System stats columns (rank, rating, gamertag) are reported in the row, not as columns.
constexpr uint16_t kRankColumn = 0xFFFF;
constexpr uint16_t kRatingColumn = 0xFFFE;
constexpr uint16_t kGamertagColumn = 0xFFFD;
constexpr uint32_t kMaxStatsRows = 100;  // X_STATS_MAX_ROW_COUNT

// Attributes each XLast matchmaking query returns with a search result, by title and
// procedure index. From AC6's SPA (notes/ac6_spa_summary.json): "Find Matches" (0) and
// "Find Matches 2" (1, the one AC6 uses) both return contexts 0x800A and 0x800B, in that order,
// and no properties.
struct QueryReturns {
  uint32_t title_id;
  uint32_t procedure;
  std::array<uint32_t, 2> ids;
};
constexpr QueryReturns kQueryReturns[] = {
    {0x4E4D07D1, 0, {kContextGameType, kContextGameMode}},
    {0x4E4D07D1, 1, {kContextGameType, kContextGameMode}},
};

const QueryReturns* FindQueryReturns(const std::string& title_hex, uint32_t procedure) {
  uint32_t title = 0;
  if (title_hex.size() != 8) return nullptr;
  for (char c : title_hex) {
    const int d = (c >= '0' && c <= '9')   ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                           : -1;
    if (d < 0) return nullptr;
    title = (title << 4) | uint32_t(d);
  }
  for (const auto& q : kQueryReturns)
    if (q.title_id == title && q.procedure == procedure) return &q;
  return nullptr;
}

// Keeps only the query's return attributes, in the query's order. Context IDs have a zero
// type nibble (bits 28-31); everything else is a property.
void ApplyQueryReturns(const QueryReturns& q, SearchResult* r) {
  std::vector<Context> contexts;
  std::vector<Property> properties;
  for (uint32_t id : q.ids) {
    if ((id >> 28) == 0) {
      for (const auto& c : r->contexts)
        if (c.id == id) {
          contexts.push_back(c);
          break;
        }
    } else {
      for (const auto& p : r->properties)
        if (p.id == id) {
          properties.push_back(p);
          break;
        }
    }
  }
  r->contexts = std::move(contexts);
  r->properties = std::move(properties);
}

bool IsSystemColumn(uint16_t id) {
  return id == kRankColumn || id == kRatingColumn || id == kGamertagColumn;
}

bool IsOnlineXuid(Xuid xuid) { return (xuid >> 48) == 0x0009; }

std::string HrText(uint32_t hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", hr);
  return buf;
}

uint32_t ValidateCreateFlags(uint32_t flags) {
  using namespace sflag;
  if (flags & kInvalidFlagBits) return hr::kSessionInvalidFlags;
  if ((flags & kUsesMatchmaking) && !(flags & kUsesPeerNetwork)) return hr::kSessionInvalidFlags;
  if ((flags & kUsesArbitration) && !(flags & kUsesStats)) return hr::kSessionInvalidFlags;
  if ((flags & kHost) && !(flags & (kUsesStats | kUsesMatchmaking | kUsesPeerNetwork))) {
    return hr::kSessionInvalidFlags;
  }
  return hr::kOk;
}

uint32_t HrFromRpc(const RpcResult& r) {
  if (!r.transport_ok) return r.err == "offline" ? hr::kSessionNotLoggedOn : hr::kFail;
  if (r.ok) return hr::kOk;
  if (r.err == "not_found") return hr::kSessionNotFound;
  if (r.err == "full") return hr::kSessionFull;
  if (r.err == "state") return hr::kSessionWrongState;
  if (r.err == "bad_request") return hr::kInvalidArg;
  return hr::kFail;  // not_host, compat, rate, internal, unknown
}

// Builds the body of a trace event (log.h Trace).
class Fields {
 public:
  Fields& U(const char* key, uint64_t v) {
    Key(key);
    s_ += std::to_string(v);
    return *this;
  }
  Fields& B(const char* key, bool v) {
    Key(key);
    s_ += v ? "true" : "false";
    return *this;
  }
  Fields& S(const char* key, std::string_view v) {
    Key(key);
    s_ += '"';
    s_ += JsonEscape(v);
    s_ += '"';
    return *this;
  }
  Fields& Hr(uint32_t hr) { return S("hr", HrText(hr)); }
  const std::string& str() const { return s_; }

 private:
  void Key(const char* key) {
    if (!s_.empty()) s_ += ',';
    s_ += '"';
    s_ += key;
    s_ += "\":";
  }
  std::string s_;
};

// xuid -> view id -> property id -> last written value.
using StatsCache = std::map<Xuid, std::map<uint32_t, std::map<uint32_t, Property>>>;

struct SessionRec {
  std::mutex op;  // serializes operations on this session; taken before Impl::mu

  // Guarded by Impl::mu.
  SessionState st;
  bool server_backed = false;  // the session exists on the lobby server
  bool stale = false;          // the server connection was lost after it was created
  bool ranked = false;         // X_CONTEXT_GAME_TYPE was 0 (ranked) at create time
  bool end_pending = false;    // session.end did not reach the server; resent before a start
  std::set<Xuid> zombies;      // arbitrated members that left (kept for stats)
  StatsCache stats;

  bool Live() const { return (st.flags & kLiveFlags) != 0; }
  bool Advertised() const {
    return st.created && st.is_host && server_backed && !stale &&
           st.phase != SessionPhase::kDeleted;
  }
};

SessionMember* FindMember(SessionState& st, Xuid xuid) {
  for (auto& m : st.members)
    if (m.xuid == xuid) return &m;
  return nullptr;
}

StatsColumn UnsetColumn(uint16_t id) {
  StatsColumn c;
  c.column_id = id;
  c.value.type = DataType::kNull;  // X_USER_DATA_TYPE UNSET (0xFF), zero data
  return c;
}

// Requested columns in request order, without duplicates or system columns.
std::vector<uint16_t> RequestedColumns(const StatsSpec& spec) {
  std::vector<uint16_t> ids;
  for (uint16_t id : spec.column_ids) {
    if (IsSystemColumn(id)) continue;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }
  return ids;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

struct XLive::Impl {
  Impl(XLiveBackend* b, bool owns) : backend(b), owned(owns ? b : nullptr) {}

  ~Impl() {
    {
      std::lock_guard lk(mu);
      stopping = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
  }

  XLiveBackend* backend;
  std::unique_ptr<XLiveBackend> owned;

  mutable std::mutex mu;
  Xuid offline_xuid = 0;
  std::map<uint32_t, uint32_t> contexts;    // user 0
  std::map<uint32_t, Property> properties;  // user 0
  uint64_t attrs_version = 0;               // bumped on every context/property change
  std::map<SessionHandle, std::shared_ptr<SessionRec>> sessions;

  // Worker state (guarded by mu).
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  bool attrs_dirty = false;
  Clock::time_point attrs_first{};
  Clock::time_point attrs_due{};
  bool stopping = false;
  std::thread worker;

  // --- Helpers (no locks held unless the name ends in Locked) ---

  bool Online() const {
    const NetState s = backend->state();
    return backend->identity().valid && (s == NetState::kConnecting || s == NetState::kOnline);
  }

  // The XUID a local user joins sessions with: the online XUID once known, else the offline
  // (profile) XUID.
  Xuid LocalXuid() const {
    const Identity id = backend->identity();
    if (id.valid && id.xuid) return id.xuid;
    std::lock_guard lk(mu);
    return offline_xuid;
  }

  // Leaderboards are keyed by online XUIDs, but AC6 can look its own rows up by the XUID it
  // cached at profile sign-in, which may be the offline (profile) XUID. Maps that one XUID to
  // the online XUID; any other value is returned unchanged.
  Xuid BoardXuid(Xuid x, const Identity& id) const {
    if (!x || !id.valid || !id.xuid || x == id.xuid) return x;
    std::lock_guard lk(mu);
    return x == offline_xuid ? id.xuid : x;
  }

  RpcResult Call(const char* method, const Json& args, uint32_t timeout_ms = kRpcTimeoutMs) {
    return backend->Call(method, codec::Dump(args), timeout_ms);
  }

  static bool ParseResult(const RpcResult& r, Json* out) {
    *out = codec::Parse(r.result_json);
    return !out->is_discarded() && out->is_object();
  }

  std::shared_ptr<SessionRec> Get(SessionHandle handle) const {
    std::lock_guard lk(mu);
    const auto it = sessions.find(handle);
    return it == sessions.end() ? nullptr : it->second;
  }

  Json UserContextsLocked() const {
    Json arr = Json::array();
    for (const auto& [id, value] : contexts) arr.push_back(codec::ContextToJson({id, value}));
    return arr;
  }

  Json UserPropertiesLocked() const {
    Json arr = Json::array();
    for (const auto& [id, p] : properties) {
      Json j;
      if (codec::PropertyToJson(p, &j)) arr.push_back(std::move(j));
    }
    return arr;
  }

  bool AnyAdvertisedLocked() const {
    for (const auto& [h, rec] : sessions)
      if (rec->Advertised()) return true;
    return false;
  }

  // The worker starts on first use, so a disabled Netplay costs no thread.
  void EnsureWorkerLocked() {
    if (!worker.joinable() && !stopping) worker = std::thread([this] {
        platform::SetCurrentThreadName("ac6net-xlive");
        WorkerLoop();
      });
  }

  void ScheduleAttrsLocked(Clock::time_point due) {
    EnsureWorkerLocked();
    const auto now = Clock::now();
    if (!attrs_dirty) {
      attrs_dirty = true;
      attrs_first = now;
    }
    attrs_due = std::min(due, attrs_first + kAttrsMaxDelay);
    cv.notify_all();
  }

  void OnAttrsChangedLocked() {
    ++attrs_version;
    if (AnyAdvertisedLocked()) ScheduleAttrsLocked(Clock::now() + kAttrsDebounce);
  }

  void EnqueueLocked(std::function<void()> task) {
    EnsureWorkerLocked();
    tasks.push_back(std::move(task));
    cv.notify_all();
  }

  void TraceOp(const char* event, const Fields& f) {
    if (TraceEnabled()) Trace(event, f.str());
  }

  // --- Worker ---

  struct AttrsTarget {
    std::string sid;
  };

  void WorkerLoop() {
    std::unique_lock lk(mu);
    while (!stopping) {
      if (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        lk.unlock();
        task();
        lk.lock();
        continue;
      }
      if (attrs_dirty) {
        if (Clock::now() < attrs_due) {
          cv.wait_until(lk, attrs_due);
          continue;
        }
        attrs_dirty = false;
        std::vector<AttrsTarget> targets;
        for (const auto& [h, rec] : sessions)
          if (rec->Advertised()) targets.push_back({codec::KidToHex(rec->st.info.session_id)});
        Json ctx = UserContextsLocked();
        Json props = UserPropertiesLocked();
        lk.unlock();
        const bool retry = PushAttrs(targets, ctx, props);
        lk.lock();
        if (retry && !attrs_dirty && !stopping) ScheduleAttrsLocked(Clock::now() + kAttrsRetry);
        continue;
      }
      cv.wait(lk);
    }
  }

  // Returns true when a transport failure means the push should be retried.
  bool PushAttrs(const std::vector<AttrsTarget>& targets, const Json& ctx, const Json& props) {
    if (targets.empty()) return false;
    if (!Online()) return true;
    bool retry = false;
    for (const auto& t : targets) {
      Json args = {{"session_id", t.sid}, {"contexts", ctx}, {"properties", props}};
      const RpcResult r = Call("session.attrs", args);
      const uint32_t res = HrFromRpc(r);
      LogF(LogLevel::kDebug, "xlive: session.attrs(%s, %zu contexts, %zu properties) -> %s",
           t.sid.c_str(), ctx.size(), props.size(), HrText(res).c_str());
      if (!r.transport_ok) {
        retry = true;
      } else if (!r.ok) {
        LogF(LogLevel::kWarn, "xlive: session.attrs(%s) refused: %s %s", t.sid.c_str(),
             r.err.c_str(), r.msg.c_str());
      }
      TraceOp(
          "xlive.session.attrs",
          Fields().S("sid", t.sid).U("contexts", ctx.size()).U("properties", props.size()).Hr(res));
    }
    return retry;
  }

  // --- Sessions ---

  uint32_t Create(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                  uint32_t private_slots, uint32_t user_index, SessionInfo* info, uint64_t* nonce,
                  Fields& f) {
    if (!info || !nonce || user_index != 0) return hr::kInvalidArg;
    if (const uint32_t v = ValidateCreateFlags(flags); v != hr::kOk) return v;
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    bool ranked = false;
    {
      std::lock_guard lk(mu);
      if (rec->st.created) return hr::kSessionWrongState;
      const auto gt = contexts.find(kContextGameType);
      ranked = gt != contexts.end() && gt->second == 0;
    }

    // flags == STATS (a stats-only session), flags == 0 (no Live features) and Xenia's
    // SINGLEPLAYER_WITH_STATS (which has no HOST bit but is not a join) stay local.
    const bool local_only =
        flags == 0 || flags == sflag::kUsesStats || flags == kSinglePlayerWithStats;
    const bool host = local_only || (flags & sflag::kHost);
    const Identity id = backend->identity();

    SessionState st;
    st.handle = handle;
    st.created = true;
    st.is_host = host;
    st.flags = flags;
    st.public_slots = public_slots;
    st.private_slots = private_slots;
    st.phase = SessionPhase::kLobby;
    bool server_backed = false;
    uint64_t version_sent = 0;

    if (local_only) {
      XnKid kid;
      backend->Random(kid.b.data(), kid.b.size());
      kid.b[0] = 0x00;  // XNKID type: not an online peer session
      info->session_id = kid;
      info->host = backend->LocalXnAddr();
      backend->Random(info->key.b.data(), info->key.b.size());
      backend->Random(reinterpret_cast<uint8_t*>(nonce), sizeof(*nonce));
      st.host_vip = id.valid ? id.vip : 0;
    } else if (host) {
      if (!Online()) return hr::kSessionNotLoggedOn;
      const Config& cfg = backend->config();
      Json args = {{"title", cfg.title_id}, {"compat", cfg.compat_key}};
      {
        std::lock_guard lk(mu);
        args.update(Json{{"flags", flags},
                         {"public_slots", public_slots},
                         {"private_slots", private_slots},
                         {"contexts", UserContextsLocked()},
                         {"properties", UserPropertiesLocked()}});
        version_sent = attrs_version;
      }
      const RpcResult r = Call("session.create", args);
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
      Json res;
      std::string sid, key, nonce_hex;
      XnKid kid;
      XnKey xkey;
      uint64_t n = 0;
      if (!ParseResult(r, &res) || !codec::GetString(res, "session_id", &sid) ||
          !codec::KidFromHex(sid, &kid) || !codec::GetString(res, "key", &key) ||
          !codec::KeyFromHex(key, &xkey) || !codec::GetString(res, "nonce", &nonce_hex) ||
          !codec::ParseHexU64(nonce_hex, &n)) {
        LogF(LogLevel::kError, "xlive: session.create returned a malformed result: %s",
             r.result_json.c_str());
        return hr::kFail;
      }
      info->session_id = kid;
      info->host = backend->LocalXnAddr();
      info->key = xkey;
      *nonce = n;
      st.host_vip = id.vip;
      server_backed = true;
    } else {
      // Join: the caller's XSESSION_INFO comes from a search result.
      if (!Online()) return hr::kSessionNotLoggedOn;
      const std::string sid = codec::KidToHex(info->session_id);
      const RpcResult r = Call("session.get", Json{{"session_id", sid}});
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
      Json res;
      codec::SessionDoc doc;
      if (!ParseResult(r, &res) || !codec::ParseSessionDoc(res, &doc) ||
          doc.session_id != info->session_id) {
        LogF(LogLevel::kError, "xlive: session.get(%s) returned a malformed document", sid.c_str());
        return hr::kFail;
      }
      if (!IsVip(doc.host_vip)) {
        LogF(LogLevel::kError, "xlive: session %s has a host outside the vIP range", sid.c_str());
        return hr::kSessionNotFound;
      }
      uint32_t given_vip = 0;
      PeerId given_peer = 0;
      if (!ParseXnAddr(info->host, &given_vip, &given_peer) || given_vip != doc.host_vip ||
          given_peer != doc.host_peer_id) {
        LogF(LogLevel::kInfo, "xlive: session %s: host address normalized to %s", sid.c_str(),
             codec::FormatIpv4(doc.host_vip).c_str());
        info->host = MakeXnAddr(doc.host_vip, doc.host_peer_id);
      }
      uint32_t vip = 0;
      if (const int err = backend->XnAddrToInAddr(info->host, &info->session_id, &vip);
          err != wsa::kOk) {
        LogF(LogLevel::kWarn, "xlive: session %s: registering the host address failed (%d)",
             sid.c_str(), err);
      }
      // AC6 arbitrates with the nonce XSessionCreate wrote (slot+504, 9.cpp:28761), and the
      // server checks it against the host's, so a joiner takes the host's nonce when the
      // server provides it. Otherwise the caller's value is kept, as Xenia does.
      if (doc.has_nonce) *nonce = doc.nonce;
      st.host_vip = doc.host_vip;
      server_backed = true;
    }

    st.info = *info;
    st.nonce = *nonce;
    {
      std::lock_guard lk(mu);
      rec->st = st;
      rec->server_backed = server_backed;
      rec->stale = false;
      rec->ranked = ranked;
      // Contexts changed while session.create was in flight: push them.
      if (rec->Advertised() && attrs_version != version_sent) {
        ScheduleAttrsLocked(Clock::now() + kAttrsDebounce);
      }
    }
    f.S("sid", codec::KidToHex(info->session_id)).B("host", host).B("local", local_only);
    return hr::kOk;
  }

  // Deletes the session locally and tells the server. Caller holds rec->op.
  uint32_t DeleteHeld(const std::shared_ptr<SessionRec>& rec, Fields& f) {
    std::string method;
    Json args;
    {
      std::lock_guard lk(mu);
      SessionState& st = rec->st;
      if (!st.created || st.phase == SessionPhase::kDeleted) return hr::kOk;
      const std::string sid = codec::KidToHex(st.info.session_id);
      f.S("sid", sid).B("host", st.is_host);
      if (rec->server_backed && !rec->stale) {
        if (st.is_host) {
          method = "session.delete";
          args = {{"session_id", sid}};
        } else {
          Json xuids = Json::array();
          for (const auto& m : st.members)
            if (m.local) xuids.push_back(codec::HexU64(m.xuid));
          if (!xuids.empty()) {
            method = "session.leave";
            args = {{"session_id", sid}, {"xuids", std::move(xuids)}};
          }
        }
      }
      st.phase = SessionPhase::kDeleted;
      st.members.clear();
      rec->zombies.clear();
      rec->stats.clear();
    }
    if (!method.empty() && Online()) {
      const RpcResult r = Call(method.c_str(), args);
      if (!r.transport_ok || !r.ok) {
        LogF(LogLevel::kWarn, "xlive: %s failed (%s %s); the server will expire it", method.c_str(),
             r.err.c_str(), r.msg.c_str());
      }
    }
    return hr::kOk;
  }

  uint32_t Delete(SessionHandle handle, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    return DeleteHeld(rec, f);
  }

  uint32_t Start(SessionHandle handle, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    bool need_rpc = false, stale = false, live = false, end_pending = false;
    std::string sid;
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!st.created ||
          (st.phase != SessionPhase::kLobby && st.phase != SessionPhase::kRegistration)) {
        return hr::kSessionWrongState;
      }
      // Arbitrated sessions must be registered before they can start.
      if ((st.flags & sflag::kUsesArbitration) && st.phase != SessionPhase::kRegistration) {
        return hr::kSessionNotRegistered;
      }
      need_rpc = st.is_host && rec->server_backed;
      stale = rec->stale;
      live = rec->Live() && rec->server_backed;
      end_pending = rec->end_pending;
      sid = codec::KidToHex(st.info.session_id);
    }
    f.S("sid", sid);
    if (live && !Online()) return hr::kSessionNotLoggedOn;
    if (need_rpc) {
      if (stale) return hr::kSessionNotLoggedOn;
      if (end_pending) {
        // The previous match's session.end was lost; the server would refuse the start.
        const RpcResult e = Call("session.end", Json{{"session_id", sid}});
        if (!e.transport_ok) return HrFromRpc(e);
        std::lock_guard lk(mu);
        rec->end_pending = false;
      }
      const RpcResult r = Call("session.start", Json{{"session_id", sid}});
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
    }
    std::lock_guard lk(mu);
    rec->st.phase = SessionPhase::kInGame;
    return hr::kOk;
  }

  uint32_t End(SessionHandle handle, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    bool need_rpc = false;
    std::string sid;
    {
      std::lock_guard lk(mu);
      SessionState& st = rec->st;
      if (!st.created || st.phase != SessionPhase::kInGame) return hr::kSessionWrongState;
      st.phase = SessionPhase::kReporting;
      need_rpc = st.is_host && rec->server_backed && !rec->stale;
      sid = codec::KidToHex(st.info.session_id);
    }
    f.S("sid", sid);
    const uint32_t flushed = FlushHeld(rec);
    f.S("flush", HrText(flushed));
    bool lost = false;
    if (need_rpc) {
      const RpcResult r = Online() ? Call("session.end", Json{{"session_id", sid}}) : RpcResult{};
      if (!r.transport_ok || !r.ok) {
        LogF(LogLevel::kWarn, "xlive: session.end(%s) failed: %s %s", sid.c_str(), r.err.c_str(),
             r.msg.c_str());
      }
      lost = !r.transport_ok;  // resent before the next start
    }
    // Reporting is done; the session can be started again.
    std::lock_guard lk(mu);
    rec->st.phase = SessionPhase::kLobby;
    rec->end_pending = lost;
    return hr::kOk;
  }

  uint32_t Modify(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                  uint32_t private_slots, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    uint32_t new_flags = flags;
    bool need_rpc = false, stale = false;
    std::string sid;
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!st.created || st.phase == SessionPhase::kDeleted) return hr::kSessionWrongState;
      if ((flags ^ st.flags) & ~kModifiableFlags) return hr::kSessionInvalidFlags;
      // Arbitration cannot be dropped once registered, or from a ranked session.
      const bool registered = st.phase == SessionPhase::kRegistration ||
                              st.phase == SessionPhase::kInGame ||
                              st.phase == SessionPhase::kReporting;
      if ((st.flags & sflag::kUsesArbitration) && !(flags & sflag::kUsesArbitration) &&
          (registered || rec->ranked)) {
        new_flags |= sflag::kUsesArbitration;
      }
      need_rpc = st.is_host && rec->server_backed;
      stale = rec->stale;
      sid = codec::KidToHex(st.info.session_id);
    }
    f.S("sid", sid).U("flags", new_flags);
    if (need_rpc) {
      if (stale || !Online()) return hr::kSessionNotLoggedOn;
      const RpcResult r = Call("session.modify", Json{{"session_id", sid},
                                                      {"flags", new_flags},
                                                      {"public_slots", public_slots},
                                                      {"private_slots", private_slots}});
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
    }
    std::lock_guard lk(mu);
    rec->st.flags = new_flags;
    rec->st.public_slots = public_slots;
    rec->st.private_slots = private_slots;
    return hr::kOk;
  }

  struct JoinEntry {
    Xuid xuid = 0;
    bool want_private = false;
    bool local = false;
    uint32_t user_index = 0;
  };

  uint32_t Join(SessionHandle handle, const std::vector<JoinEntry>& entries, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    const Xuid self = LocalXuid();
    std::vector<SessionMember> added;
    Json send = Json::array();
    bool stale = false;
    std::string sid;
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!st.created || st.phase == SessionPhase::kDeleted) return hr::kSessionWrongState;
      // Joining a started session is only allowed when join-in-progress is.
      if (st.phase == SessionPhase::kInGame &&
          (st.flags & (sflag::kUsesArbitration | sflag::kJoinInProgressDisabled))) {
        return hr::kSessionFull;
      }
      uint32_t filled_private = 0;
      for (const auto& m : st.members)
        if (m.is_private && !rec->zombies.count(m.xuid)) ++filled_private;
      for (const auto& e : entries) {
        const auto same = [&](const SessionMember& m) { return m.xuid == e.xuid; };
        if (FindMember(rec->st, e.xuid) || std::any_of(added.begin(), added.end(), same)) {
          continue;  // already a member
        }
        if (rec->server_backed && rec->Live() && !IsOnlineXuid(e.xuid)) {
          LogF(LogLevel::kWarn, "xlive: joining %016llx, which is not an online XUID",
               (unsigned long long)e.xuid);
        }
        SessionMember m;
        m.xuid = e.xuid;
        m.local = e.local;
        m.user_index = e.user_index;
        // A private request falls back to a public slot when no private slot is free.
        m.is_private = e.want_private && filled_private < st.private_slots;
        if (m.is_private) ++filled_private;
        added.push_back(m);
        // The server accepts other players' XUIDs only from the host.
        if (rec->server_backed && (st.is_host || e.xuid == self)) {
          send.push_back({{"xuid", codec::HexU64(m.xuid)}, {"private", m.is_private}});
        }
      }
      stale = rec->stale;
      sid = codec::KidToHex(st.info.session_id);
    }
    f.S("sid", sid).U("n", added.size()).U("sent", send.size());
    if (added.empty()) return hr::kOk;
    if (!send.empty()) {
      if (stale || !Online()) return hr::kSessionNotLoggedOn;
      const RpcResult r = Call("session.join", Json{{"session_id", sid}, {"members", send}});
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
    }
    std::lock_guard lk(mu);
    for (const auto& m : added)
      if (!FindMember(rec->st, m.xuid)) rec->st.members.push_back(m);
    return hr::kOk;
  }

  uint32_t Leave(SessionHandle handle, const std::vector<Xuid>& xuids, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    const Xuid self = LocalXuid();
    Json send = Json::array();
    bool stale = false;
    std::string sid;
    size_t left = 0;
    {
      std::lock_guard lk(mu);
      SessionState& st = rec->st;
      if (!st.created || st.phase == SessionPhase::kDeleted) return hr::kSessionWrongState;
      const bool arbitrated = st.flags & sflag::kUsesArbitration;
      for (Xuid xuid : xuids) {
        if (!FindMember(st, xuid)) continue;
        ++left;
        // Arbitrated sessions keep departed members for stats reporting.
        if (arbitrated) {
          rec->zombies.insert(xuid);
          continue;
        }
        std::erase_if(st.members, [&](const SessionMember& m) { return m.xuid == xuid; });
        if (rec->server_backed && (st.is_host || xuid == self)) {
          send.push_back(codec::HexU64(xuid));
        }
      }
      stale = rec->stale;
      sid = codec::KidToHex(st.info.session_id);
    }
    f.S("sid", sid).U("n", left).U("sent", send.size());
    if (!send.empty() && !stale && Online()) {
      const RpcResult r = Call("session.leave", Json{{"session_id", sid}, {"xuids", send}});
      if (!r.transport_ok || !r.ok) {
        LogF(LogLevel::kWarn, "xlive: session.leave(%s) failed: %s %s", sid.c_str(), r.err.c_str(),
             r.msg.c_str());
      }
    }
    return hr::kOk;
  }

  uint32_t Search(uint32_t procedure_index, uint32_t user_index, uint32_t max_results,
                  uint32_t num_users, const std::vector<Property>& props,
                  const std::vector<Context>& ctxs, std::vector<SearchResult>* out, Fields& f) {
    if (!out) return hr::kInvalidArg;
    out->clear();
    if (user_index != 0) return hr::kInvalidArg;
    if (!Online()) return hr::kSessionNotLoggedOn;
    const Identity id = backend->identity();
    const QueryReturns* returns = FindQueryReturns(backend->config().title_id, procedure_index);
    const Json args = {{"title", backend->config().title_id},
                       {"compat", backend->config().compat_key},
                       {"proc", procedure_index},
                       {"max_results", max_results},
                       {"num_users", num_users},
                       {"contexts", codec::ContextsToJson(ctxs)},
                       {"properties", codec::PropertiesToJson(props)}};
    const RpcResult r = Call("session.search", args, kSearchTimeoutMs);
    if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
    Json res;
    const Json* results = nullptr;
    if (!ParseResult(r, &res) || !(results = codec::Find(res, "results")) || !results->is_array()) {
      LogF(LogLevel::kError, "xlive: session.search returned a malformed result");
      return hr::kFail;
    }
    std::set<std::array<uint8_t, 8>> own;  // our own sessions are never search results
    {
      std::lock_guard lk(mu);
      for (const auto& [h, rec] : sessions)
        if (rec->st.created) own.insert(rec->st.info.session_id.b);
    }
    size_t skipped = 0;
    for (const auto& item : *results) {
      if (out->size() >= max_results) break;
      codec::SessionDoc doc;
      if (!codec::ParseSessionDoc(item, &doc) || !doc.has_key || !IsVip(doc.host_vip)) {
        ++skipped;
        continue;
      }
      if ((id.valid && doc.host_peer_id == id.peer_id) || own.count(doc.session_id.b)) continue;
      SearchResult sr;
      sr.info.session_id = doc.session_id;
      sr.info.key = doc.key;
      sr.info.host = MakeXnAddr(doc.host_vip, doc.host_peer_id);
      sr.filled_public = doc.filled_public;
      sr.filled_private = doc.filled_private;
      sr.open_public =
          doc.public_slots > doc.filled_public ? doc.public_slots - doc.filled_public : 0;
      sr.open_private =
          doc.private_slots > doc.filled_private ? doc.private_slots - doc.filled_private : 0;
      sr.contexts = std::move(doc.contexts);
      sr.properties = std::move(doc.properties);
      if (returns) ApplyQueryReturns(*returns, &sr);
      out->push_back(std::move(sr));
    }
    if (skipped) LogF(LogLevel::kWarn, "xlive: session.search: skipped %zu bad results", skipped);
    f.U("count", out->size());
    return hr::kOk;
  }

  uint32_t Arbitrate(SessionHandle handle, uint64_t nonce, std::vector<Registrant>* out,
                     Fields& f) {
    if (!out) return hr::kInvalidArg;
    out->clear();
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    bool server_backed = false, stale = false;
    std::string sid;
    std::vector<Xuid> members;
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!st.created ||
          (st.phase != SessionPhase::kLobby && st.phase != SessionPhase::kRegistration)) {
        return hr::kSessionWrongState;
      }
      server_backed = rec->server_backed;
      stale = rec->stale;
      sid = codec::KidToHex(st.info.session_id);
      for (const auto& m : st.members) members.push_back(m.xuid);
    }
    f.S("sid", sid);
    std::vector<Registrant> regs;
    if (server_backed) {
      if (stale || !Online()) return hr::kSessionNotLoggedOn;
      const RpcResult r =
          Call("session.arbitrate", Json{{"session_id", sid}, {"nonce", codec::HexU64(nonce)}});
      if (const uint32_t e = HrFromRpc(r); e != hr::kOk) return e;
      Json res;
      const Json* list = nullptr;
      if (!ParseResult(r, &res) || !(list = codec::Find(res, "registrants")) || !list->is_array()) {
        LogF(LogLevel::kError, "xlive: session.arbitrate returned a malformed result");
        return hr::kFail;
      }
      for (const auto& item : *list) {
        Registrant reg;
        uint32_t trust = 1;
        const Json* xuids = codec::Find(item, "xuids");
        if (!codec::GetU64Flexible(item, "machine_id", &reg.machine_id) || !xuids ||
            !xuids->is_array()) {
          continue;
        }
        if (codec::GetU32(item, "trust", &trust))
          reg.trustworthiness = uint8_t(std::min(trust, 255u));
        for (const auto& x : *xuids) {
          uint64_t xuid = 0;
          if (x.is_string() && codec::ParseHexU64(x.get_ref<const std::string&>(), &xuid)) {
            reg.users.push_back(xuid);
          }
        }
        regs.push_back(std::move(reg));
      }
    } else {
      // Local session: this machine is the only registrant.
      Registrant reg;
      reg.machine_id = MachineIdFromXnAddr(backend->LocalXnAddr());
      reg.users = members;
      regs.push_back(std::move(reg));
    }
    f.U("registrants", regs.size());
    {
      std::lock_guard lk(mu);
      rec->st.phase = SessionPhase::kRegistration;
    }
    *out = std::move(regs);
    return hr::kOk;
  }

  uint32_t WriteStats(SessionHandle handle, Xuid xuid, const std::vector<StatsWriteView>& views,
                      Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    std::lock_guard lk(mu);
    const SessionState& st = rec->st;
    if (!st.created || st.phase == SessionPhase::kDeleted) return hr::kSessionWrongState;
    if (!(st.flags & sflag::kUsesStats)) return hr::kSessionInvalidFlags;
    if (st.phase != SessionPhase::kInGame) return hr::kSessionWrongState;
    f.S("sid", codec::KidToHex(st.info.session_id)).S("xuid", codec::HexU64(xuid));
    // XUID 0 carries TrueSkill system overrides, which v1 does not model.
    if (!xuid) return hr::kOk;
    size_t cached = 0;
    for (const auto& view : views) {
      for (const auto& p : view.properties) {
        // XSessionWriteStats does not support WSTRING or BINARY values.
        const DataType t = p.value.type;
        if (t == DataType::kUnicode || t == DataType::kBinary || t == DataType::kNull) continue;
        rec->stats[xuid][view.view_id][p.id] = p;  // last write wins
        ++cached;
      }
    }
    f.U("views", views.size()).U("cached", cached);
    return hr::kOk;
  }

  // Sends the cached stats (one stats.write per XUID). Caller holds rec->op.
  uint32_t FlushHeld(const std::shared_ptr<SessionRec>& rec) {
    struct Pending {
      Xuid xuid;
      Json args;
    };
    std::vector<Pending> pending;
    bool stale = false;
    const std::string title = backend->config().title_id;
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!(st.flags & sflag::kUsesStats) || rec->stats.empty()) return hr::kOk;
      // In arbitrated sessions only the host reports, so peers do not double-count.
      if (!st.is_host && (st.flags & sflag::kUsesArbitration)) {
        rec->stats.clear();
        return hr::kOk;
      }
      // A session the server does not know (stats-only or offline) writes with the zero
      // session ID; the server then accepts the caller's own XUID on unranked views.
      const std::string sid =
          rec->server_backed ? codec::KidToHex(st.info.session_id) : std::string(16, '0');
      for (const auto& [xuid, views] : rec->stats) {
        Json jviews = Json::array();
        for (const auto& [view_id, props] : views) {
          Json jprops = Json::array();
          for (const auto& [pid, p] : props) {
            Json j;
            if (codec::PropertyToJson(p, &j)) jprops.push_back(std::move(j));
          }
          jviews.push_back({{"view_id", view_id}, {"props", std::move(jprops)}});
        }
        pending.push_back({xuid, Json{{"session_id", sid},
                                      {"title", title},
                                      {"xuid", codec::HexU64(xuid)},
                                      {"views", std::move(jviews)}}});
      }
      stale = rec->stale;
    }
    if (stale || !Online()) {
      LogF(LogLevel::kWarn, "xlive: not online; keeping %zu cached stats rows", pending.size());
      return hr::kSessionNotLoggedOn;
    }
    uint32_t result = hr::kOk;
    size_t written = 0;
    for (const auto& p : pending) {
      const RpcResult r = Call("stats.write", p.args);
      const uint32_t e = HrFromRpc(r);
      if (e == hr::kOk) {
        ++written;
        std::lock_guard lk(mu);
        rec->stats.erase(p.xuid);
      } else {
        LogF(LogLevel::kWarn, "xlive: stats.write(%016llx) failed: %s %s",
             (unsigned long long)p.xuid, r.err.c_str(), r.msg.c_str());
        if (result == hr::kOk) result = e;
      }
    }
    TraceOp("xlive.stats.flush",
            Fields().U("rows", pending.size()).U("written", written).Hr(result));
    return result;
  }

  uint32_t Flush(SessionHandle handle, Fields& f) {
    auto rec = Get(handle);
    if (!rec) return hr::kInvalidArg;
    std::lock_guard op(rec->op);
    {
      std::lock_guard lk(mu);
      const SessionState& st = rec->st;
      if (!st.created || !(st.flags & sflag::kUsesStats) || st.phase != SessionPhase::kInGame) {
        return hr::kSessionWrongState;
      }
      f.S("sid", codec::KidToHex(st.info.session_id));
    }
    return FlushHeld(rec);
  }

  // --- Stats reads ---

  // Row for a player without stats: rank 0 and every requested column UNSET.
  StatsRow MissingRow(Xuid xuid, const std::vector<uint16_t>& columns, const Identity& id) const {
    StatsRow row;
    row.xuid = xuid;
    if (id.valid && xuid == id.xuid) row.gamertag = id.gamertag;
    for (uint16_t c : columns) row.columns.push_back(UnsetColumn(c));
    return row;
  }

  // Normalizes a server row: the requested columns in request order; values the guest
  // structures cannot hold (unicode, binary) and missing values become UNSET.
  static bool ParseRow(const Json& j, const std::vector<uint16_t>& columns, StatsRow* out) {
    StatsRow row;
    if (!codec::GetHexU64(j, "xuid", &row.xuid)) return false;
    codec::GetU32(j, "rank", &row.rank);
    codec::GetI64(j, "rating", &row.rating);
    codec::GetString(j, "gamertag", &row.gamertag);
    // Columns are keyed by the requested (u16) column id; a server that echoes the full
    // property id instead is matched on its low 16 bits.
    std::map<uint32_t, Property> values;
    std::map<uint16_t, const Property*> by_low_id;
    if (const Json* cols = codec::Find(j, "columns")) {
      for (auto& p : codec::PropertiesFromJson(*cols)) values.try_emplace(p.id, std::move(p));
    }
    for (const auto& [id, p] : values)
      if (id > 0xFFFF) by_low_id.try_emplace(uint16_t(id), &p);
    for (uint16_t c : columns) {
      const Property* p = nullptr;
      if (const auto it = values.find(c); it != values.end()) {
        p = &it->second;
      } else if (const auto lo = by_low_id.find(c); lo != by_low_id.end()) {
        p = lo->second;
      }
      if (!p || p->value.type == DataType::kUnicode || p->value.type == DataType::kBinary) {
        row.columns.push_back(UnsetColumn(c));
      } else {
        row.columns.push_back({c, p->value});
      }
    }
    *out = std::move(row);
    return true;
  }

  static Json SpecColumns(const StatsSpec& spec) {
    Json cols = Json::array();
    for (uint16_t c : spec.column_ids) cols.push_back(c);
    return cols;
  }

  // Finds the response view for `view_id` (falls back to position `index`).
  static const Json* FindView(const Json& res, uint32_t view_id, size_t index) {
    const Json* views = codec::Find(res, "views");
    if (!views || !views->is_array()) return nullptr;
    for (const auto& v : *views) {
      uint32_t id = 0;
      if (codec::GetU32(v, "view_id", &id) && id == view_id) return &v;
    }
    if (index < views->size()) {
      uint32_t id = 0;
      const Json& v = (*views)[index];
      if (!codec::GetU32(v, "view_id", &id)) return &v;
    }
    return nullptr;
  }

  uint32_t StatsRead(const std::vector<Xuid>& xuids_in, const std::vector<StatsSpec>& specs,
                     std::vector<StatsView>* out, Fields& f) {
    if (!out) return hr::kInvalidArg;
    out->clear();
    std::vector<Xuid> xuids;
    for (Xuid x : xuids_in)
      if (std::find(xuids.begin(), xuids.end(), x) == xuids.end()) xuids.push_back(x);
    const Identity id = backend->identity();
    bool mapped = false;
    std::vector<Xuid> board_xuids;
    for (Xuid x : xuids) {
      board_xuids.push_back(BoardXuid(x, id));
      mapped |= board_xuids.back() != x;
    }
    const bool online = Online();
    Json res;
    bool have = false;
    if (online && !xuids.empty() && !specs.empty()) {
      Json jx = Json::array();
      for (Xuid x : board_xuids) jx.push_back(codec::HexU64(x));
      Json js = Json::array();
      for (const auto& s : specs)
        js.push_back({{"view_id", s.view_id}, {"columns", SpecColumns(s)}});
      const RpcResult r = Call("stats.read", Json{{"title", backend->config().title_id},
                                                  {"xuids", std::move(jx)},
                                                  {"specs", std::move(js)}});
      have = HrFromRpc(r) == hr::kOk && ParseResult(r, &res);
      if (!have) {
        LogF(LogLevel::kWarn, "xlive: stats.read failed (%s %s); returning empty rows",
             r.err.c_str(), r.msg.c_str());
      }
    }
    for (size_t i = 0; i < specs.size(); ++i) {
      const StatsSpec& spec = specs[i];
      const std::vector<uint16_t> columns = RequestedColumns(spec);
      StatsView view;
      view.view_id = spec.view_id;
      std::map<Xuid, StatsRow> rows;
      if (const Json* jv = have ? FindView(res, spec.view_id, i) : nullptr) {
        codec::GetU32(*jv, "total_rows", &view.total_rows);
        if (const Json* jr = codec::Find(*jv, "rows"); jr && jr->is_array()) {
          for (const auto& item : *jr) {
            StatsRow row;
            if (ParseRow(item, columns, &row)) rows.try_emplace(row.xuid, std::move(row));
          }
        }
      }
      // Always one row per requested player (AC6 needs a result to host), under the XUID the
      // title asked for.
      for (size_t k = 0; k < xuids.size(); ++k) {
        auto it = rows.find(board_xuids[k]);
        // Copied, not moved: the offline and the online XUID can both map to one board row.
        view.rows.push_back(it != rows.end() ? it->second : MissingRow(xuids[k], columns, id));
        view.rows.back().xuid = xuids[k];
      }
      out->push_back(std::move(view));
    }
    f.U("xuids", xuids.size()).U("views", out->size()).B("server", have).B("mapped", mapped);
    return online ? hr::kOk : hr::kNotLoggedOn;
  }

  uint32_t StatsEnumerate(const StatsSpec& spec, StatsPivot pivot, uint64_t pivot_value,
                          uint32_t num_rows, StatsView* out, Fields& f) {
    if (!out) return hr::kInvalidArg;
    *out = StatsView{};
    out->view_id = spec.view_id;
    if (num_rows == 0) return hr::kInvalidArg;
    num_rows = std::min(num_rows, kMaxStatsRows);
    const Identity id = backend->identity();
    const bool online = Online();
    const std::vector<uint16_t> columns = RequestedColumns(spec);
    bool have = false;
    const uint64_t requested = pivot_value;
    if (pivot == StatsPivot::kXuid) pivot_value = BoardXuid(pivot_value, id);
    if (online) {
      Json args = {{"title", backend->config().title_id},
                   {"view_id", spec.view_id},
                   {"columns", SpecColumns(spec)},
                   {"num_rows", num_rows}};
      switch (pivot) {
        case StatsPivot::kXuid:
          args["pivot"] = "xuid";
          args["value"] = codec::HexU64(pivot_value);
          break;
        case StatsPivot::kRank:
          args["pivot"] = "rank";
          args["value"] = uint32_t(pivot_value);
          break;
        case StatsPivot::kRating:
          args["pivot"] = "rating";
          args["value"] = int64_t(pivot_value);
          break;
      }
      const RpcResult r = Call("stats.enumerate", args);
      Json res;
      if (HrFromRpc(r) == hr::kOk && ParseResult(r, &res)) {
        have = true;
        if (const Json* jv = FindView(res, spec.view_id, 0)) {
          codec::GetU32(*jv, "total_rows", &out->total_rows);
          if (const Json* jr = codec::Find(*jv, "rows"); jr && jr->is_array()) {
            for (const auto& item : *jr) {
              if (out->rows.size() >= num_rows) break;
              StatsRow row;
              if (ParseRow(item, columns, &row)) out->rows.push_back(std::move(row));
            }
          }
        }
      } else {
        LogF(LogLevel::kWarn, "xlive: stats.enumerate failed (%s %s); returning empty rows",
             r.err.c_str(), r.msg.c_str());
      }
    }
    // Enumerating around a player always yields that player's row (AC6 needs a result to
    // host, research/xenia_netplay.md 1.8).
    bool found = false;
    if (pivot == StatsPivot::kXuid && pivot_value != 0) {
      for (StatsRow& r : out->rows) {
        if (r.xuid != pivot_value) continue;
        found = true;
        r.xuid = requested;  // the title finds "its" row by the XUID it asked with
      }
      if (!found && out->rows.size() < num_rows) out->rows.push_back(MissingRow(requested, columns, id));
    }
    f.U("view", spec.view_id).U("rows", out->rows.size()).B("server", have);
    switch (pivot) {
      case StatsPivot::kXuid:
        f.S("pivot", "xuid").S("value", codec::HexU64(requested)).B("mapped", pivot_value != requested)
            .B("found", found);
        break;
      case StatsPivot::kRank:
        f.S("pivot", "rank").U("value", uint32_t(requested));
        break;
      case StatsPivot::kRating:
        f.S("pivot", "rating");
        break;
    }
    return online ? hr::kOk : hr::kNotLoggedOn;
  }
};

// ---------------------------------------------------------------------------------------------

XLive::XLive(XLiveBackend* backend, bool owns_backend)
    : impl_(std::make_unique<Impl>(backend, owns_backend)) {}

XLive::~XLive() = default;

std::unique_ptr<XLive> MakeXLiveForTest(XLiveBackend* backend) {
  return std::unique_ptr<XLive>(new XLive(backend, /*owns_backend=*/false));
}

void XLive::SetOfflineXuid(Xuid xuid) {
  LogF(LogLevel::kDebug, "xlive: SetOfflineXuid(%016llx)", (unsigned long long)xuid);
  std::lock_guard lk(impl_->mu);
  impl_->offline_xuid = xuid;
}

Xuid XLive::OfflineXuid() const {
  std::lock_guard lk(impl_->mu);
  return impl_->offline_xuid;
}

bool XLive::SignedInToLive() const { return impl_->Online(); }

Xuid XLive::OnlineXuid() const {
  const Identity id = impl_->backend->identity();
  return id.valid ? id.xuid : 0;
}

std::string XLive::Gamertag() const {
  const Identity id = impl_->backend->identity();
  return id.valid ? id.gamertag : std::string();
}

void XLive::UserSetContext(uint32_t user_index, uint32_t context_id, uint32_t value) {
  LogF(LogLevel::kDebug, "xlive: UserSetContext(user=%u, id=0x%08X, value=%u)", user_index,
       context_id, value);
  if (user_index != 0) return;
  std::lock_guard lk(impl_->mu);
  auto [it, inserted] = impl_->contexts.try_emplace(context_id, value);
  if (!inserted && it->second == value) return;
  it->second = value;
  impl_->OnAttrsChangedLocked();
}

void XLive::UserSetProperty(uint32_t user_index, const Property& property) {
  LogF(LogLevel::kDebug, "xlive: UserSetProperty(user=%u, id=0x%08X, type=%u)", user_index,
       property.id, unsigned(property.value.type));
  if (user_index != 0) return;
  std::lock_guard lk(impl_->mu);
  auto [it, inserted] = impl_->properties.try_emplace(property.id, property);
  if (!inserted && it->second == property) return;
  it->second = property;
  impl_->OnAttrsChangedLocked();
}

bool XLive::UserGetContext(uint32_t user_index, uint32_t context_id, uint32_t* value) const {
  if (user_index != 0 || !value) return false;
  std::lock_guard lk(impl_->mu);
  const auto it = impl_->contexts.find(context_id);
  if (it == impl_->contexts.end()) return false;
  *value = it->second;
  return true;
}

bool XLive::UserGetProperty(uint32_t user_index, uint32_t property_id, Property* property) const {
  if (user_index != 0 || !property) return false;
  std::lock_guard lk(impl_->mu);
  const auto it = impl_->properties.find(property_id);
  if (it == impl_->properties.end()) return false;
  *property = it->second;
  return true;
}

void XLive::SessionHandleCreated(SessionHandle handle) {
  LogF(LogLevel::kDebug, "xlive: SessionHandleCreated(0x%08X)", handle);
  auto rec = std::make_shared<SessionRec>();
  rec->st.handle = handle;
  std::lock_guard lk(impl_->mu);
  auto& slot = impl_->sessions[handle];
  if (slot) {
    LogF(LogLevel::kWarn, "xlive: session handle 0x%08X reused without close", handle);
    impl_->EnqueueLocked([impl = impl_.get(), old = slot] {
      std::lock_guard op(old->op);
      Fields f;
      impl->DeleteHeld(old, f);
    });
  }
  slot = std::move(rec);
}

void XLive::SessionHandleClosed(SessionHandle handle) {
  LogF(LogLevel::kDebug, "xlive: SessionHandleClosed(0x%08X)", handle);
  std::lock_guard lk(impl_->mu);
  const auto it = impl_->sessions.find(handle);
  if (it == impl_->sessions.end()) return;
  auto rec = std::move(it->second);
  impl_->sessions.erase(it);
  // A session the guest never deleted is removed from the server in the background, after
  // any operation still in flight on it.
  impl_->EnqueueLocked([impl = impl_.get(), rec] {
    std::lock_guard op(rec->op);
    Fields f;
    f.U("handle", rec->st.handle);
    impl->DeleteHeld(rec, f);
    impl->TraceOp("xlive.session.close", f);
  });
}

uint32_t XLive::SessionCreate(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                              uint32_t private_slots, uint32_t user_index, SessionInfo* info,
                              uint64_t* nonce) {
  LogF(LogLevel::kDebug,
       "xlive: SessionCreate(handle=0x%08X, flags=0x%08X, public=%u, private=%u, user=%u)", handle,
       flags, public_slots, private_slots, user_index);
  Fields f;
  f.U("handle", handle).U("flags", flags);
  const uint32_t r =
      impl_->Create(handle, flags, public_slots, private_slots, user_index, info, nonce, f);
  LogF(LogLevel::kDebug, "xlive: SessionCreate(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.create", f.Hr(r));
  return r;
}

uint32_t XLive::SessionDelete(SessionHandle handle) {
  LogF(LogLevel::kDebug, "xlive: SessionDelete(0x%08X)", handle);
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->Delete(handle, f);
  impl_->TraceOp("xlive.session.delete", f.Hr(r));
  return r;
}

uint32_t XLive::SessionStart(SessionHandle handle, uint32_t flags) {
  LogF(LogLevel::kDebug, "xlive: SessionStart(0x%08X, flags=0x%08X)", handle, flags);
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->Start(handle, f);
  LogF(LogLevel::kDebug, "xlive: SessionStart(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.start", f.Hr(r));
  return r;
}

uint32_t XLive::SessionEnd(SessionHandle handle) {
  LogF(LogLevel::kDebug, "xlive: SessionEnd(0x%08X)", handle);
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->End(handle, f);
  LogF(LogLevel::kDebug, "xlive: SessionEnd(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.end", f.Hr(r));
  return r;
}

uint32_t XLive::SessionModify(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                              uint32_t private_slots) {
  LogF(LogLevel::kDebug, "xlive: SessionModify(0x%08X, flags=0x%08X, public=%u, private=%u)",
       handle, flags, public_slots, private_slots);
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->Modify(handle, flags, public_slots, private_slots, f);
  LogF(LogLevel::kDebug, "xlive: SessionModify(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.modify", f.Hr(r));
  return r;
}

uint32_t XLive::SessionJoinLocal(SessionHandle handle, const std::vector<uint32_t>& user_indices,
                                 const std::vector<bool>& private_slots) {
  LogF(LogLevel::kDebug, "xlive: SessionJoinLocal(0x%08X, %zu users)", handle, user_indices.size());
  Fields f;
  f.U("handle", handle).B("local", true);
  uint32_t r = hr::kOk;
  const Xuid xuid = impl_->LocalXuid();
  std::vector<Impl::JoinEntry> entries;
  if (user_indices.empty()) r = hr::kInvalidArg;
  for (size_t i = 0; r == hr::kOk && i < user_indices.size(); ++i) {
    if (user_indices[i] != 0 || !xuid) {
      r = hr::kInvalidArg;  // only user 0 is signed in
      break;
    }
    entries.push_back({xuid, i < private_slots.size() && private_slots[i], true, 0});
  }
  if (r == hr::kOk) r = impl_->Join(handle, entries, f);
  LogF(LogLevel::kDebug, "xlive: SessionJoinLocal(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.join", f.Hr(r));
  return r;
}

uint32_t XLive::SessionJoinRemote(SessionHandle handle, const std::vector<Xuid>& xuids,
                                  const std::vector<bool>& private_slots) {
  LogF(LogLevel::kDebug, "xlive: SessionJoinRemote(0x%08X, %zu xuids)", handle, xuids.size());
  Fields f;
  f.U("handle", handle).B("local", false);
  uint32_t r = hr::kOk;
  std::vector<Impl::JoinEntry> entries;
  if (xuids.empty()) r = hr::kInvalidArg;
  const Xuid self = impl_->LocalXuid();
  for (size_t i = 0; r == hr::kOk && i < xuids.size(); ++i) {
    // The local user may be joined by XUID too; it stays a local member.
    const bool local = self && xuids[i] == self;
    entries.push_back({xuids[i], i < private_slots.size() && private_slots[i], local, 0});
  }
  if (r == hr::kOk) r = impl_->Join(handle, entries, f);
  LogF(LogLevel::kDebug, "xlive: SessionJoinRemote(0x%08X) -> %s", handle, HrText(r).c_str());
  impl_->TraceOp("xlive.session.join", f.Hr(r));
  return r;
}

uint32_t XLive::SessionLeaveLocal(SessionHandle handle, const std::vector<uint32_t>& user_indices) {
  LogF(LogLevel::kDebug, "xlive: SessionLeaveLocal(0x%08X, %zu users)", handle,
       user_indices.size());
  Fields f;
  f.U("handle", handle).B("local", true);
  uint32_t r = hr::kOk;
  const Xuid xuid = impl_->LocalXuid();
  std::vector<Xuid> xuids;
  if (user_indices.empty()) r = hr::kInvalidArg;
  for (uint32_t idx : user_indices) {
    if (idx != 0 || !xuid) {
      r = hr::kInvalidArg;
      break;
    }
    xuids.push_back(xuid);
  }
  if (r == hr::kOk) r = impl_->Leave(handle, xuids, f);
  impl_->TraceOp("xlive.session.leave", f.Hr(r));
  return r;
}

uint32_t XLive::SessionLeaveRemote(SessionHandle handle, const std::vector<Xuid>& xuids) {
  LogF(LogLevel::kDebug, "xlive: SessionLeaveRemote(0x%08X, %zu xuids)", handle, xuids.size());
  Fields f;
  f.U("handle", handle).B("local", false);
  const uint32_t r = xuids.empty() ? hr::kInvalidArg : impl_->Leave(handle, xuids, f);
  impl_->TraceOp("xlive.session.leave", f.Hr(r));
  return r;
}

uint32_t XLive::SessionSearch(uint32_t procedure_index, uint32_t user_index, uint32_t max_results,
                              uint32_t num_users, const std::vector<Property>& properties,
                              const std::vector<Context>& contexts,
                              std::vector<SearchResult>* out) {
  LogF(LogLevel::kDebug,
       "xlive: SessionSearch(proc=%u, user=%u, max=%u, users=%u, %zu props, %zu contexts)",
       procedure_index, user_index, max_results, num_users, properties.size(), contexts.size());
  Fields f;
  f.U("proc", procedure_index);
  const uint32_t r = impl_->Search(procedure_index, user_index, max_results, num_users, properties,
                                   contexts, out, f);
  LogF(LogLevel::kDebug, "xlive: SessionSearch -> %s, %zu results", HrText(r).c_str(),
       out ? out->size() : size_t{0});
  if (r != hr::kOk) f.U("count", 0);
  impl_->TraceOp("xlive.session.search", f.Hr(r));
  return r;
}

uint32_t XLive::SessionArbitrationRegister(SessionHandle handle, uint32_t flags, uint64_t nonce,
                                           uint32_t duration_s, std::vector<Registrant>* out) {
  LogF(LogLevel::kDebug,
       "xlive: SessionArbitrationRegister(0x%08X, flags=0x%08X, nonce=%016llx, %u s)", handle,
       flags, (unsigned long long)nonce, duration_s);
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->Arbitrate(handle, nonce, out, f);
  LogF(LogLevel::kDebug, "xlive: SessionArbitrationRegister(0x%08X) -> %s", handle,
       HrText(r).c_str());
  impl_->TraceOp("xlive.session.arbitrate", f.Hr(r));
  return r;
}

uint32_t XLive::SessionWriteStats(SessionHandle handle, Xuid xuid,
                                  const std::vector<StatsWriteView>& views) {
  LogF(LogLevel::kDebug, "xlive: SessionWriteStats(0x%08X, %016llx, %zu views)", handle,
       (unsigned long long)xuid, views.size());
  Fields f;
  f.U("handle", handle);
  const uint32_t r = impl_->WriteStats(handle, xuid, views, f);
  impl_->TraceOp("xlive.stats.write", f.Hr(r));
  return r;
}

uint32_t XLive::SessionFlushStats(SessionHandle handle) {
  LogF(LogLevel::kDebug, "xlive: SessionFlushStats(0x%08X)", handle);
  Fields f;
  f.U("handle", handle);
  return impl_->Flush(handle, f);
}

bool XLive::GetSessionState(SessionHandle handle, SessionState* out) const {
  if (!out) return false;
  std::lock_guard lk(impl_->mu);
  const auto it = impl_->sessions.find(handle);
  if (it == impl_->sessions.end()) return false;
  *out = it->second->st;
  return true;
}

uint32_t XLive::StatsRead(const std::vector<Xuid>& xuids, const std::vector<StatsSpec>& specs,
                          std::vector<StatsView>* out) {
  LogF(LogLevel::kDebug, "xlive: StatsRead(%zu xuids, %zu specs)", xuids.size(), specs.size());
  Fields f;
  const uint32_t r = impl_->StatsRead(xuids, specs, out, f);
  impl_->TraceOp("xlive.stats.read", f.Hr(r));
  return r;
}

uint32_t XLive::StatsEnumerate(const StatsSpec& spec, StatsPivot pivot, uint64_t pivot_value,
                               uint32_t num_rows, StatsView* out) {
  LogF(LogLevel::kDebug, "xlive: StatsEnumerate(view=%u, pivot=%d, value=%016llx, rows=%u)",
       spec.view_id, int(pivot), (unsigned long long)pivot_value, num_rows);
  Fields f;
  const uint32_t r = impl_->StatsEnumerate(spec, pivot, pivot_value, num_rows, out, f);
  impl_->TraceOp("xlive.stats.enumerate", f.Hr(r));
  return r;
}

void XLive::OnConnectionLost() {
  LogF(LogLevel::kInfo, "xlive: server connection lost; marking sessions stale");
  size_t n = 0;
  {
    std::lock_guard lk(impl_->mu);
    for (auto& [h, rec] : impl_->sessions) {
      if (rec->server_backed && !rec->stale) {
        rec->stale = true;
        ++n;
      }
    }
    impl_->attrs_dirty = false;
  }
  impl_->TraceOp("xlive.connection_lost", Fields().U("sessions", n));
}

}  // namespace ac6net::xlive
