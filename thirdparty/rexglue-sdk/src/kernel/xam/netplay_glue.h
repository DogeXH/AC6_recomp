/**
 * @file        netplay_glue.h
 * @brief       XAM-side glue between the ReXGlue kernel and the ac6net online core.
 *
 * Everything here is compiled only when the runtime links ac6net (REX_HAVE_AC6NET=1). With the
 * netplay_enabled cvar off (the default) Enabled() is false and every caller keeps its old
 * behaviour. See ac6-online-20261019/design/ARCHITECTURE.md sections 5.1-5.3.
 *
 * Threading:
 * - Startup()/Shutdown() run on the thread that sets the executable module / tears the kernel
 *   down.
 * - Async XMsg handlers and deferred notifications run on three serial glue worker lanes (XGI,
 *   XLiveBase, general), each an XHostThread so handlers keep a current XThread exactly as on
 *   the guest thread. Requests of one app complete in submission order.
 * - The ac6net network thread never calls into the kernel: its state callback only posts a task
 *   to the glue workers.
 */
#pragma once

#if REX_HAVE_AC6NET

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <ac6net/netplay.h>
#include <ac6net/types.h>
#include <ac6net/xlive.h>

#include <rex/system/xobject.h>

#include "netplay_xam_marshal.h"
#include <rex/system/xtypes.h>

namespace rex::memory {
class Memory;
}
namespace rex::system {
class KernelState;
class XNotifyListener;
}  // namespace rex::system

namespace rex::netplay {

// --- Lifecycle --------------------------------------------------------------------------------

// Vendored-SDK addition: KernelState here does not expose the runtime's user_data_root, so its
// constructor hands it over (the Mac fork reads kernel_state->emulator()->user_data_root()).
void SetUserDataRoot(const std::filesystem::path& user_data_root);

// Called once after the title module is set (KernelState::SetExecutableModule). Reads the cvars,
// dumps the SPA when netplay_dump_spa is set, and starts ac6net when netplay_enabled is true.
void Startup(system::KernelState* kernel_state);
// Called at kernel teardown. Stops the glue workers and ac6net. Safe to call when never started.
void Shutdown();

// True when netplay_enabled was set and ac6net started. Everything else is gated on this.
bool Enabled();
// True when the local user should appear signed in to Live (sign-in state 2).
bool SignedInToLive();

ac6net::xlive::XLive& Live();

// Profile values while Enabled(); callers fall back to the profile otherwise.
ac6net::Xuid OnlineXuid();      // 0 if the server has not assigned one yet
std::string Gamertag();         // empty if none is known

// --- Glue workers -----------------------------------------------------------------------------

// Runs `fn` on the general glue worker lane. Returns false (and drops `fn`) when the workers are
// not running.
bool Post(std::function<void()> fn);

// --- Async XMsg (XMsgStartIORequest(Ex) with an XOVERLAPPED) ------------------------------------

// True when a request for `app_id` (0xFB XGI, 0xFC XLiveBase) carrying an overlapped should run
// on the glue workers. XUserWriteAchievements (0xB0008) keeps its synchronous completion: it
// never touches the network and its unlock path has only ever run on guest threads.
bool ShouldDispatchAsync(uint32_t app_id, uint32_t message);

// Copies the request (buffer_length bytes, like Xenia's AppManager::DispatchMessageAsync), marks
// the overlapped pending and runs the app handler on a worker. The worker completes the
// overlapped through KernelState::CompleteOverlappedEx (the 0xFEFEFEFE-guarded path).
// Returns X_ERROR_IO_PENDING (a request buffer that cannot be read completes the overlapped at
// once with X_E_INVALIDARG), or X_E_INVALIDARG without touching anything when the overlapped
// itself is not valid guest memory.
X_RESULT DispatchMessageAsync(system::KernelState* kernel_state, uint32_t app_id, uint32_t message,
                              uint32_t buffer_ptr, uint32_t buffer_length, uint32_t overlapped_ptr);

// Lets an app handler report Xenia's split result for an async request: the overlapped gets
// `result` and `extended_error` instead of the returned HRESULT. Ignored for synchronous calls.
void SetAsyncCompletion(uint32_t result, uint32_t extended_error);

// True if an async request on this overlapped is still running.
bool IsAsyncPending(uint32_t overlapped_ptr);
// Waits until the async request on this overlapped has completed (or the timeout expires).
bool WaitAsyncRequest(uint32_t overlapped_ptr, uint32_t timeout_ms);

// --- Notifications ----------------------------------------------------------------------------

// Called from KernelState::RegisterNotifyListener with the kernel global lock held. Queues the
// Live connection notifications on a Live-mask listener while signed in to Live.
void OnNotifyListenerRegistered(system::XNotifyListener* listener);

// Broadcasts XN_SYS_UI(1), then XN_SYS_UI(0) 100 ms later, from a worker.
void PulseSystemUi();

// --- Guest structure helpers (layouts in netplay_xam_marshal.h) --------------------------------

using marshal::GuestArena;
using marshal::kSessionInfoSize;
using marshal::kUserContextSize;
using marshal::kUserPropertySize;
using marshal::ReadSessionInfo;
using marshal::WriteSessionInfo;

// True if [ptr, ptr + size) is non-null, does not wrap and lies in mapped guest heaps.
bool GuestRangeValid(memory::Memory* memory, uint32_t ptr, uint32_t size);

// An arena over a validated guest output buffer; empty (size 0, every Alloc fails) if the range
// is not valid guest memory.
GuestArena MakeArena(memory::Memory* memory, uint32_t ptr, uint32_t size);

// Reads a NUL-terminated UTF-16BE guest string of at most `max_chars` characters, checking every
// character's address. Returns false for an unreadable or unterminated string.
bool ReadGuestString16(memory::Memory* memory, uint32_t ptr, uint32_t max_chars,
                       std::u16string* out);

// Reads an XUSER_PROPERTY at `ptr`, copying referenced unicode/binary bytes (at most
// marshal::kMaxUserDataBytes). Returns false if the structure or its data is not readable.
bool ReadUserProperty(memory::Memory* memory, uint32_t ptr, ac6net::Property* out);
// Reads the raw value an XGIUserSetPropertyEx passes ({id, size, data_ptr}); the type comes from
// the property ID's top nibble.
bool ReadRawPropertyValue(memory::Memory* memory, uint32_t property_id, uint32_t size,
                          uint32_t data_ptr, ac6net::Property* out);

// --- XamSession kernel object ----------------------------------------------------------------
// Port of the X_KSESSION idea from Xenia Netplay (src/xenia/kernel/xsession.cc:30-40,
// xam_user.cc:1441-1467; Copyright 2026 Xenia Canary, BSD-3-Clause): a real kernel object whose
// 4-byte guest object holds its own handle, so the guest pointer AC6 passes in every XGI request
// resolves back to the handle that keys the session in ac6net::xlive.

struct X_KSESSION {
  be<uint32_t> handle;
};
static_assert_size(X_KSESSION, 4);

class XSession : public system::XObject {
 public:
  static const Type kObjectType = Type::Session;

  explicit XSession(system::KernelState* kernel_state);
  ~XSession() override;

  // Allocates the guest X_KSESSION and registers the handle with xlive.
  X_STATUS Initialize();

  uint32_t session_handle() const { return session_handle_; }

 private:
  uint32_t session_handle_ = 0;
  bool registered_ = false;
};

// Resolves the X_KSESSION guest pointer from an XGI request to its session handle. Returns 0 if
// the pointer or the handle is not a live XSession.
uint32_t SessionHandleFromObject(system::KernelState* kernel_state, uint32_t obj_ptr);

}  // namespace rex::netplay

#endif  // REX_HAVE_AC6NET
