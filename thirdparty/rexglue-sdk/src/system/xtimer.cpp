/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

#include <native/platform.h>
#if REX_PLATFORM_LINUX
// Deck tuning: the audio threads are pinned with glibc's pthread affinity API.
#include <pthread.h>
#include <sched.h>
#endif

#include <atomic>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(audio_pin_sound_thread, true, "Audio",
                    "Pin the guest thread that registers the 10ms periodic "
                    "timer (AC6's sound-update thread) to CPU 6, the audio "
                    "side of the reserved physical core, so heavy sim load "
                    "cannot starve the mixer of audio data.");

namespace rex::system {

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = rex::thread::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = rex::thread::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                          uint32_t routine_arg, bool resume) {
  using rex::chrono::WinSystemClock;
  using rex::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);

  // AC6 runs its sound-update loop on the one guest thread that registers a
  // 10ms periodic timer. Under heavy 60fps sim load that thread competes on
  // the game cores and starves the host mixer of audio data (stretched and
  // cut-off voice lines). Shelter it next to the audio worker.
  // Linux only (Deck tuning); other platforms keep the scheduler's placement.
#if REX_PLATFORM_LINUX
  if (period_ms == 10 && REXCVAR_GET(audio_pin_sound_thread)) {
    static std::atomic<bool> sound_thread_pinned{false};
    bool expected = false;
    if (sound_thread_pinned.compare_exchange_strong(expected, true)) {
      cpu_set_t cpu_set;
      CPU_ZERO(&cpu_set);
      CPU_SET(6, &cpu_set);
      if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) == 0) {
        pthread_setname_np(pthread_self(), "AC6 SoundThread");
        REXLOG_INFO("Guest sound thread (10ms periodic timer) pinned to CPU 6.");
      }
    }
  }
#endif  // REX_PLATFORM_LINUX
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    callback = [this]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = rex::chrono::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      REXSYS_INFO("XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
                  callback_routine_, callback_routine_arg_, time_low, time_high);
      callback_thread_->EnqueueApc(callback_routine_, callback_routine_arg_, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result =
        timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::system
