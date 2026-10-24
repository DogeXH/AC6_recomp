/**
 * @file        netplay_glue.cpp
 * @brief       XAM-side glue between the ReXGlue kernel and the ac6net online core.
 *
 * Parts of this file port behaviour from Xenia Canary Netplay
 * (https://github.com/AdrianCassar/xenia-canary, netplay_canary_experimental @ 6696b72):
 * the async XMsg request copy (src/xenia/kernel/xam/app_manager.cc:54-91) and the X_KSESSION
 * kernel object (src/xenia/kernel/xsession.cc:30-40). Those parts are
 * Copyright 2026 Xenia Canary, released under the BSD-3-Clause license.
 */

#include "netplay_glue.h"

#if REX_HAVE_AC6NET

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include <ac6net/config.h>
#include <ac6net/log.h>

#include <native/platform.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(netplay_enabled, false, "Netplay",
                    "Enable AC6 online play through an ac6lobby server. Off: the runtime behaves "
                    "exactly as without netplay.");
REXCVAR_DEFINE_STRING(netplay_server, "127.0.0.1:47800", "Netplay",
                      "ac6lobby server as host:port (IPv4, [IPv6] or DNS name)");
REXCVAR_DEFINE_STRING(netplay_server_key, "", "Netplay",
                      "Pinned X25519 public key of the server (64 hex characters)");
REXCVAR_DEFINE_STRING(netplay_gamertag, "", "Netplay",
                      "Requested gamertag (at most 15 characters; the server may suffix it)");
REXCVAR_DEFINE_UINT32(netplay_port, 0, "Netplay", "Local UDP port for netplay; 0 = random");
REXCVAR_DEFINE_BOOL(netplay_force_relay, false, "Netplay",
                    "Never use direct peer paths; send all peer traffic through the relay");
REXCVAR_DEFINE_BOOL(netplay_privacy, false, "Netplay",
                    "Never reveal this machine's addresses to peers (implies relay-only paths)");
REXCVAR_DEFINE_BOOL(netplay_lan, true, "Netplay", "Offer LAN and loopback path candidates");
REXCVAR_DEFINE_STRING(netplay_trace, "", "Netplay", "Path of a JSONL netplay trace; empty = off");
REXCVAR_DEFINE_STRING(netplay_dump_spa, "", "Netplay",
                      "Write the title's SPA (XDBF) resource to this path at startup; empty = off");

namespace rex::netplay {
namespace {

using rex::system::KernelState;
using rex::system::XHostThread;
using rex::system::object_ref;

constexpr uint32_t kTitleIdAc6 = 0x4E4D07D1;
constexpr uint32_t kMaxAsyncRequestBytes = 64 * 1024;

// Notification IDs (Xenia xbox.h) and payloads.
constexpr uint32_t kXnSysUi = 0x00000009;
constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
constexpr uint32_t kXnLiveConnectionChanged = 0x02000001;
constexpr uint32_t kXnLiveLinkStateChanged = 0x02000003;
constexpr uint64_t kListenerMaskLive = 0x2;

// --- State ---------------------------------------------------------------------------------------

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_started{false};  // Startup ran (whatever the outcome)
KernelState* g_kernel_state = nullptr;
std::filesystem::path g_user_data_root;  // set by the KernelState constructor

// Last Live connection state the guest was told about (true = established).
std::mutex g_live_mutex;
bool g_live_announced = false;

// --- Glue workers --------------------------------------------------------------------------
//
// Three serial lanes, each one XHostThread with a FIFO queue: XGI requests, XLiveBase requests,
// and general tasks (state notifications, UI pulses). Requests of one app keep their submission
// order, as on Xenia's single dispatch thread, while a slow lobby RPC on one lane never delays
// the other lanes.

enum class Lane : uint32_t { kGeneral = 0, kXgi = 1, kXLiveBase = 2 };
constexpr uint32_t kLaneCount = 3;
constexpr const char* kLaneNames[kLaneCount] = {"General", "XGI", "XLiveBase"};

class WorkerQueue {
 public:
  bool Start(KernelState* kernel_state, const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_) {
      return true;
    }
    running_ = true;
    auto thread = object_ref<XHostThread>(new XHostThread(kernel_state, 128 * 1024, 0, [this]() {
      Run();
      return 0;
    }));
    thread->set_name(fmt::format("Netplay {}", name));
    if (XFAILED(thread->Create())) {
      REXKRNL_ERROR("netplay: could not create the {} glue worker", name);
      running_ = false;
      return false;
    }
    thread_ = std::move(thread);
    return true;
  }

  void Stop() {
    object_ref<XHostThread> thread;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      thread = std::move(thread_);
    }
    cond_.notify_all();
    if (thread) {
      thread->Wait(0, 0, 0, nullptr);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_.empty()) {
      REXKRNL_WARN("netplay: dropped {} queued glue tasks at shutdown", queue_.size());
      queue_.clear();
    }
  }

  bool Post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return false;
      }
      queue_.push_back(std::move(fn));
    }
    cond_.notify_one();
    return true;
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_) {
          return;
        }
        fn = std::move(queue_.front());
        queue_.pop_front();
      }
      fn();
    }
  }

  std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<std::function<void()>> queue_;
  object_ref<XHostThread> thread_;
  bool running_ = false;
};

class Workers {
 public:
  static Workers& Get() {
    static Workers workers;
    return workers;
  }

  bool Start(KernelState* kernel_state) {
    for (uint32_t i = 0; i < kLaneCount; ++i) {
      if (!lanes_[i].Start(kernel_state, kLaneNames[i])) {
        Stop();
        return false;
      }
    }
    return true;
  }

  void Stop() {
    for (auto& lane : lanes_) {
      lane.Stop();
    }
  }

  bool Post(Lane lane, std::function<void()> fn) {
    return lanes_[static_cast<uint32_t>(lane)].Post(std::move(fn));
  }

 private:
  WorkerQueue lanes_[kLaneCount];
};

// --- Async request bookkeeping ---------------------------------------------------------------

struct AsyncCompletion {
  bool set = false;
  uint32_t result = 0;
  uint32_t extended_error = 0;
};
thread_local AsyncCompletion t_async_completion;
thread_local bool t_in_async_handler = false;

std::mutex g_pending_mutex;
std::condition_variable g_pending_cond;
std::unordered_set<uint32_t> g_pending_overlapped;

// --- Notifications ---------------------------------------------------------------------------

void Broadcast(uint32_t id, uint32_t data) {
  if (g_kernel_state) {
    g_kernel_state->BroadcastNotification(id, data);
  }
}

// Runs on a glue worker. Tells the guest about Live connection transitions (section 5.1).
void ApplyNetState(ac6net::NetState state) {
  if (!g_enabled.load()) {
    return;
  }
  if (state == ac6net::NetState::kOffline) {
    ac6net::Netplay::Get().xlive().OnConnectionLost();
  }
  const bool live = state == ac6net::NetState::kOnline ||
                    (state == ac6net::NetState::kConnecting && SignedInToLive());
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    if (live != g_live_announced) {
      g_live_announced = live;
      changed = true;
    }
  }
  if (!changed) {
    return;
  }
  if (ac6net::TraceEnabled()) {
    ac6net::Trace("xam.live", live ? "\"live\":1" : "\"live\":0");
  }
  if (live) {
    REXKRNL_INFO("netplay: Live connection established");
    Broadcast(kXnLiveConnectionChanged, ac6net::hr::kLogonConnectionEstablished);
    Broadcast(kXnLiveLinkStateChanged, 1);
  } else {
    REXKRNL_WARN("netplay: Live connection lost");
    Broadcast(kXnLiveConnectionChanged, ac6net::hr::kLogonDisconnected);
  }
  Broadcast(kXnSysSigninChanged, 1);
}

// --- Startup helpers -------------------------------------------------------------------------

void InstallLogSink() {
  ac6net::SetLogSink([](ac6net::LogLevel level, std::string_view message) {
    switch (level) {
      case ac6net::LogLevel::kTrace:
      case ac6net::LogLevel::kDebug:
        REXKRNL_DEBUG("[netplay] {}", message);
        break;
      case ac6net::LogLevel::kInfo:
        REXKRNL_INFO("[netplay] {}", message);
        break;
      case ac6net::LogLevel::kWarn:
        REXKRNL_WARN("[netplay] {}", message);
        break;
      case ac6net::LogLevel::kError:
        REXKRNL_ERROR("[netplay] {}", message);
        break;
    }
  });
}

// The app's 60 fps unlock lives in the executable, so the SDK reads it through the cvar registry.
// Missing cvar (another app, or a build without the unlock) means "60", the AC6 recomp default.
std::string FpsMode() {
  if (!rex::cvar::GetFlagInfo("ac6_unlock_fps")) {
    return "60";
  }
  return rex::cvar::Query<bool>("ac6_unlock_fps") ? "60" : "30";
}

void DumpSpa(KernelState* kernel_state, const std::string& path) {
  auto module = kernel_state->GetExecutableModule();
  if (!module) {
    REXKRNL_WARN("netplay_dump_spa: no executable module");
    return;
  }
  uint32_t data = 0;
  uint32_t size = 0;
  const std::string section = fmt::format("{:08X}", module->title_id());
  if (XFAILED(module->GetSection(section.c_str(), &data, &size)) || !data || !size ||
      !GuestRangeValid(kernel_state->memory(), data, size)) {
    REXKRNL_WARN("netplay_dump_spa: title has no XDBF section {}", section);
    return;
  }
  std::ofstream out(rex::to_path(path), std::ios::binary | std::ios::trunc);  // UTF-8 cvar
  if (!out) {
    REXKRNL_ERROR("netplay_dump_spa: cannot open {}", path);
    return;
  }
  out.write(reinterpret_cast<const char*>(kernel_state->memory()->TranslateVirtual(data)), size);
  REXKRNL_INFO("netplay_dump_spa: wrote {} bytes of SPA {} to {}", size, section, path);
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------------------------

void SetUserDataRoot(const std::filesystem::path& user_data_root) {
  g_user_data_root = user_data_root;
}

void Startup(KernelState* kernel_state) {
  if (g_started.exchange(true)) {
    return;
  }
  g_kernel_state = kernel_state;

  if (!REXCVAR_GET(netplay_dump_spa).empty()) {
    DumpSpa(kernel_state, REXCVAR_GET(netplay_dump_spa));
  }
  if (!REXCVAR_GET(netplay_enabled)) {
    return;
  }

  auto module = kernel_state->GetExecutableModule();
  rex::xex2_opt_execution_info* exec_info = nullptr;
  if (module) {
    module->GetOptHeader(rex::XEX_HEADER_EXECUTION_INFO, &exec_info);
  }
  const uint32_t title_id = exec_info ? uint32_t(exec_info->title_id) : kTitleIdAc6;
  const uint32_t media_id = exec_info ? uint32_t(exec_info->media_id) : 0;
  const uint32_t version = exec_info ? uint32_t(exec_info->version_value) : 0;
  if (title_id != kTitleIdAc6) {
    REXKRNL_WARN("netplay: title {:08X} is not AC6 ({:08X}); sessions will only match the same title",
                 title_id, kTitleIdAc6);
  }

  ac6net::Config config;
  config.enabled = true;
  config.server = REXCVAR_GET(netplay_server);
  config.server_key_hex = REXCVAR_GET(netplay_server_key);
  config.gamertag = REXCVAR_GET(netplay_gamertag);
  if (REXCVAR_GET(netplay_port) > 0xFFFF) {
    REXKRNL_WARN("netplay_port {} is out of range; using a random port", REXCVAR_GET(netplay_port));
  } else {
    config.port = static_cast<uint16_t>(REXCVAR_GET(netplay_port));
  }
  config.force_relay = REXCVAR_GET(netplay_force_relay);
  config.privacy = REXCVAR_GET(netplay_privacy);
  config.lan = REXCVAR_GET(netplay_lan);
  config.trace_path = REXCVAR_GET(netplay_trace);
  config.title_id = fmt::format("{:08X}", title_id);
  const std::string fps_mode = FpsMode();
  config.compat_key =
      ac6net::ComputeCompatKey(title_id, media_id, version, ac6net::kProtocolVersion, fps_mode);

  std::filesystem::path root = g_user_data_root;
#if REX_PLATFORM_WIN32
  // Windows: the default user data root is Documents\<app>, which OneDrive (Known Folder Move)
  // syncs to the cloud and to every PC on the account, and which Defender Controlled Folder
  // Access may block for an unknown exe. The netplay identity (the account's secret key)
  // belongs to this machine: keep it in %LOCALAPPDATA%\<app>\netplay unless user_data_root
  // is set explicitly (portable installs, test instances), which keeps root\netplay.
  if (!root.empty() && REXCVAR_QUERY(std::string, user_data_root).empty()) {
    if (const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA");
        local_app_data && *local_app_data) {
      root = std::filesystem::path(local_app_data) / g_user_data_root.filename();
    }
  }
#endif
  std::error_code ec;
  if (!root.empty()) {
    root = std::filesystem::absolute(root, ec);
  }
  // UTF-8, not path::string(): on Windows that converts to the ANSI code page and throws for
  // a user profile path outside it (Cyrillic/CJK user names on a Western locale).
  config.data_dir = rex::path_to_utf8(root / "netplay");

  InstallLogSink();
  if (!Workers::Get().Start(kernel_state)) {
    REXKRNL_ERROR("netplay: no glue workers; netplay stays off");
    return;
  }

  auto& net = ac6net::Netplay::Get();
  // The callback runs on the ac6net network thread: never touch the kernel there.
  net.SetStateCallback([](ac6net::NetState state) {
    Post([state]() { ApplyNetState(state); });
  });

  REXKRNL_INFO(
      "netplay: starting (server {}, title {}, media {:08X}, version {:08X}, fps {}, compat {}, "
      "data {})",
      config.server, config.title_id, media_id, version, fps_mode, config.compat_key,
      config.data_dir);
  if (!net.Start(config)) {
    REXKRNL_ERROR("netplay: ac6net failed to start; netplay stays off");
    net.SetStateCallback(nullptr);
    Workers::Get().Stop();
    return;
  }
  net.xlive().SetOfflineXuid(kernel_state->user_profile()->xuid());
  g_enabled = true;

  // First run (no cached identity): give registration a moment so the title sees sign-in
  // state 2 at boot. With a cached identity this returns at once.
  if (!net.WaitRegistered(config.first_connect_wait_ms)) {
    REXKRNL_WARN("netplay: not registered yet; the title starts signed in locally");
  }
  const auto identity = net.identity();
  REXKRNL_INFO("netplay: identity {} gamertag '{}' xuid {:016X} vip {}",
               identity.valid ? "valid" : "pending", identity.gamertag, identity.xuid,
               ac6net::Ipv4ToString(identity.vip));
  Post([state = net.state()]() { ApplyNetState(state); });
}

void Shutdown() {
  if (!g_started.load()) {
    return;
  }
  const bool was_enabled = g_enabled.exchange(false);
  Workers::Get().Stop();
  if (was_enabled) {
    auto& net = ac6net::Netplay::Get();
    net.SetStateCallback(nullptr);
    net.Stop();
    REXKRNL_INFO("netplay: stopped");
  }
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_pending_overlapped.clear();
  }
  g_pending_cond.notify_all();
  g_kernel_state = nullptr;
  g_started = false;
}

bool Enabled() {
  return g_enabled.load(std::memory_order_acquire);
}

bool SignedInToLive() {
  // kOffline (server unreachable for > 30 s) signs the user out of Live even with a valid
  // identity, matching the DISCONNECTED + SIGNINCHANGED broadcast (ARCHITECTURE.md 5.1).
  if (!Enabled()) {
    return false;
  }
  auto& net = ac6net::Netplay::Get();
  return net.state() != ac6net::NetState::kOffline && net.xlive().SignedInToLive();
}

ac6net::xlive::XLive& Live() {
  return ac6net::Netplay::Get().xlive();
}

ac6net::Xuid OnlineXuid() {
  return Enabled() ? Live().OnlineXuid() : 0;
}

std::string Gamertag() {
  if (!Enabled()) {
    return {};
  }
  std::string tag = Live().Gamertag();
  if (tag.empty()) {
    tag = REXCVAR_GET(netplay_gamertag);
  }
  if (tag.size() > 15) {
    tag.resize(15);
  }
  return tag;
}

bool Post(std::function<void()> fn) {
  return Workers::Get().Post(Lane::kGeneral, std::move(fn));
}

// --- Async XMsg ------------------------------------------------------------------------------

bool ShouldDispatchAsync(uint32_t app_id, uint32_t message) {
  if (!Enabled()) {
    return false;
  }
  return (app_id == 0xFB && message != 0x000B0008) || app_id == 0xFC;
}

void SetAsyncCompletion(uint32_t result, uint32_t extended_error) {
  if (!t_in_async_handler) {
    return;
  }
  t_async_completion.set = true;
  t_async_completion.result = result;
  t_async_completion.extended_error = extended_error;
}

X_RESULT DispatchMessageAsync(KernelState* kernel_state, uint32_t app_id, uint32_t message,
                              uint32_t buffer_ptr, uint32_t buffer_length,
                              uint32_t overlapped_ptr) {
  auto* memory = kernel_state->memory();
  if (!GuestRangeValid(memory, overlapped_ptr, sizeof(system::XAM_OVERLAPPED))) {
    return X_E_INVALIDARG;
  }

  // Copy the request struct like Xenia does: the guest may reuse its buffer as soon as the call
  // returns IO_PENDING. Pointers inside the struct still refer to guest memory.
  uint32_t request_ptr = 0;
  if (buffer_ptr && buffer_length) {
    if (buffer_length > kMaxAsyncRequestBytes ||
        !GuestRangeValid(memory, buffer_ptr, buffer_length)) {
      kernel_state->CompleteOverlappedImmediate(overlapped_ptr, X_E_INVALIDARG);
      return X_ERROR_IO_PENDING;
    }
    request_ptr = memory->SystemHeapAlloc(buffer_length);
    if (!request_ptr) {
      kernel_state->CompleteOverlappedImmediate(overlapped_ptr, X_E_FAIL);
      return X_ERROR_IO_PENDING;
    }
    std::memcpy(memory->TranslateVirtual(request_ptr), memory->TranslateVirtual(buffer_ptr),
                buffer_length);
  } else {
    request_ptr = buffer_ptr;
  }
  const bool owns_copy = request_ptr && request_ptr != buffer_ptr;

  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    if (!g_pending_overlapped.insert(overlapped_ptr).second) {
      REXKRNL_WARN("netplay: overlapped {:08X} reused while its request is still pending",
                   overlapped_ptr);
    }
  }
  // Same bookkeeping CompleteOverlappedDeferredEx does at queue time: the APC (if any) targets
  // the requesting guest thread.
  auto* ovl = memory->TranslateVirtual(overlapped_ptr);
  system::XOverlappedSetResult(ovl, X_ERROR_IO_PENDING);
  system::XOverlappedSetContext(ovl, system::XThread::GetCurrentThreadHandle());

  auto run = [kernel_state, memory, app_id, message, request_ptr, buffer_length, overlapped_ptr,
              owns_copy]() {
    t_in_async_handler = true;
    t_async_completion = {};
    const X_HRESULT hr = kernel_state->app_manager()->DispatchMessageSync(app_id, message,
                                                                          request_ptr, buffer_length);
    t_in_async_handler = false;
    uint32_t result = hr;
    uint32_t extended_error = hr;
    if (t_async_completion.set) {
      result = t_async_completion.result;
      extended_error = t_async_completion.extended_error;
    }
    // Same length convention as CompleteOverlappedImmediate (some titles test it).
    const uint32_t length = result ? 0xFFFFFFFFu : 0u;
    kernel_state->CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
    if (owns_copy) {
      memory->SystemHeapFree(request_ptr);
    }
    {
      std::lock_guard<std::mutex> lock(g_pending_mutex);
      g_pending_overlapped.erase(overlapped_ptr);
    }
    g_pending_cond.notify_all();
  };

  const Lane lane = app_id == 0xFB ? Lane::kXgi : Lane::kXLiveBase;
  if (!Workers::Get().Post(lane, std::move(run))) {
    {
      std::lock_guard<std::mutex> lock(g_pending_mutex);
      g_pending_overlapped.erase(overlapped_ptr);
    }
    if (owns_copy) {
      memory->SystemHeapFree(request_ptr);
    }
    // Workers are gone (shutdown): complete at once so the guest never waits forever.
    kernel_state->CompleteOverlappedEx(overlapped_ptr, X_ERROR_FUNCTION_FAILED,
                                       X_ERROR_FUNCTION_FAILED, 0xFFFFFFFFu);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_IO_PENDING;
}

bool IsAsyncPending(uint32_t overlapped_ptr) {
  std::lock_guard<std::mutex> lock(g_pending_mutex);
  return g_pending_overlapped.count(overlapped_ptr) != 0;
}

bool WaitAsyncRequest(uint32_t overlapped_ptr, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(g_pending_mutex);
  return g_pending_cond.wait_for(lock, std::chrono::milliseconds(timeout_ms), [overlapped_ptr] {
    return g_pending_overlapped.count(overlapped_ptr) == 0;
  });
}

// --- Notifications ---------------------------------------------------------------------------

void OnNotifyListenerRegistered(system::XNotifyListener* listener) {
  if (!Enabled() || !listener || !(listener->mask() & kListenerMaskLive)) {
    return;
  }
  // Only non-blocking reads here: the kernel global lock is held.
  if (!SignedInToLive()) {
    return;
  }
  listener->EnqueueNotification(kXnLiveConnectionChanged,
                                ac6net::hr::kLogonConnectionEstablished);
  listener->EnqueueNotification(kXnLiveLinkStateChanged, 1);
}

void PulseSystemUi() {
  if (!Enabled()) {
    return;
  }
  Post([]() {
    Broadcast(kXnSysUi, 1);
    rex::thread::Sleep(std::chrono::milliseconds(100));
    Broadcast(kXnSysUi, 0);
  });
}

// --- Guest structure helpers -----------------------------------------------------------------

bool GuestRangeValid(memory::Memory* memory, uint32_t ptr, uint32_t size) {
  if (!memory || !ptr || !size) {
    return false;
  }
  const uint64_t end = uint64_t(ptr) + size - 1;
  if (end > 0xFFFFFFFFull) {
    return false;
  }
  return memory->LookupHeap(ptr) && memory->LookupHeap(static_cast<uint32_t>(end));
}

bool ReadGuestString16(memory::Memory* memory, uint32_t ptr, uint32_t max_chars,
                       std::u16string* out) {
  out->clear();
  for (uint32_t i = 0; i < max_chars; ++i) {
    const uint64_t address = uint64_t(ptr) + uint64_t(i) * 2;
    if (address > 0xFFFFFFFEull ||
        !GuestRangeValid(memory, static_cast<uint32_t>(address), 2)) {
      return false;
    }
    const char16_t c = static_cast<char16_t>(
        memory::load_and_swap<uint16_t>(memory->TranslateVirtual(static_cast<uint32_t>(address))));
    if (!c) {
      return true;
    }
    out->push_back(c);
  }
  return false;  // unterminated within max_chars
}

GuestArena MakeArena(memory::Memory* memory, uint32_t ptr, uint32_t size) {
  if (!GuestRangeValid(memory, ptr, size)) {
    return GuestArena();
  }
  return GuestArena(ptr, memory->TranslateVirtual(ptr), size);
}

namespace {

bool CopyGuestBytes(memory::Memory* memory, uint32_t ptr, uint32_t size,
                    std::vector<uint8_t>* out) {
  out->clear();
  if (!size) {
    return true;
  }
  size = std::min(size, marshal::kMaxUserDataBytes);
  if (!GuestRangeValid(memory, ptr, size)) {
    return false;
  }
  const uint8_t* src = memory->TranslateVirtual(ptr);
  out->assign(src, src + size);
  return true;
}

}  // namespace

bool ReadUserProperty(memory::Memory* memory, uint32_t ptr, ac6net::Property* out) {
  if (!GuestRangeValid(memory, ptr, kUserPropertySize)) {
    return false;
  }
  const uint8_t* p = memory->TranslateVirtual(ptr);
  *out = {};
  out->id = marshal::LoadBE<uint32_t>(p + 0);
  const ac6net::DataType type = marshal::DataTypeFromByte(p[8]);
  out->value.type = type;
  const uint8_t* value = p + 16;
  if (marshal::NeedsExtraData(type)) {
    const uint32_t size = marshal::LoadBE<uint32_t>(value + 0);
    const uint32_t data_ptr = marshal::LoadBE<uint32_t>(value + 4);
    if (!data_ptr) {
      return true;  // declared but empty
    }
    return CopyGuestBytes(memory, data_ptr, size, &out->value.bytes);
  }
  marshal::DecodeFixedValue(type, value, &out->value);
  return true;
}

bool ReadRawPropertyValue(memory::Memory* memory, uint32_t property_id, uint32_t size,
                          uint32_t data_ptr, ac6net::Property* out) {
  *out = {};
  out->id = property_id;
  const ac6net::DataType type = marshal::DataTypeFromPropertyId(property_id);
  out->value.type = type;
  if (marshal::NeedsExtraData(type)) {
    return CopyGuestBytes(memory, data_ptr, size, &out->value.bytes);
  }
  const uint32_t width = marshal::FixedValueSize(type);
  if (!width || (size && size < width) || !GuestRangeValid(memory, data_ptr, width)) {
    return false;
  }
  marshal::DecodeFixedValue(type, memory->TranslateVirtual(data_ptr), &out->value);
  return true;
}

// --- XSession --------------------------------------------------------------------------------

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, Type::Session) {}

XSession::~XSession() {
  // Runs on whatever guest thread dropped the last reference (often inside NtClose). xlive keeps
  // SessionHandleClosed local and non-blocking.
  if (registered_ && Enabled()) {
    Live().SessionHandleClosed(session_handle_);
  }
}

X_STATUS XSession::Initialize() {
  auto native = CreateNative(sizeof(X_KSESSION));
  if (!native) {
    return X_STATUS_NO_MEMORY;
  }
  session_handle_ = handle();
  // Based on XAM the guest object is only these 4 bytes (Xenia xsession.cc).
  reinterpret_cast<X_KSESSION*>(native)->handle = session_handle_;
  if (Enabled()) {
    Live().SessionHandleCreated(session_handle_);
    registered_ = true;
  }
  return X_STATUS_SUCCESS;
}

uint32_t SessionHandleFromObject(KernelState* kernel_state, uint32_t obj_ptr) {
  auto* memory = kernel_state->memory();
  if (!GuestRangeValid(memory, obj_ptr, sizeof(X_KSESSION))) {
    return 0;
  }
  const uint32_t handle = memory::load_and_swap<uint32_t>(memory->TranslateVirtual(obj_ptr));
  auto session = kernel_state->object_table()->LookupObject<XSession>(handle);
  if (!session) {
    return 0;
  }
  return session->session_handle();
}

}  // namespace rex::netplay

#endif  // REX_HAVE_AC6NET
