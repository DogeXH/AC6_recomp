/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if REX_HAVE_AC6NET
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include <ac6net/log.h>

#include <rex/string.h>

#include "../netplay_glue.h"
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

#if REX_HAVE_AC6NET
namespace {

constexpr uint32_t kAsyncTaskMessageSize = 0x28;  // XLIVEBASE_ASYNC_MESSAGE
constexpr uint32_t kBuildServerPathSize = 0x28;   // X_STORAGE_BUILD_SERVER_PATH
constexpr uint32_t kMaxItemNameChars = 256;
constexpr uint32_t kUserIndexNone = 0xFE;

// XStorageBuildServerPath (0x58035), following Xenia Netplay xlivebase_app.cc:1948-2120
// (Copyright 2026 Xenia Canary, BSD-3-Clause) for the request layout and the guest path forms.
// There is no storage backend in v1, so nothing is created on a server.
X_HRESULT StorageBuildServerPath(KernelState* kernel_state, uint32_t buffer_ptr) {
  auto* memory = kernel_state->memory();
  if (!rex::netplay::GuestRangeValid(memory, buffer_ptr, kBuildServerPathSize)) {
    return X_E_INVALIDARG;
  }
  const uint8_t* req = memory->TranslateVirtual(buffer_ptr);
  const uint32_t user_index = memory::load_and_swap<uint32_t>(req + 0);
  const uint64_t request_xuid = memory::load_and_swap<uint64_t>(req + 8);
  const uint32_t facility = memory::load_and_swap<uint32_t>(req + 16);
  const uint32_t info_ptr = memory::load_and_swap<uint32_t>(req + 20);
  const uint32_t info_size = memory::load_and_swap<uint32_t>(req + 24);
  const uint32_t file_name_ptr = memory::load_and_swap<uint32_t>(req + 28);
  const uint32_t server_path_ptr = memory::load_and_swap<uint32_t>(req + 32);
  const uint32_t server_path_length_ptr = memory::load_and_swap<uint32_t>(req + 36);

  if (user_index >= 4 && user_index != kUserIndexNone) {
    return X_E_INVALIDARG;
  }
  if (!rex::netplay::SignedInToLive()) {
    return ac6net::hr::kNotLoggedOn;
  }
  uint64_t xuid = request_xuid;
  if (user_index != kUserIndexNone) {
    if (user_index != 0) {
      return ac6net::hr::kNotLoggedOn;
    }
    xuid = rex::netplay::OnlineXuid();
  }
  if (!xuid || !rex::netplay::GuestRangeValid(memory, server_path_length_ptr, 4)) {
    return X_E_INVALIDARG;
  }

  std::u16string file_name;
  if (file_name_ptr &&
      !rex::netplay::ReadGuestString16(memory, file_name_ptr, kMaxItemNameChars, &file_name)) {
    return X_E_INVALIDARG;
  }
  const uint32_t title_id = kernel_state->title_id();
  std::string path;
  switch (facility) {
    case 1: {  // FACILITY_GAME_CLIP: info is {leaderboard_id}
      if (!rex::netplay::GuestRangeValid(memory, info_ptr, 4) || info_size != 4) {
        return X_E_INVALIDARG;
      }
      const uint32_t view_id = memory::load_and_swap<uint32_t>(memory->TranslateVirtual(info_ptr));
      path = fmt::format("//xestats/u:{:016x}/{:08x}/{:08x}", xuid, title_id, view_id);
      break;
    }
    case 2:  // FACILITY_PER_TITLE
      if (info_ptr || info_size || file_name.empty()) {
        return X_E_INVALIDARG;
      }
      path = fmt::format("//title.{:08x}/t:{:08x}/{}", title_id, title_id,
                         rex::string::to_utf8(file_name));
      break;
    case 3:  // FACILITY_PER_USER_TITLE
      if (info_ptr || info_size || file_name.empty()) {
        return X_E_INVALIDARG;
      }
      path = fmt::format("//tuser.{:08x}/u:{:016x}/{:08x}/{}", title_id, xuid, title_id,
                         rex::string::to_utf8(file_name));
      break;
    default:
      return X_E_INVALIDARG;
  }

  const std::u16string guest_path = rex::string::to_utf16(path);
  const uint32_t needed_chars = static_cast<uint32_t>(guest_path.size()) + 1;
  uint8_t* length_out = memory->TranslateVirtual(server_path_length_ptr);
  if (server_path_ptr) {
    // The in/out length counts characters, including the terminator.
    const uint32_t capacity = memory::load_and_swap<uint32_t>(length_out);
    if (capacity < needed_chars ||
        !rex::netplay::GuestRangeValid(memory, server_path_ptr, needed_chars * 2)) {
      return X_E_INVALIDARG;
    }
    uint8_t* out = memory->TranslateVirtual(server_path_ptr);
    for (uint32_t i = 0; i < needed_chars; ++i) {
      const uint16_t c = i < guest_path.size() ? static_cast<uint16_t>(guest_path[i]) : 0;
      memory::store_and_swap<uint16_t>(out + i * 2, c);
    }
  }
  memory::store_and_swap<uint32_t>(length_out, needed_chars);
  REXKRNL_INFO("netplay XStorageBuildServerPath(facility {}) -> {}", facility, path);
  return X_E_SUCCESS;
}

// XUserMuteListQuery (0x5800E): arg1 = {user_index, pad, u64 remote_xuid, result}, arg2 = BOOL*
// (AC6 wrapper rex_sub_821FE278). Nobody is muted in v1.
X_HRESULT MuteListQuery(KernelState* kernel_state, uint32_t request_ptr, uint32_t out_ptr) {
  auto* memory = kernel_state->memory();
  if (!rex::netplay::GuestRangeValid(memory, request_ptr, 0x10) ||
      !rex::netplay::GuestRangeValid(memory, out_ptr, 4)) {
    return X_E_INVALIDARG;
  }
  const uint32_t user_index = memory::load_and_swap<uint32_t>(memory->TranslateVirtual(request_ptr));
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(out_ptr), 0);
  return X_E_SUCCESS;
}

// Generic marshalled 0x0005xxxx task with a 40-byte XLIVEBASE_ASYNC_MESSAGE. AC6 sends one
// (rex_sub_821FDF98, most likely XStorageDownloadToMemory 0x50009) whose low 16 bits come from
// the title's schema table. No storage exists in v1, so it fails the way titles expect a missing
// file: X_ERROR_FUNCTION_FAILED with X_ONLINE_E_STORAGE_FILE_NOT_FOUND as the extended error
// (Xenia xlivebase_app.cc:1647-1651).
X_HRESULT AsyncTaskNotFound(uint32_t message) {
  static std::mutex mutex;
  static std::unordered_set<uint32_t> logged;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (logged.insert(message).second) {
      REXKRNL_WARN("netplay: XLiveBase async task {:08X} -> storage file not found", message);
    }
  }
  if (ac6net::TraceEnabled()) {
    ac6net::Trace("xlivebase.async", fmt::format("\"msg\":\"{:08X}\"", message));
  }
  rex::netplay::SetAsyncCompletion(X_ERROR_FUNCTION_FAILED, ac6net::hr::kStorageFileNotFound);
  return ac6net::hr::kStorageFileNotFound;
}

}  // namespace
#endif  // REX_HAVE_AC6NET

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
#if REX_HAVE_AC6NET
  if (rex::netplay::Enabled()) {
    switch (message) {
      case 0x0005800E:
        return MuteListQuery(kernel_state_, buffer_ptr, buffer_length);
      case 0x00058035:
        return StorageBuildServerPath(kernel_state_, buffer_ptr);
      default:
        if ((message & 0xFFFF0000) == 0x00050000 && buffer_length == kAsyncTaskMessageSize) {
          return AsyncTaskNotFound(message);
        }
        // 0x58004 (logon ID 1) and 0x58023 (no accepted invite: fail) keep their cases below.
        break;
    }
  }
#endif
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
