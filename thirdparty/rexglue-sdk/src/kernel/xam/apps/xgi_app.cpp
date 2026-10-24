/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if REX_HAVE_AC6NET
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <ac6net/log.h>

#include "../netplay_glue.h"
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

#if REX_HAVE_AC6NET
namespace {

namespace xlive = ac6net::xlive;
namespace hr = ac6net::hr;
namespace marshal = rex::netplay::marshal;

// Request layouts (big-endian), from Xenia Netplay xsession.h:79-218 and re/xmsg_map_verify.md
// sections 1 (C5-C10) and 2.1. Offsets are noted at each read.
constexpr uint32_t kSetContextSize = 0x18;     // {user_index, pad, u64 xuid, id, value}
constexpr uint32_t kSetPropertySize = 0x20;    // {user_index, pad, u64 xuid, id, size, ptr, pad}
constexpr uint32_t kSessionCreateSize = 0x1C;  // XGI_SESSION_CREATE
constexpr uint32_t kSessionStateSize = 0x10;   // XGI_SESSION_STATE {obj, flags, u64 nonce}
constexpr uint32_t kSessionManageSize = 0x14;  // XGI_SESSION_MANAGE
constexpr uint32_t kSessionModifySize = 0x10;  // XGI_SESSION_MODIFY
constexpr uint32_t kArbitrationSize = 0x20;    // XGI_SESSION_ARBITRATION
constexpr uint32_t kSearchExSize = 0x24;       // XGI_SESSION_SEARCH_EX
constexpr uint32_t kWriteStatsSize = 0x18;     // XGI_STATS_WRITE {obj, pad, u64 xuid, n, ptr}

constexpr uint32_t kViewPropertiesSize = 0x0C; // XSESSION_VIEW_PROPERTIES

constexpr uint32_t kMaxManageCount = 32;
constexpr uint32_t kMaxFilterCount = 64;
constexpr uint32_t kMaxStatsViews = 64;
constexpr uint32_t kMaxViewProperties = 64;
constexpr uint32_t kMaxSearchResults = 200;

// JSONL trace event for the test harness: {"ev":"xgi.<name>","handle":"..","hr":"..",...}.
void TraceXgi(const char* event, uint32_t handle, uint32_t result, const std::string& extra = {}) {
  if (!ac6net::TraceEnabled()) {
    return;
  }
  ac6net::Trace(event, fmt::format("\"handle\":\"{:08X}\",\"hr\":\"{:08X}\"{}{}", handle, result,
                                   extra.empty() ? "" : ",", extra));
}

class XgiNetplay {
 public:
  XgiNetplay(KernelState* kernel_state, uint32_t buffer_ptr, uint32_t buffer_length)
      : kernel_state_(kernel_state),
        memory_(kernel_state->memory()),
        buffer_ptr_(buffer_ptr),
        buffer_length_(buffer_length) {}

  // Returns false if `message` is not one the netplay glue handles.
  bool Dispatch(uint32_t message, X_HRESULT* result) {
    switch (message) {
      case 0x000B0006:
        *result = SetContext();
        return true;
      case 0x000B0007:
        *result = SetProperty();
        return true;
      case 0x000B0010:
        *result = SessionCreate();
        return true;
      case 0x000B0011:
        *result = SessionState(message);
        return true;
      case 0x000B0012:
        *result = SessionManage(true);
        return true;
      case 0x000B0013:
        *result = SessionManage(false);
        return true;
      case 0x000B0014:
      case 0x000B0015:
        *result = SessionState(message);
        return true;
      case 0x000B0018:
        *result = SessionModify();
        return true;
      case 0x000B001A:
        *result = ArbitrationRegister();
        return true;
      case 0x000B001C:
        *result = SearchEx();
        return true;
      case 0x000B0025:
        *result = WriteStats();
        return true;
      default:
        return false;
    }
  }

 private:
  // The request struct must be readable; a non-zero buffer_length must cover it.
  const uint8_t* Request(uint32_t size) const {
    if (buffer_length_ && buffer_length_ < size) {
      return nullptr;
    }
    if (!rex::netplay::GuestRangeValid(memory_, buffer_ptr_, size)) {
      return nullptr;
    }
    return memory_->TranslateVirtual(buffer_ptr_);
  }

  static uint32_t U32(const uint8_t* p, uint32_t offset) {
    return memory::load_and_swap<uint32_t>(p + offset);
  }
  static uint64_t U64(const uint8_t* p, uint32_t offset) {
    return memory::load_and_swap<uint64_t>(p + offset);
  }

  uint32_t Session(uint32_t obj_ptr) const {
    return rex::netplay::SessionHandleFromObject(kernel_state_, obj_ptr);
  }

  X_HRESULT SetContext() {
    const uint8_t* req = Request(kSetContextSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t user_index = U32(req, 0);
    const uint32_t context_id = U32(req, 16);
    const uint32_t value = U32(req, 20);
    REXKRNL_DEBUG("netplay XGIUserSetContext({}, {:08X}, {:08X})", user_index, context_id, value);
    rex::netplay::Live().UserSetContext(user_index, context_id, value);
    return X_E_SUCCESS;
  }

  X_HRESULT SetProperty() {
    const uint8_t* req = Request(kSetPropertySize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t user_index = U32(req, 0);
    const uint32_t property_id = U32(req, 16);
    const uint32_t size = U32(req, 20);
    const uint32_t data_ptr = U32(req, 24);
    if (!data_ptr) {
      // Xenia: some titles pass a null pointer for an unexpected property; accept and ignore.
      REXKRNL_DEBUG("netplay XGIUserSetPropertyEx({:08X}) without data", property_id);
      return X_E_SUCCESS;
    }
    ac6net::Property property;
    if (!rex::netplay::ReadRawPropertyValue(memory_, property_id, size, data_ptr, &property)) {
      REXKRNL_WARN("netplay XGIUserSetPropertyEx({:08X}, {}, {:08X}): unreadable value",
                   property_id, size, data_ptr);
      return X_E_INVALIDARG;
    }
    REXKRNL_DEBUG("netplay XGIUserSetPropertyEx({}, {:08X}, {})", user_index, property_id, size);
    rex::netplay::Live().UserSetProperty(user_index, property);
    return X_E_SUCCESS;
  }

  X_HRESULT SessionCreate() {
    const uint8_t* req = Request(kSessionCreateSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    const uint32_t flags = U32(req, 4);
    const uint32_t public_slots = U32(req, 8);
    const uint32_t private_slots = U32(req, 12);
    const uint32_t user_index = U32(req, 16);  // ReXGlue called this user_xuid; it is the index
    const uint32_t info_ptr = U32(req, 20);
    const uint32_t nonce_ptr = U32(req, 24);
    if (!handle || !rex::netplay::GuestRangeValid(memory_, info_ptr, rex::netplay::kSessionInfoSize) ||
        !rex::netplay::GuestRangeValid(memory_, nonce_ptr, 8)) {
      return X_E_INVALIDARG;
    }
    uint8_t* info_guest = memory_->TranslateVirtual(info_ptr);
    ac6net::SessionInfo info = rex::netplay::ReadSessionInfo(info_guest);
    uint64_t nonce = 0;
    const uint32_t result = rex::netplay::Live().SessionCreate(handle, flags, public_slots,
                                                               private_slots, user_index, &info,
                                                               &nonce);
    REXKRNL_INFO("netplay XSessionCreate(handle {:08X}, flags {:08X}, {}+{} slots, user {}) -> {:08X}",
                 handle, flags, public_slots, private_slots, user_index, result);
    TraceXgi("xgi.create", handle, result,
             fmt::format("\"flags\":{},\"public\":{},\"private\":{},\"host\":{}", flags,
                         public_slots, private_slots, (flags & ac6net::sflag::kHost) ? 1 : 0));
    if (result != hr::kOk) {
      return result;
    }
    rex::netplay::WriteSessionInfo(info_guest, info);
    if ((flags & ac6net::sflag::kHost) || nonce) {
      memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), nonce);
    }
    return X_E_SUCCESS;
  }

  // Delete (0xB0011), Start (0xB0014), End (0xB0015): XGI_SESSION_STATE.
  X_HRESULT SessionState(uint32_t message) {
    const uint8_t* req = Request(kSessionStateSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    const uint32_t flags = U32(req, 4);
    if (!handle) {
      return X_E_INVALIDARG;
    }
    uint32_t result = hr::kFail;
    const char* name = "";
    switch (message) {
      case 0x000B0011:
        name = "XSessionDelete";
        result = rex::netplay::Live().SessionDelete(handle);
        break;
      case 0x000B0014:
        name = "XSessionStart";
        result = rex::netplay::Live().SessionStart(handle, flags);
        break;
      case 0x000B0015:
        name = "XSessionEnd";
        result = rex::netplay::Live().SessionEnd(handle);
        break;
    }
    REXKRNL_INFO("netplay {}(handle {:08X}) -> {:08X}", name, handle, result);
    TraceXgi(message == 0x000B0011   ? "xgi.delete"
             : message == 0x000B0014 ? "xgi.start"
                                     : "xgi.end",
             handle, result);
    return result;
  }

  // Join (0xB0012) and Leave (0xB0013), local or remote by which array pointer is set
  // (Xenia xsession.cc:289-307).
  X_HRESULT SessionManage(bool join) {
    const uint8_t* req = Request(kSessionManageSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    const uint32_t count = U32(req, 4);
    const uint32_t xuids_ptr = U32(req, 8);
    const uint32_t indices_ptr = U32(req, 12);
    const uint32_t private_ptr = join ? U32(req, 16) : 0;
    if (!handle || !count || count > kMaxManageCount || (!xuids_ptr && !indices_ptr)) {
      return X_E_INVALIDARG;
    }
    std::vector<bool> private_slots(count, false);
    if (private_ptr) {
      if (!rex::netplay::GuestRangeValid(memory_, private_ptr, count * 4)) {
        return X_E_INVALIDARG;
      }
      const uint8_t* p = memory_->TranslateVirtual(private_ptr);
      for (uint32_t i = 0; i < count; ++i) {
        private_slots[i] = U32(p, i * 4) != 0;
      }
    }
    auto& live = rex::netplay::Live();
    uint32_t result;
    if (xuids_ptr) {
      if (!rex::netplay::GuestRangeValid(memory_, xuids_ptr, count * 8)) {
        return X_E_INVALIDARG;
      }
      const uint8_t* p = memory_->TranslateVirtual(xuids_ptr);
      std::vector<ac6net::Xuid> xuids(count);
      for (uint32_t i = 0; i < count; ++i) {
        xuids[i] = U64(p, i * 8);
      }
      result = join ? live.SessionJoinRemote(handle, xuids, private_slots)
                    : live.SessionLeaveRemote(handle, xuids);
      REXKRNL_INFO("netplay XSession{}Remote(handle {:08X}, {} users, first {:016X}) -> {:08X}",
                   join ? "Join" : "Leave", handle, count, xuids[0], result);
      TraceXgi(join ? "xgi.join" : "xgi.leave", handle, result,
               fmt::format("\"remote\":1,\"count\":{},\"xuid\":\"{:016x}\"", count, xuids[0]));
    } else {
      if (!rex::netplay::GuestRangeValid(memory_, indices_ptr, count * 4)) {
        return X_E_INVALIDARG;
      }
      const uint8_t* p = memory_->TranslateVirtual(indices_ptr);
      std::vector<uint32_t> indices(count);
      for (uint32_t i = 0; i < count; ++i) {
        indices[i] = U32(p, i * 4);
      }
      result = join ? live.SessionJoinLocal(handle, indices, private_slots)
                    : live.SessionLeaveLocal(handle, indices);
      REXKRNL_INFO("netplay XSession{}Local(handle {:08X}, {} users) -> {:08X}",
                   join ? "Join" : "Leave", handle, count, result);
      TraceXgi(join ? "xgi.join" : "xgi.leave", handle, result,
               fmt::format("\"remote\":0,\"count\":{}", count));
    }
    return result;
  }

  X_HRESULT SessionModify() {
    const uint8_t* req = Request(kSessionModifySize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    if (!handle) {
      return X_E_INVALIDARG;
    }
    const uint32_t flags = U32(req, 4);
    const uint32_t public_slots = U32(req, 8);
    const uint32_t private_slots = U32(req, 12);
    const uint32_t result =
        rex::netplay::Live().SessionModify(handle, flags, public_slots, private_slots);
    REXKRNL_INFO("netplay XSessionModify(handle {:08X}, flags {:08X}, {}+{}) -> {:08X}", handle,
                 flags, public_slots, private_slots, result);
    TraceXgi("xgi.modify", handle, result, fmt::format("\"flags\":{}", flags));
    return result;
  }

  // XSESSION_REGISTRATION_RESULTS {count, registrants_ptr}, then XSESSION_REGISTRANT[count]
  // {u64 machine_id, trustworthiness, num_users, users_ptr, pad}, then each registrant's XUIDs,
  // all inside results_buffer_size (Xenia xsession.cc:759-811 allocates them on the system
  // heap instead). AC6's wrapper (rex_sub_821FDA68) answers the size query itself
  // (*pcb < 3592 -> ERROR_INSUFFICIENT_BUFFER) and only sends the fill call.
  X_HRESULT ArbitrationRegister() {
    const uint8_t* req = Request(kArbitrationSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    const uint32_t flags = U32(req, 4);
    const uint64_t nonce = U64(req, 8);
    const uint32_t duration = U32(req, 16);
    const uint32_t results_size = U32(req, 20);
    const uint32_t results_ptr = U32(req, 24);
    if (!handle || !results_ptr) {
      return X_E_INVALIDARG;
    }
    if (results_size < marshal::kRegistrationResultsSize) {
      return hr::kInsufficientBuffer;
    }
    std::vector<xlive::Registrant> registrants;
    const uint32_t result = rex::netplay::Live().SessionArbitrationRegister(handle, flags, nonce,
                                                                            duration, &registrants);
    REXKRNL_INFO("netplay XSessionArbitrationRegister(handle {:08X}, nonce {:016X}) -> {:08X}, {} "
                 "machines",
                 handle, nonce, result, registrants.size());
    TraceXgi("xgi.arbitrate", handle, result,
             fmt::format("\"machines\":{}", registrants.size()));
    if (result != hr::kOk) {
      return result;
    }
    auto arena = rex::netplay::MakeArena(memory_, results_ptr, results_size);
    if (!arena.size()) {
      return X_E_INVALIDARG;
    }
    arena.Reset();
    if (!marshal::LayoutRegistrationResults(arena, registrants)) {
      arena.Reset();
      return hr::kInsufficientBuffer;
    }
    return X_E_SUCCESS;
  }

  X_HRESULT SearchEx() {
    const uint8_t* req = Request(kSearchExSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t proc_index = U32(req, 0);
    const uint32_t user_index = U32(req, 4);
    const uint32_t max_results = std::min(U32(req, 8), kMaxSearchResults);
    const uint32_t num_props = memory::load_and_swap<uint16_t>(req + 12);
    const uint32_t num_ctx = memory::load_and_swap<uint16_t>(req + 14);
    const uint32_t props_ptr = U32(req, 16);
    const uint32_t ctx_ptr = U32(req, 20);
    const uint32_t results_size = U32(req, 24);
    const uint32_t results_ptr = U32(req, 28);
    const uint32_t num_users = U32(req, 32);
    if (!results_ptr || num_props > kMaxFilterCount || num_ctx > kMaxFilterCount) {
      return X_E_INVALIDARG;
    }

    std::vector<ac6net::Property> properties;
    if (num_props) {
      if (!rex::netplay::GuestRangeValid(memory_, props_ptr,
                                         num_props * rex::netplay::kUserPropertySize)) {
        return X_E_INVALIDARG;
      }
      for (uint32_t i = 0; i < num_props; ++i) {
        ac6net::Property property;
        if (!rex::netplay::ReadUserProperty(
                memory_, props_ptr + i * rex::netplay::kUserPropertySize, &property)) {
          return X_E_INVALIDARG;
        }
        properties.push_back(std::move(property));
      }
    }
    std::vector<ac6net::Context> contexts;
    if (num_ctx) {
      if (!rex::netplay::GuestRangeValid(memory_, ctx_ptr,
                                         num_ctx * rex::netplay::kUserContextSize)) {
        return X_E_INVALIDARG;
      }
      const uint8_t* p = memory_->TranslateVirtual(ctx_ptr);
      for (uint32_t i = 0; i < num_ctx; ++i) {
        contexts.push_back({U32(p, i * 8), U32(p, i * 8 + 4)});
      }
    }

    auto arena = rex::netplay::MakeArena(memory_, results_ptr, results_size);
    if (arena.size() < marshal::kSearchHeaderSize) {
      // AC6's wrapper (rex_sub_821FDC88) turns this into ERROR_INSUFFICIENT_BUFFER with *pcb set.
      return hr::kSessionInsufficientBuffer;
    }

    std::vector<xlive::SearchResult> results;
    const uint32_t result = rex::netplay::Live().SessionSearch(
        proc_index, user_index, max_results, num_users, properties, contexts, &results);
    REXKRNL_INFO("netplay XSessionSearchEx(proc {}, max {}, users {}, {} props, {} ctx) -> {:08X}, "
                 "{} results",
                 proc_index, max_results, num_users, num_props, num_ctx, result, results.size());
    TraceXgi("xgi.search", 0, result,
             fmt::format("\"proc\":{},\"results\":{}", proc_index, results.size()));
    if (result != hr::kOk) {
      arena.Reset();
      return result;
    }

    // Keep as many whole results as fit (Xenia writes past a short buffer; this never does).
    const int count = marshal::LayoutSearchResultsFitting(arena, results, max_results);
    if (count < 0) {
      return hr::kSessionInsufficientBuffer;
    }
    if (static_cast<size_t>(count) < std::min<size_t>(results.size(), max_results)) {
      REXKRNL_WARN("netplay XSessionSearchEx: {} of {} results fit in {} bytes", count,
                   results.size(), results_size);
    }
    return X_E_SUCCESS;
  }

  X_HRESULT WriteStats() {
    const uint8_t* req = Request(kWriteStatsSize);
    if (!req) {
      return X_E_INVALIDARG;
    }
    const uint32_t handle = Session(U32(req, 0));
    const uint64_t xuid = U64(req, 8);
    const uint32_t num_views = U32(req, 16);
    const uint32_t views_ptr = U32(req, 20);
    if (!handle || num_views > kMaxStatsViews ||
        (num_views && !rex::netplay::GuestRangeValid(memory_, views_ptr,
                                                     num_views * kViewPropertiesSize))) {
      return X_E_INVALIDARG;
    }
    std::vector<xlive::StatsWriteView> views;
    for (uint32_t v = 0; v < num_views; ++v) {
      const uint8_t* view = memory_->TranslateVirtual(views_ptr + v * kViewPropertiesSize);
      xlive::StatsWriteView out;
      out.view_id = U32(view, 0);
      const uint32_t count = U32(view, 4);
      const uint32_t properties_ptr = U32(view, 8);
      if (count > kMaxViewProperties ||
          (count && !rex::netplay::GuestRangeValid(memory_, properties_ptr,
                                                   count * rex::netplay::kUserPropertySize))) {
        return X_E_INVALIDARG;
      }
      for (uint32_t p = 0; p < count; ++p) {
        ac6net::Property property;
        if (!rex::netplay::ReadUserProperty(
                memory_, properties_ptr + p * rex::netplay::kUserPropertySize, &property)) {
          return X_E_INVALIDARG;
        }
        out.properties.push_back(std::move(property));
      }
      views.push_back(std::move(out));
    }
    const uint32_t result = rex::netplay::Live().SessionWriteStats(handle, xuid, views);
    REXKRNL_INFO("netplay XSessionWriteStats(handle {:08X}, xuid {:016X}, {} views) -> {:08X}",
                 handle, xuid, num_views, result);
    TraceXgi("xgi.write_stats", handle, result,
             fmt::format("\"xuid\":\"{:016x}\",\"views\":{}", xuid, num_views));
    return result;
  }

  KernelState* kernel_state_;
  memory::Memory* memory_;
  uint32_t buffer_ptr_;
  uint32_t buffer_length_;
};

}  // namespace
#endif  // REX_HAVE_AC6NET

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
#if REX_HAVE_AC6NET
  if (rex::netplay::Enabled()) {
    X_HRESULT result = X_E_FAIL;
    if (XgiNetplay(kernel_state_, buffer_ptr, buffer_length).Dispatch(message, &result)) {
      return result;
    }
  }
#endif
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      REXKRNL_DEBUG("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
                    achievements_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 0x18);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      // TODO(PermaNull): reverse buffer contents.
      REXKRNL_DEBUG("XGISessionDelete");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t unk_0 = memory::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 0x10);

      assert_zero(unk_0);
      REXKRNL_DEBUG("XGISessionJoinLocal({:08X}, {}, {}, {:08X}, {:08X})", session_ptr, user_count,
                    unk_0, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      REXKRNL_DEBUG("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      REXKRNL_DEBUG("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}{:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
