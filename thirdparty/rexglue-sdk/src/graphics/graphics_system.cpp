/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/present_deadline.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(present_pacing_preserve_phase, false, "GPU",
                    "Preserve present phase across delays up to one quarter frame")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");

REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "GPU", "Enable GPU trace streaming");
REXCVAR_DEFINE_BOOL(guest_vblank_sync_to_refresh, false, "GPU",
                    "Keep guest VBlank cadence tied to the guest refresh rate even when host "
                    "vsync is disabled");

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
constexpr bool kStoreShaders = true;

bool HasMeaningfulTextureFetch(const rex::system::GraphicsSwapSubmission& submission) {
  for (uint32_t word : submission.texture_fetch) {
    if (word != 0) {
      return true;
    }
  }
  return false;
}

rex::system::GraphicsSwapSubmission MergeSwapSubmission(
    const rex::system::GraphicsSwapSubmission& base,
    const rex::system::GraphicsSwapSubmission& incoming) {
  rex::system::GraphicsSwapSubmission merged = base;
  if (incoming.frontbuffer_virtual_address) {
    merged.frontbuffer_virtual_address = incoming.frontbuffer_virtual_address;
  }
  if (incoming.frontbuffer_physical_address) {
    merged.frontbuffer_physical_address = incoming.frontbuffer_physical_address;
  }
  if (incoming.frontbuffer_width) {
    merged.frontbuffer_width = incoming.frontbuffer_width;
  }
  if (incoming.frontbuffer_height) {
    merged.frontbuffer_height = incoming.frontbuffer_height;
  }
  if (incoming.texture_format) {
    merged.texture_format = incoming.texture_format;
  }
  if (incoming.color_space) {
    merged.color_space = incoming.color_space;
  }
  if (HasMeaningfulTextureFetch(incoming)) {
    merged.texture_fetch = incoming.texture_fetch;
  }
  return merged;
}

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

namespace {
// Modern present pacing state (see SetGuestPresentPacing / PaceGuestPresent).
// The counters pair committed guest swap packets reserved by VdSwap with
// terminal IssueSwap retirements, classified as presenter-delivered or dropped.
// PaceGuestPresent blocks the swapping guest thread on that pairing plus an
// absolute-deadline frame limiter - GPU backpressure and a rate ceiling, the
// modern game loop - instead of pacing the guest off the vblank grid.
std::mutex g_present_pacing_mutex;
std::condition_variable g_present_pacing_cv;
// Mirrors of the values applied under the pacing mutex. Render hooks call all
// three setters every frame; the overwhelmingly common unchanged path performs
// atomic operations only and returns without taking the mutex. Session-dirty
// bits are claimed with read-modify-write so the first call for every setting
// after command-processor setup must serialize even if a value mirror is stale.
constexpr uint32_t kPresentPacingFastTargetDirty = 1u << 0;
constexpr uint32_t kPresentPacingFastMaxInFlightDirty = 1u << 1;
constexpr uint32_t kPresentPacingFastTelemetryDirty = 1u << 2;
constexpr uint32_t kPresentPacingFastAllDirty =
    kPresentPacingFastTargetDirty |
    kPresentPacingFastMaxInFlightDirty |
    kPresentPacingFastTelemetryDirty;
std::atomic<double> g_present_pacing_fast_target_hz{0.0};
std::atomic<uint32_t> g_present_pacing_fast_max_in_flight{1};
std::atomic<bool> g_present_pacing_fast_telemetry_enabled{false};
std::atomic<uint32_t> g_present_pacing_fast_dirty_mask{
    kPresentPacingFastAllDirty};
// Guarded by g_present_pacing_mutex.
double g_present_pacing_target_hz = 0.0;
uint32_t g_present_pacing_max_in_flight = 1;
std::chrono::steady_clock::time_point g_present_pacing_deadline{};
std::atomic<uint64_t> g_present_pacing_target_epoch{0};
bool g_present_pacing_delivery_fail_open = false;
bool g_present_pacing_telemetry_enabled = false;
bool g_present_pacing_drop_warning_emitted = false;
bool g_present_pacing_invariant_warning_emitted = false;
bool g_present_pacing_ever_active = false;
bool g_present_pacing_accept_reservations = false;
// VdSwap primary-ring submission is serialized by the Xbox guest command
// stream. FIFO activity classification relies on that ordering; a future
// backend that invokes VdSwap concurrently must carry an explicit reservation
// token through PM4 commit instead of sharing this queue implicitly.
std::deque<bool> g_present_pacing_reservations;

struct PresentPacingCounters {
  uint64_t reserved = 0;
  uint64_t retired = 0;
  uint64_t delivered = 0;
  uint64_t dropped = 0;
  uint64_t active_retired = 0;
  uint64_t active_delivered = 0;
  uint64_t active_dropped = 0;
  uint64_t inactive_retired = 0;
  uint64_t inactive_delivered = 0;
  uint64_t inactive_dropped = 0;
  uint64_t ignored_unpaired_inactive = 0;
  uint64_t invariant_failures = 0;
  uint64_t active_reservations = 0;
  uint64_t backpressure_waits = 0;
  uint64_t limiter_waits = 0;
  uint64_t delivery_timeouts = 0;
  std::chrono::steady_clock::duration backpressure_wait_time{};
  std::chrono::steady_clock::duration limiter_wait_time{};
};

struct PresentPacingTelemetryEdge {
  PresentPacingCounters counters;
  std::chrono::steady_clock::time_point time{};
};

struct PresentPacingTelemetryReport {
  bool emit = false;
  const char* reason = "";
  uint64_t arm_id = 0;
  double target_hz = 0.0;
  uint32_t max_in_flight = 1;
  PresentPacingCounters totals;
  PresentPacingCounters delta;
  std::chrono::steady_clock::duration wall_time{};
  uint64_t high_water_backlog = 0;
  int64_t signed_skew = 0;
  bool fail_open = false;
};

PresentPacingCounters g_present_pacing_counters;
PresentPacingTelemetryEdge g_present_pacing_telemetry_edge;
uint64_t g_present_pacing_telemetry_arm_id = 0;
uint64_t g_present_pacing_telemetry_high_water_backlog = 0;

uint64_t PresentPacingBacklogLocked() {
  return g_present_pacing_counters.reserved >=
                 g_present_pacing_counters.retired
             ? g_present_pacing_counters.reserved -
                   g_present_pacing_counters.retired
             : 0;
}

int64_t PresentPacingSignedSkewLocked() {
  if (g_present_pacing_counters.reserved >=
      g_present_pacing_counters.retired) {
    const uint64_t skew = g_present_pacing_counters.reserved -
                          g_present_pacing_counters.retired;
    return skew > uint64_t(INT64_MAX) ? INT64_MAX : int64_t(skew);
  }
  const uint64_t skew = g_present_pacing_counters.retired -
                        g_present_pacing_counters.reserved;
  return skew > uint64_t(INT64_MAX) ? INT64_MIN : -int64_t(skew);
}

PresentPacingCounters SubtractPresentPacingCounters(
    const PresentPacingCounters& current,
    const PresentPacingCounters& previous) {
  PresentPacingCounters delta;
  delta.reserved = current.reserved - previous.reserved;
  delta.retired = current.retired - previous.retired;
  delta.delivered = current.delivered - previous.delivered;
  delta.dropped = current.dropped - previous.dropped;
  delta.active_retired = current.active_retired - previous.active_retired;
  delta.active_delivered =
      current.active_delivered - previous.active_delivered;
  delta.active_dropped = current.active_dropped - previous.active_dropped;
  delta.inactive_retired =
      current.inactive_retired - previous.inactive_retired;
  delta.inactive_delivered =
      current.inactive_delivered - previous.inactive_delivered;
  delta.inactive_dropped =
      current.inactive_dropped - previous.inactive_dropped;
  delta.ignored_unpaired_inactive =
      current.ignored_unpaired_inactive -
      previous.ignored_unpaired_inactive;
  delta.invariant_failures =
      current.invariant_failures - previous.invariant_failures;
  delta.active_reservations =
      current.active_reservations - previous.active_reservations;
  delta.backpressure_waits =
      current.backpressure_waits - previous.backpressure_waits;
  delta.limiter_waits = current.limiter_waits - previous.limiter_waits;
  delta.delivery_timeouts =
      current.delivery_timeouts - previous.delivery_timeouts;
  delta.backpressure_wait_time =
      current.backpressure_wait_time - previous.backpressure_wait_time;
  delta.limiter_wait_time =
      current.limiter_wait_time - previous.limiter_wait_time;
  return delta;
}

bool HasPresentPacingTelemetryActivity(
    const PresentPacingCounters& delta) {
  return delta.reserved || delta.retired || delta.delivered || delta.dropped ||
         delta.active_retired || delta.active_delivered ||
         delta.active_dropped || delta.inactive_retired ||
         delta.inactive_delivered || delta.inactive_dropped ||
         delta.ignored_unpaired_inactive || delta.invariant_failures ||
         delta.active_reservations || delta.backpressure_waits ||
         delta.limiter_waits || delta.delivery_timeouts;
}

void ResetPresentPacingTelemetryEdgeLocked(
    std::chrono::steady_clock::time_point now) {
  g_present_pacing_telemetry_edge = {g_present_pacing_counters, now};
  g_present_pacing_telemetry_high_water_backlog =
      PresentPacingBacklogLocked();
}

PresentPacingTelemetryReport FinishPresentPacingTelemetryWindowLocked(
    const char* reason, std::chrono::steady_clock::time_point now) {
  PresentPacingTelemetryReport report;
  if (!g_present_pacing_telemetry_enabled ||
      g_present_pacing_telemetry_edge.time.time_since_epoch().count() == 0) {
    return report;
  }
  report.delta = SubtractPresentPacingCounters(
      g_present_pacing_counters,
      g_present_pacing_telemetry_edge.counters);
  if (!HasPresentPacingTelemetryActivity(report.delta)) {
    return report;
  }
  report.emit = true;
  report.reason = reason;
  report.arm_id = g_present_pacing_telemetry_arm_id;
  report.target_hz = g_present_pacing_target_hz;
  report.max_in_flight = g_present_pacing_max_in_flight;
  report.totals = g_present_pacing_counters;
  report.wall_time = now - g_present_pacing_telemetry_edge.time;
  report.high_water_backlog =
      g_present_pacing_telemetry_high_water_backlog;
  report.signed_skew = PresentPacingSignedSkewLocked();
  report.fail_open = g_present_pacing_delivery_fail_open;
  return report;
}

double PresentPacingDurationMilliseconds(
    std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

void EmitPresentPacingTelemetry(
    const PresentPacingTelemetryReport& report) {
  if (!report.emit) {
    return;
  }
  // Performance mode intentionally forces error-only logging. This report is
  // explicitly opt-in and emitted only once per telemetry window, so keep it
  // visible alongside the existing AC6 frame-rate telemetry.
  REXGPU_ERROR(
      "Present overlap arm {} {}: {} active swaps in {:.3f} ms at {:.3f} Hz "
      "max-in-flight {}; reserved {} (+{}), retired {} (+{}), delivered {} "
      "(+{}), dropped {} (+{}); paced delivered {} (+{}), paced dropped {} "
      "(+{}), inactive delivered {} (+{}), inactive dropped {} (+{}), "
      "ignored pre-pacing unpaired {} (+{}); signed skew {}, invariant "
      "failures {} (+{}), high-water backlog {}; backpressure {} waits / "
      "{:.3f} ms, limiter {} waits / {:.3f} ms, timeouts {} (+{}), run {}.",
      report.arm_id, report.reason, report.delta.active_reservations,
      PresentPacingDurationMilliseconds(report.wall_time), report.target_hz,
      report.max_in_flight, report.totals.reserved, report.delta.reserved,
      report.totals.retired, report.delta.retired, report.totals.delivered,
      report.delta.delivered, report.totals.dropped, report.delta.dropped,
      report.totals.active_delivered, report.delta.active_delivered,
      report.totals.active_dropped, report.delta.active_dropped,
      report.totals.inactive_delivered, report.delta.inactive_delivered,
      report.totals.inactive_dropped, report.delta.inactive_dropped,
      report.totals.ignored_unpaired_inactive,
      report.delta.ignored_unpaired_inactive, report.signed_skew,
      report.totals.invariant_failures, report.delta.invariant_failures,
      report.high_water_backlog,
      report.delta.backpressure_waits,
      PresentPacingDurationMilliseconds(report.delta.backpressure_wait_time),
      report.delta.limiter_waits,
      PresentPacingDurationMilliseconds(report.delta.limiter_wait_time),
      report.totals.delivery_timeouts, report.delta.delivery_timeouts,
      report.fail_open ? "INVALID (fail-open)" : "valid");
}

void ResetPresentPacingStateAfterCommandProcessorShutdown() {
  PresentPacingTelemetryReport report;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    const auto telemetry_now =
        g_present_pacing_telemetry_enabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
    report = FinishPresentPacingTelemetryWindowLocked(
        "graphics-system-shutdown", telemetry_now);

    // Shutdown has joined and destroyed the command processor, so no further
    // IssueSwap retirement may race this reset. Leave a freshly constructed
    // GraphicsSystem in the same process with no stale reservations, invalid
    // run latch, limiter deadline, telemetry arm, or accounting history.
    g_present_pacing_fast_target_hz.store(0.0,
                                          std::memory_order_release);
    g_present_pacing_fast_max_in_flight.store(1,
                                               std::memory_order_release);
    g_present_pacing_fast_telemetry_enabled.store(
        false, std::memory_order_release);
    g_present_pacing_fast_dirty_mask.fetch_or(
        kPresentPacingFastAllDirty, std::memory_order_release);
    g_present_pacing_target_hz = 0.0;
    g_present_pacing_max_in_flight = 1;
    g_present_pacing_deadline = {};
    g_present_pacing_target_epoch.fetch_add(1, std::memory_order_release);
    g_present_pacing_delivery_fail_open = false;
    g_present_pacing_telemetry_enabled = false;
    g_present_pacing_drop_warning_emitted = false;
    g_present_pacing_invariant_warning_emitted = false;
    g_present_pacing_ever_active = false;
    g_present_pacing_accept_reservations = false;
    g_present_pacing_reservations.clear();
    g_present_pacing_counters = {};
    g_present_pacing_telemetry_edge = {};
    g_present_pacing_telemetry_arm_id = 0;
    g_present_pacing_telemetry_high_water_backlog = 0;
  }
  g_present_pacing_cv.notify_all();
  EmitPresentPacingTelemetry(report);
}
}  // namespace

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() {
  PresentPacingTelemetryReport report;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (g_present_pacing_telemetry_enabled) {
      const auto telemetry_now = std::chrono::steady_clock::now();
      report = FinishPresentPacingTelemetryWindowLocked(
          "graphics-system-end", telemetry_now);
      g_present_pacing_telemetry_enabled = false;
      g_present_pacing_fast_telemetry_enabled.store(
          false, std::memory_order_release);
      ++g_present_pacing_telemetry_arm_id;
      ResetPresentPacingTelemetryEdgeLocked({});
    }
  }
  EmitPresentPacingTelemetry(report);
}

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    // A successful command-processor start begins a new pairing session.
    // Shutdown reset all accounting after joining the previous processor.
    g_present_pacing_accept_reservations = true;
    g_present_pacing_fast_dirty_mask.fetch_or(
        kPresentPacingFastAllDirty, std::memory_order_release);
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
  uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
  uint64_t vsync_interval_ticks =
      std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
  uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
  guest_vblank_interval_ticks_.store(vsync_interval_ticks, std::memory_order_release);

  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this, vsync_interval_ticks,
                                                               no_vsync_interval_ticks,
                                                               guest_tick_frequency]() {
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
        while (vsync_worker_running_) {
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          uint64_t interval_ticks = REXCVAR_GET(guest_vblank_sync_to_refresh)
                                        ? vsync_interval_ticks
                                        : (REXCVAR_GET(vsync) ? vsync_interval_ticks
                                                              : no_vsync_interval_ticks);
          double vblank_hz_override = GetGuestVblankHzOverride();
          if (vblank_hz_override > 0.0) {
            interval_ticks = std::max(
                uint64_t(1), uint64_t(double(guest_tick_frequency) / vblank_hz_override));
          }
          // Re-anchor when far behind so a shrinking interval (an override
          // switching from a paced rate to a much faster one) or a long stall
          // does not burst a backlog of MarkVblank calls in one wake.
          if (current_time - last_frame_time >= interval_ticks * 4) {
            last_frame_time = current_time - interval_ticks;
          }
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  if (REXCVAR_GET(trace_gpu_stream)) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  // Runtime teardown quiesces guest execution before entering here (ReXApp
  // joins its module thread before destroying Runtime, which clears the kernel
  // graphics pointer before calling Shutdown). Therefore no successful VdSwap
  // reservation can still be between PaceGuestPresent and PM4 construction.
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  // Every backend IssueSwap guard has retired before CommandProcessor::Shutdown
  // returns, so this is the first safe point for clearing process-global
  // reservation and pacing state.
  ResetPresentPacingStateAfterCommandProcessorShutdown();
  SetGuestVblankHzOverride(0.0);

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::SetFrameBoundaryCallback(std::function<void(rex::memory::Memory*)> callback) {
  frame_boundary_callback_ = std::move(callback);
}

bool GraphicsSystem::HandleVideoSwap(const system::GraphicsSwapSubmission& submission) {
  {
    std::lock_guard<std::mutex> lock(last_swap_submission_mutex_);
    last_swap_submission_ = MergeSwapSubmission(last_swap_submission_, submission);
    ++last_swap_submission_sequence_;
  }
  if (frame_boundary_callback_) {
    frame_boundary_callback_(memory_);
  }
  return false;
}

bool GraphicsSystem::GetLastSwapSubmission(system::GraphicsSwapSubmission* out_submission,
                                           uint64_t* out_sequence) const {
  std::lock_guard<std::mutex> lock(last_swap_submission_mutex_);
  if (out_submission) {
    *out_submission = last_swap_submission_;
  }
  if (out_sequence) {
    *out_sequence = last_swap_submission_sequence_;
  }
  return last_swap_submission_sequence_ != 0;
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);
  if (source == 0) {
    last_vblank_interrupt_guest_tick_.store(chrono::Clock::QueryGuestTickCount(),
                                            std::memory_order_release);
  }

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

namespace {
std::atomic<double> g_guest_vblank_hz_override{0.0};
}  // namespace

void GraphicsSystem::SetGuestVblankHzOverride(double hz) {
  g_guest_vblank_hz_override.store(hz, std::memory_order_relaxed);
}

double GraphicsSystem::GetGuestVblankHzOverride() {
  return g_guest_vblank_hz_override.load(std::memory_order_relaxed);
}

void GraphicsSystem::SetGuestPresentPacing(double target_hz) {
  const bool session_slow_path =
      (g_present_pacing_fast_dirty_mask.fetch_and(
           ~kPresentPacingFastTargetDirty, std::memory_order_acq_rel) &
       kPresentPacingFastTargetDirty) != 0;
  if (!session_slow_path &&
      g_present_pacing_fast_target_hz.load(
          std::memory_order_acquire) == target_hz) {
    return;
  }

  PresentPacingTelemetryReport report;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (!g_present_pacing_accept_reservations) {
      return;
    }
    if (g_present_pacing_target_hz != target_hz) {
      const auto telemetry_now =
          g_present_pacing_telemetry_enabled
              ? std::chrono::steady_clock::now()
              : std::chrono::steady_clock::time_point{};
      report = FinishPresentPacingTelemetryWindowLocked(
          "target-transition", telemetry_now);
      const double previous_target_hz =
          std::exchange(g_present_pacing_target_hz, target_hz);
      assert_true(previous_target_hz != target_hz);
      if (target_hz > 0.0) {
        g_present_pacing_ever_active = true;
      }
      g_present_pacing_deadline = {};
      g_present_pacing_target_epoch.fetch_add(
          1, std::memory_order_release);
      ++g_present_pacing_telemetry_arm_id;
      ResetPresentPacingTelemetryEdgeLocked(telemetry_now);
      changed = true;
    }
    g_present_pacing_fast_target_hz.store(
        g_present_pacing_target_hz, std::memory_order_release);
  }
  if (changed) {
    g_present_pacing_cv.notify_all();
    EmitPresentPacingTelemetry(report);
  }
}

void GraphicsSystem::SetGuestPresentMaxInFlight(uint32_t max_in_flight) {
  max_in_flight =
      std::min(std::max(max_in_flight, uint32_t(1)), uint32_t(2));
  const bool session_slow_path =
      (g_present_pacing_fast_dirty_mask.fetch_and(
           ~kPresentPacingFastMaxInFlightDirty,
           std::memory_order_acq_rel) &
       kPresentPacingFastMaxInFlightDirty) != 0;
  if (!session_slow_path &&
      g_present_pacing_fast_max_in_flight.load(
          std::memory_order_acquire) == max_in_flight) {
    return;
  }

  PresentPacingTelemetryReport report;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (!g_present_pacing_accept_reservations) {
      return;
    }
    if (g_present_pacing_max_in_flight != max_in_flight) {
      const auto telemetry_now =
          g_present_pacing_telemetry_enabled
              ? std::chrono::steady_clock::now()
              : std::chrono::steady_clock::time_point{};
      report = FinishPresentPacingTelemetryWindowLocked(
          "max-in-flight-transition", telemetry_now);
      g_present_pacing_max_in_flight = max_in_flight;
      // A capacity A/B edge starts a new telemetry arm, but deliberately keeps
      // the limiter deadline and all current-session counters intact.
      ++g_present_pacing_telemetry_arm_id;
      ResetPresentPacingTelemetryEdgeLocked(telemetry_now);
      changed = true;
    }
    g_present_pacing_fast_max_in_flight.store(
        g_present_pacing_max_in_flight, std::memory_order_release);
  }
  if (changed) {
    g_present_pacing_cv.notify_all();
    EmitPresentPacingTelemetry(report);
  }
}

void GraphicsSystem::SetGuestPresentTelemetryEnabled(bool enabled) {
  const bool session_slow_path =
      (g_present_pacing_fast_dirty_mask.fetch_and(
           ~kPresentPacingFastTelemetryDirty,
           std::memory_order_acq_rel) &
       kPresentPacingFastTelemetryDirty) != 0;
  if (!session_slow_path &&
      g_present_pacing_fast_telemetry_enabled.load(
          std::memory_order_acquire) == enabled) {
    return;
  }

  PresentPacingTelemetryReport report;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (!g_present_pacing_accept_reservations) {
      return;
    }
    if (g_present_pacing_telemetry_enabled != enabled) {
      const auto telemetry_now = std::chrono::steady_clock::now();
      if (g_present_pacing_telemetry_enabled) {
        report = FinishPresentPacingTelemetryWindowLocked(
            "telemetry-end", telemetry_now);
      }
      g_present_pacing_telemetry_enabled = enabled;
      ++g_present_pacing_telemetry_arm_id;
      ResetPresentPacingTelemetryEdgeLocked(
          enabled
              ? telemetry_now
              : std::chrono::steady_clock::time_point{});
      changed = true;
    }
    g_present_pacing_fast_telemetry_enabled.store(
        g_present_pacing_telemetry_enabled, std::memory_order_release);
  }
  if (changed) {
    EmitPresentPacingTelemetry(report);
  }
}

bool GraphicsSystem::PaceGuestPresent() {
  bool backpressure_waited = false;
  bool measure_backpressure_wait = false;
  std::chrono::steady_clock::duration backpressure_wait_time{};
  bool delivery_timed_out = false;
  uint64_t timeout_reserved = 0;
  uint64_t timeout_retired = 0;

  bool run_limiter = false;
  bool limiter_waited = false;
  bool measure_limiter_wait = false;
  uint64_t limiter_target_epoch = 0;
  std::chrono::steady_clock::time_point limiter_wake{};
  std::chrono::steady_clock::time_point limiter_measure_begin{};
  std::chrono::steady_clock::duration limiter_wait_time{};

  {
    std::unique_lock<std::mutex> lock(g_present_pacing_mutex);
    const auto room_available = [&] {
      if (!g_present_pacing_accept_reservations ||
          g_present_pacing_target_hz <= 0.0 ||
          g_present_pacing_delivery_fail_open) {
        return true;
      }
      const uint64_t backlog = PresentPacingBacklogLocked();
      return g_present_pacing_counters.retired >=
                 g_present_pacing_counters.reserved ||
             backlog < g_present_pacing_max_in_flight;
    };

    if (!room_available()) {
      backpressure_waited = true;
      measure_backpressure_wait = g_present_pacing_telemetry_enabled;
      std::chrono::steady_clock::time_point wait_begin{};
      if (measure_backpressure_wait) {
        wait_begin = std::chrono::steady_clock::now();
      }
      const bool room_became_available = g_present_pacing_cv.wait_for(
          lock, std::chrono::milliseconds(100), room_available);
      if (measure_backpressure_wait) {
        backpressure_wait_time =
            std::chrono::steady_clock::now() - wait_begin;
      }
      if (!room_became_available) {
        g_present_pacing_delivery_fail_open = true;
        ++g_present_pacing_counters.delivery_timeouts;
        delivery_timed_out = true;
        timeout_reserved = g_present_pacing_counters.reserved;
        timeout_retired = g_present_pacing_counters.retired;
      }
    }

    // Pairing is disabled before GPU setup and after teardown. Never report a
    // successful reservation outside a live command-processor session; VdSwap
    // will leave its already-reserved ring range as valid Type2 NOP packets.
    if (!g_present_pacing_accept_reservations) {
      return false;
    }

    if (backpressure_waited) {
      ++g_present_pacing_counters.backpressure_waits;
      if (measure_backpressure_wait) {
        g_present_pacing_counters.backpressure_wait_time +=
            backpressure_wait_time;
      }
    }

    // Re-check the current target after every wake before atomically reserving
    // the committed PM4 swap. Capacity testing and reservation share this
    // critical section, so concurrent guest callers cannot overbook the arm.
    const double target_hz = g_present_pacing_target_hz;
    const bool pacing_active = target_hz > 0.0;
    g_present_pacing_reservations.push_back(pacing_active);
    ++g_present_pacing_counters.reserved;
    if (pacing_active) {
      ++g_present_pacing_counters.active_reservations;
    }
    assert_true(uint64_t(g_present_pacing_reservations.size()) ==
                PresentPacingBacklogLocked());
    if (g_present_pacing_telemetry_enabled) {
      g_present_pacing_telemetry_high_water_backlog = std::max(
          g_present_pacing_telemetry_high_water_backlog,
          PresentPacingBacklogLocked());
    }

    // Re-check again immediately before scheduling the limiter. Target
    // transitions reset the deadline under this same mutex and increment the
    // epoch so a transition after unlock cancels the pending wait.
    const double limiter_target_hz = g_present_pacing_target_hz;
    if (limiter_target_hz > 0.0) {
      const auto limiter_now = std::chrono::steady_clock::now();
      const auto interval =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / limiter_target_hz));
      const bool preserve_phase = REXCVAR_GET(present_pacing_preserve_phase);
      const auto schedule = detail::SchedulePresentDeadline(
          limiter_now, g_present_pacing_deadline, interval, preserve_phase);
      limiter_wake = schedule.wake;
      g_present_pacing_deadline = schedule.next;
      static bool phase_logged = false;
      if (preserve_phase && !phase_logged) {
        phase_logged = true;
        REXGPU_ERROR("Present phase preservation active: quarter-frame catch-up bound");
      }
      limiter_target_epoch =
          g_present_pacing_target_epoch.load(std::memory_order_acquire);
      run_limiter = true;
      limiter_waited = limiter_now < limiter_wake;
      measure_limiter_wait =
          limiter_waited && g_present_pacing_telemetry_enabled;
      if (measure_limiter_wait) {
        limiter_measure_begin = limiter_now;
      }
    }
  }

  if (delivery_timed_out) {
    const uint64_t timeout_backlog =
        timeout_reserved >= timeout_retired
            ? timeout_reserved - timeout_retired
            : 0;
    REXGPU_WARN(
        "Present overlap delivery timed out after 100 ms (reserved {}, "
        "retired {}, backlog {}). Backpressure is permanently fail-open; "
        "this process is invalid for A/B telemetry.",
        timeout_reserved, timeout_retired, timeout_backlog);
    g_present_pacing_cv.notify_all();
  }

  // Absolute-deadline ceiling, still active after fail-open. Coarse waiting is
  // interruptible by a target transition; the final ~2 ms spin also checks
  // the target epoch so disabling/changing pacing cannot leave a stale wait.
  if (run_limiter && limiter_waited) {
    while (g_present_pacing_target_epoch.load(std::memory_order_acquire) ==
           limiter_target_epoch) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= limiter_wake) {
        break;
      }
      const auto remaining = limiter_wake - now;
      if (remaining > std::chrono::milliseconds(2)) {
        std::unique_lock<std::mutex> lock(g_present_pacing_mutex);
        g_present_pacing_cv.wait_for(
            lock,
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining) -
                std::chrono::milliseconds(2),
            [&] {
              return g_present_pacing_target_epoch.load(
                         std::memory_order_acquire) != limiter_target_epoch;
            });
      } else {
        std::this_thread::yield();
      }
    }
    if (measure_limiter_wait) {
      limiter_wait_time =
          std::chrono::steady_clock::now() - limiter_measure_begin;
    }
  }

  PresentPacingTelemetryReport report;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (!g_present_pacing_accept_reservations) {
      return false;
    }
    if (limiter_waited) {
      ++g_present_pacing_counters.limiter_waits;
      if (measure_limiter_wait) {
        g_present_pacing_counters.limiter_wait_time += limiter_wait_time;
      }
    }
    if (g_present_pacing_telemetry_enabled &&
        g_present_pacing_counters.active_reservations -
                g_present_pacing_telemetry_edge.counters.active_reservations >=
            300) {
      const auto telemetry_now = std::chrono::steady_clock::now();
      report = FinishPresentPacingTelemetryWindowLocked(
          "periodic", telemetry_now);
      ResetPresentPacingTelemetryEdgeLocked(telemetry_now);
    }
  }
  EmitPresentPacingTelemetry(report);
  return true;
}

void GraphicsSystem::RetireGuestPresent(bool delivered) {
  bool log_drop = false;
  bool log_invariant_failure = false;
  uint64_t reserved = 0;
  uint64_t retired = 0;
  {
    std::lock_guard<std::mutex> lock(g_present_pacing_mutex);
    if (!g_present_pacing_accept_reservations) {
      return;
    }
    auto& counters = g_present_pacing_counters;
    if (g_present_pacing_reservations.empty() ||
        counters.retired >= counters.reserved) {
      // Saturate rather than manufacturing retirement credit that a future
      // reservation could consume. Standalone trace tools may issue swaps
      // without VdSwap and are ignored until pacing has ever become active.
      // Once an active pacing epoch exists, an unpaired retirement invalidates
      // the process because it may conceal a missing active reservation.
      if (g_present_pacing_reservations.empty() &&
          !g_present_pacing_ever_active &&
          counters.retired >= counters.reserved) {
        ++counters.ignored_unpaired_inactive;
      } else {
        ++counters.invariant_failures;
        g_present_pacing_delivery_fail_open = true;
        if (!g_present_pacing_invariant_warning_emitted) {
          g_present_pacing_invariant_warning_emitted = true;
          log_invariant_failure = true;
        }
      }
    } else {
      const bool reservation_was_active =
          g_present_pacing_reservations.front();
      g_present_pacing_reservations.pop_front();
      ++counters.retired;
      if (delivered) {
        ++counters.delivered;
      } else {
        ++counters.dropped;
      }
      if (reservation_was_active) {
        ++counters.active_retired;
        if (delivered) {
          ++counters.active_delivered;
        } else {
          ++counters.active_dropped;
          // The committed swap retired exactly once and its reservation has
          // already been removed, so capacity accounting is still sound. A
          // dropped frame invalidates the current performance sample, but it
          // must not permanently remove backpressure from every later frame.
          // Reserve fail-open for missing or unpaired retirements, where
          // continuing to wait could actually deadlock the guest.
          if (!g_present_pacing_drop_warning_emitted) {
            g_present_pacing_drop_warning_emitted = true;
            log_drop = true;
          }
        }
      } else {
        ++counters.inactive_retired;
        if (delivered) {
          ++counters.inactive_delivered;
        } else {
          ++counters.inactive_dropped;
        }
      }
    }
    assert_true(counters.retired <= counters.reserved);
    assert_true(counters.delivered + counters.dropped == counters.retired);
    assert_true(counters.active_delivered + counters.active_dropped ==
                counters.active_retired);
    assert_true(counters.inactive_delivered + counters.inactive_dropped ==
                counters.inactive_retired);
    assert_true(uint64_t(g_present_pacing_reservations.size()) ==
                PresentPacingBacklogLocked());
    reserved = counters.reserved;
    retired = counters.retired;
  }
  g_present_pacing_cv.notify_all();

  if (log_drop) {
    REXGPU_WARN(
        "Present overlap observed a dropped active-paced committed swap "
        "(reserved {}, retired {}). The retirement was paired, so "
        "backpressure remains active; the affected telemetry window is "
        "invalid.",
        reserved, retired);
  }
  if (log_invariant_failure) {
    REXGPU_WARN(
        "Present overlap retirement had no matching reservation (reserved {}, "
        "retired {}). Retirement was saturated; backpressure is permanently "
        "fail-open and this process is invalid for A/B telemetry.",
        reserved, retired);
  }
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!kStoreShaders) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::EndTracing() {
  command_processor_->EndTracing();
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
