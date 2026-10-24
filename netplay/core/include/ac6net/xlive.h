// ac6net - XGI-level Live emulation: users, contexts/properties, sessions, search, arbitration,
// stats. The XAM glue parses guest structures, calls these, and writes results back.
// All calls are synchronous; the ones that talk to the server block (RPC) and must run on a
// worker thread when the guest supplied an XOVERLAPPED.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ac6net/types.h"

namespace ac6net {
class Netplay;
}

namespace ac6net::xlive {

// Guest handle of the XamSession object (from XamSessionCreateHandle).
using SessionHandle = uint32_t;

enum class SessionPhase { kNone, kLobby, kRegistration, kInGame, kReporting, kDeleted };

struct SessionMember {
  Xuid xuid = 0;
  bool is_private = false;
  bool local = false;        // joined through JoinLocal on this machine
  uint32_t user_index = 0;   // valid when local
};

struct SessionState {
  SessionHandle handle = 0;
  bool created = false;
  bool is_host = false;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  SessionInfo info;           // XNKID / host XNADDR / XNKEY
  uint64_t nonce = 0;
  uint32_t host_vip = 0;
  SessionPhase phase = SessionPhase::kNone;
  std::vector<SessionMember> members;
};

struct SearchResult {  // XSESSION_SEARCHRESULT contents
  SessionInfo info;
  uint32_t open_public = 0, open_private = 0, filled_public = 0, filled_private = 0;
  std::vector<Property> properties;
  std::vector<Context> contexts;
};

struct Registrant {  // XSESSION_REGISTRANT
  uint64_t machine_id = 0;
  uint8_t trustworthiness = 1;
  std::vector<Xuid> users;
};

struct StatsSpec {  // XUSER_STATS_SPEC
  uint32_t view_id = 0;
  std::vector<uint16_t> column_ids;
};

struct StatsColumn {  // XUSER_STATS_COLUMN
  uint16_t column_id = 0;
  UserData value;
};

struct StatsRow {  // XUSER_STATS_ROW
  Xuid xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string gamertag;
  std::vector<StatsColumn> columns;
};

struct StatsView {  // XUSER_STATS_VIEW
  uint32_t view_id = 0;
  uint32_t total_rows = 0;
  std::vector<StatsRow> rows;
};

struct StatsWriteView {  // XSESSION_VIEW_PROPERTIES
  uint32_t view_id = 0;
  std::vector<Property> properties;
};

enum class StatsPivot { kXuid = 0, kRank = 1, kRating = 2 };

class XLiveBackend;  // internal seam (src/xlive_backend.h)

class XLive {
 public:
  explicit XLive(Netplay* netplay);
  ~XLive();
  XLive(const XLive&) = delete;
  XLive& operator=(const XLive&) = delete;

  // --- Local user (index 0 only in v1) ---
  // The glue reports the profile's offline XUID (content XUID) once at startup.
  void SetOfflineXuid(Xuid xuid);
  Xuid OfflineXuid() const;
  // True when the local user should appear signed in to Live (state 2).
  bool SignedInToLive() const;
  Xuid OnlineXuid() const;
  std::string Gamertag() const;

  void UserSetContext(uint32_t user_index, uint32_t context_id, uint32_t value);
  void UserSetProperty(uint32_t user_index, const Property& property);
  bool UserGetContext(uint32_t user_index, uint32_t context_id, uint32_t* value) const;
  bool UserGetProperty(uint32_t user_index, uint32_t property_id, Property* property) const;

  // --- Sessions. Every call returns an HRESULT (hr::kOk on success). ---
  void SessionHandleCreated(SessionHandle handle);
  void SessionHandleClosed(SessionHandle handle);

  // HOST flag set: creates a session on the server, fills *info (XNKID, own XNADDR, XNKEY) and
  // *nonce. HOST clear: joins the session named by info->session_id (looks it up on the server,
  // learns the host's vIP); *info is left as provided except host address normalization.
  uint32_t SessionCreate(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                         uint32_t private_slots, uint32_t user_index, SessionInfo* info,
                         uint64_t* nonce);
  uint32_t SessionDelete(SessionHandle handle);
  uint32_t SessionStart(SessionHandle handle, uint32_t flags);
  uint32_t SessionEnd(SessionHandle handle);
  uint32_t SessionModify(SessionHandle handle, uint32_t flags, uint32_t public_slots,
                         uint32_t private_slots);
  uint32_t SessionJoinLocal(SessionHandle handle, const std::vector<uint32_t>& user_indices,
                            const std::vector<bool>& private_slots);
  uint32_t SessionJoinRemote(SessionHandle handle, const std::vector<Xuid>& xuids,
                             const std::vector<bool>& private_slots);
  uint32_t SessionLeaveLocal(SessionHandle handle, const std::vector<uint32_t>& user_indices);
  uint32_t SessionLeaveRemote(SessionHandle handle, const std::vector<Xuid>& xuids);
  uint32_t SessionSearch(uint32_t procedure_index, uint32_t user_index, uint32_t max_results,
                         uint32_t num_users, const std::vector<Property>& properties,
                         const std::vector<Context>& contexts, std::vector<SearchResult>* out);
  uint32_t SessionArbitrationRegister(SessionHandle handle, uint32_t flags, uint64_t nonce,
                                      uint32_t duration_s, std::vector<Registrant>* out);
  uint32_t SessionWriteStats(SessionHandle handle, Xuid xuid,
                             const std::vector<StatsWriteView>& views);
  uint32_t SessionFlushStats(SessionHandle handle);
  bool GetSessionState(SessionHandle handle, SessionState* out) const;

  // --- Stats reads (XUserReadStats / XamUserCreateStatsEnumerator) ---
  // Always succeeds when signed in: missing rows come back as one row per requested XUID with
  // rank 0 and zero-valued columns (AC6 needs a result to host; see research/xenia_netplay.md).
  uint32_t StatsRead(const std::vector<Xuid>& xuids, const std::vector<StatsSpec>& specs,
                     std::vector<StatsView>* out);
  uint32_t StatsEnumerate(const StatsSpec& spec, StatsPivot pivot, uint64_t pivot_value,
                          uint32_t num_rows, StatsView* out);

  // Server connection lost: mark sessions stale (calls keep failing fast).
  void OnConnectionLost();

 private:
  friend std::unique_ptr<XLive> MakeXLiveForTest(XLiveBackend* backend);
  // Internal: XLive driven by `backend` (deleted with the XLive when `owns_backend`).
  XLive(XLiveBackend* backend, bool owns_backend);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ac6net::xlive
