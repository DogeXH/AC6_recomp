/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

#include <rex/logging.h>
#include <rex/system/xevent.h>

namespace rex::system {

XEvent::XEvent(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  manual_reset_ = manual_reset;
  if (manual_reset) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
  UpdateGuestSignalState(initial_state ? 1 : 0);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  if (out_type) {
    *out_type = manual_reset_ ? 0x00 : 0x01;
  }
  if (out_state) {
    // Query the live host event, not the stale guest header
    auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
    if (result == rex::thread::WaitResult::kSuccess) {
      *out_state = 1;
      // Re-signal since we consumed the signal by waiting
      event_->Set();
    } else {
      *out_state = 0;
    }
  }
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  // Guest code polls the dispatch header directly. Publish guest state before
  // releasing a host waiter so the awakened thread cannot observe stale zero.
  UpdateGuestSignalState(1);
  return event_->SetQueryPrevious() ? 1 : 0;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  const bool previous = event_->PulseQueryPrevious();
  UpdateGuestSignalState(0);
  return previous ? 1 : 0;
}

int32_t XEvent::Reset() {
  const bool previous = event_->ResetQueryPrevious();
  UpdateGuestSignalState(0);
  return previous ? 1 : 0;
}

void XEvent::Clear() {
  event_->Reset();
  UpdateGuestSignalState(0);
}

void XEvent::WaitCallback() {
  if (!manual_reset_) {
    UpdateGuestSignalState(0);
  }
}

void XEvent::UpdateGuestSignalState(uint32_t value) {
  if (!guest_object()) {
    return;
  }
  auto* header = guest_object<X_DISPATCH_HEADER>();
  if (header) {
    header->signal_state = value;
  }
}

}  // namespace rex::system
