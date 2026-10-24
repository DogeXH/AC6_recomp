#include "ac6_backend_hooks.h"

#include "../ac6_native_renderer2/native_renderer.h"

#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

#include "ac6_backend_pass_classifier.h"
#include "render_hooks.h"

REXCVAR_DEFINE_BOOL(ac6_backend_debug_swap, false, "AC6/Backend",
                    "Log AC6 swap-path diagnostics from the authoritative backend");
REXCVAR_DEFINE_BOOL(ac6_backend_log_signatures, false, "AC6/Backend",
                    "Log AC6 capture signatures used for backend-fix routing");
REXCVAR_DEFINE_BOOL(ac6_backend_signature_diagnostics, false, "AC6/Backend",
                    "Track per-frame render signatures for diagnostics overlays and backend-fix research");

namespace ac6::backend {

void NativeRendererOnGpuDraw(
    rex::graphics::CommandProcessor* command_processor,
    rex::graphics::RegisterFile* register_file,
    rex::memory::Memory* memory,
    uint32_t primitive_type,
    uint32_t index_count) {
  ac6::nr2::OnGpuDrawExact(command_processor, register_file, memory,
                           primitive_type, index_count);
}

bool NativeRendererOnGpuCopy(
    rex::graphics::CommandProcessor* command_processor,
    rex::graphics::RegisterFile* register_file,
    rex::memory::Memory* memory,
    uint32_t* written_address,
    uint32_t* written_length) {
  if (ac6::nr2::SharedAuthorityEnabled()) {
    // Shared native draws already land in the emulator-owned EDRAM target, so
    // the immediately preceding emulator copy is authoritative. Running the
    // private resolve path here would recreate the duplicate device/target
    // work this mode is designed to remove.
    if (written_address) {
      *written_address = 0;
    }
    if (written_length) {
      *written_length = 0;
    }
    return false;
  }
  return ac6::nr2::OnGpuCopyExact(command_processor, register_file, memory,
                                  written_address, written_length);
}

void NativeRendererOnGpuSwap() {
  ac6::nr2::OnGpuSwapExact();
}

bool NativeRendererSharedAuthorityEnabled() {
  return ac6::nr2::SharedAuthorityEnabled();
}

bool NativeRendererPrepareVulkanDraw(const NativeVulkanDrawRequest& request,
                                     NativeVulkanDrawPlan& plan) {
  plan = {};
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (!ac6::nr2::PrepareAuthoritativeVulkanDraw(
          request.command_processor, request.device, request.pipeline_layout,
          request.pipeline_description_hash, request.pipeline_description,
          sizeof(request.pipeline_description),
          request.vertex_shader, request.pixel_shader,
          request.generated_geometry_shader, request.host_vertex_shader_type,
          request.dynamic_rendering, request.host_render_targets,
          request.memexport_writes_possible, &pipeline)) {
    return false;
  }
  plan.device = request.device;
  plan.pipeline_layout = request.pipeline_layout;
  plan.pipeline = pipeline;
  return pipeline != VK_NULL_HANDLE;
}

void NativeRendererOnVulkanShutdown(VkDevice device) {
  ac6::nr2::ShutdownAuthoritativeVulkanDevice(device);
}
namespace {

std::mutex g_snapshot_mutex;
BackendDiagnosticsSnapshot g_snapshot{};
std::unordered_map<uint64_t, uint32_t> g_signature_hits;

bool ShouldLogSignature(const BackendDiagnosticsSnapshot& snapshot) {
  if (!REXCVAR_GET(ac6_backend_log_signatures) || snapshot.latest_signature.stable_id == 0) {
    return false;
  }
  return snapshot.repeated_signature_count == 1 ||
         (snapshot.repeated_signature_count % 32) == 0;
}

}  // namespace

void AnalyzeFrameBoundary(
    const ac6::d3d::FrameCaptureSnapshot& frame_capture,
    const ac6::d3d::FrameCaptureSummary& capture_summary,
    const ac6::d3d::ShadowState& shadow_state,
    const rex::system::GraphicsSwapSubmission* swap_submission,
    const uint64_t swap_submission_sequence,
    const uint64_t guest_vblank_interval_ticks,
    const uint64_t last_guest_vblank_tick,
    const ac6::FrameStats& frame_stats,
    const rex::audio::AudioTelemetrySnapshot* audio_telemetry,
    const rex::audio::AudioClientTimingSnapshot* audio_timing) {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);

  const uint64_t previous_swap_sequence = g_snapshot.swap_submission_sequence;
  g_snapshot.valid = true;
  g_snapshot.frame_index = capture_summary.frame_index;
  g_snapshot.swap_submission_sequence = swap_submission_sequence;
  g_snapshot.swap_submission_valid = swap_submission != nullptr;
  g_snapshot.guest_vblank_interval_ticks = guest_vblank_interval_ticks;
  g_snapshot.last_guest_vblank_tick = last_guest_vblank_tick;
  g_snapshot.host_frame_time_ms = frame_stats.frame_time_ms;
  g_snapshot.host_fps = frame_stats.fps;
  g_snapshot.host_frame_count = frame_stats.frame_count;
  g_snapshot.capture_draw_count = capture_summary.draw_count;
  g_snapshot.capture_clear_count = capture_summary.clear_count;
  g_snapshot.capture_resolve_count = capture_summary.resolve_count;

  if (swap_submission) {
    g_snapshot.frontbuffer_width = swap_submission->frontbuffer_width;
    g_snapshot.frontbuffer_height = swap_submission->frontbuffer_height;
    g_snapshot.texture_format = swap_submission->texture_format;
    g_snapshot.color_space = swap_submission->color_space;
  } else {
    g_snapshot.frontbuffer_width = 0;
    g_snapshot.frontbuffer_height = 0;
    g_snapshot.texture_format = 0;
    g_snapshot.color_space = 0;
  }

  if (swap_submission_sequence != previous_swap_sequence) {
    g_snapshot.swap_source = SwapSourceType::kUnknown;
    g_snapshot.active_vertex_shader_hash = 0;
    g_snapshot.active_pixel_shader_hash = 0;
  }

  if (audio_telemetry) {
    g_snapshot.audio_active_clients = audio_telemetry->active_clients;
    g_snapshot.audio_queued_frames = audio_telemetry->queued_frames;
    g_snapshot.audio_peak_queued_frames = audio_telemetry->peak_queued_frames;
    g_snapshot.audio_dropped_frames = audio_telemetry->dropped_frames;
    g_snapshot.audio_underruns = audio_telemetry->underruns;
    g_snapshot.audio_silence_injections = audio_telemetry->silence_injections;
    g_snapshot.audio_backend_name = audio_telemetry->backend_name;
  } else {
    g_snapshot.audio_active_clients = 0;
    g_snapshot.audio_queued_frames = 0;
    g_snapshot.audio_peak_queued_frames = 0;
    g_snapshot.audio_dropped_frames = 0;
    g_snapshot.audio_underruns = 0;
    g_snapshot.audio_silence_injections = 0;
    g_snapshot.audio_backend_name.clear();
  }

  if (audio_timing) {
    g_snapshot.audio_timing_valid = true;
    g_snapshot.audio_consumed_frames = audio_timing->consumed_frames;
    g_snapshot.audio_queued_played_frames = audio_timing->queued_played_frames;
    g_snapshot.audio_submitted_tic = audio_timing->submitted_tic;
    g_snapshot.audio_host_elapsed_tic = audio_timing->host_elapsed_tic;
    g_snapshot.audio_startup_inflight_frames = audio_timing->startup_inflight_frames;
    g_snapshot.audio_callback_dispatch_count = audio_timing->callback_dispatch_count;
    g_snapshot.audio_callback_throttle_count = audio_timing->callback_throttle_count;
  } else {
    g_snapshot.audio_timing_valid = false;
    g_snapshot.audio_consumed_frames = 0;
    g_snapshot.audio_queued_played_frames = 0;
    g_snapshot.audio_submitted_tic = 0;
    g_snapshot.audio_host_elapsed_tic = 0;
    g_snapshot.audio_startup_inflight_frames = 0;
    g_snapshot.audio_callback_dispatch_count = 0;
    g_snapshot.audio_callback_throttle_count = 0;
  }

  if (REXCVAR_GET(ac6_backend_signature_diagnostics) ||
      REXCVAR_GET(ac6_backend_log_signatures)) {
    g_snapshot.latest_signature = BuildRenderEventSignature(
        frame_capture, capture_summary, shadow_state, swap_submission,
        g_snapshot.active_vertex_shader_hash, g_snapshot.active_pixel_shader_hash);
    g_snapshot.latest_signature.classification =
        ClassifySignature(g_snapshot.latest_signature);
    g_snapshot.latest_signature_tags = BuildSignatureTags(g_snapshot.latest_signature);
    g_snapshot.repeated_signature_count =
        ++g_signature_hits[g_snapshot.latest_signature.stable_id];
  } else {
    g_snapshot.latest_signature = {};
    g_snapshot.latest_signature_tags.clear();
    g_snapshot.repeated_signature_count = 0;
  }

  if (ShouldLogSignature(g_snapshot)) {
    REXLOG_INFO(
        "AC6 backend signature frame={} class={} id={:016X} hits={} tags={} draws={} resolves={} viewport={}x{} pointlist={}",
        g_snapshot.frame_index, ToString(g_snapshot.latest_signature.classification),
        g_snapshot.latest_signature.stable_id, g_snapshot.repeated_signature_count,
        g_snapshot.latest_signature_tags, g_snapshot.capture_draw_count,
        g_snapshot.capture_resolve_count, g_snapshot.latest_signature.viewport_width,
        g_snapshot.latest_signature.viewport_height,
        g_snapshot.latest_signature.topology_pointlist_count);
  }
}

void ReportSwapDecision(const rex::system::GraphicsSwapSubmission& submission,
                        const uint64_t submission_sequence,
                        const SwapSourceType swap_source,
                        const bool swap_source_scaled,
                        const uint32_t guest_output_width,
                        const uint32_t guest_output_height,
                        const uint32_t source_width,
                        const uint32_t source_height,
                        const uint64_t active_vertex_shader_hash,
                        const uint64_t active_pixel_shader_hash) {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);

  g_snapshot.valid = true;
  g_snapshot.swap_submission_valid = true;
  g_snapshot.swap_submission_sequence = submission_sequence;
  g_snapshot.swap_source = swap_source;
  g_snapshot.swap_source_scaled = swap_source_scaled;
  g_snapshot.guest_output_width = guest_output_width;
  g_snapshot.guest_output_height = guest_output_height;
  g_snapshot.source_width = source_width;
  g_snapshot.source_height = source_height;
  g_snapshot.frontbuffer_width = submission.frontbuffer_width;
  g_snapshot.frontbuffer_height = submission.frontbuffer_height;
  g_snapshot.texture_format = submission.texture_format;
  g_snapshot.color_space = submission.color_space;
  g_snapshot.active_vertex_shader_hash = active_vertex_shader_hash;
  g_snapshot.active_pixel_shader_hash = active_pixel_shader_hash;
  g_snapshot.latest_signature.active_vertex_shader_hash = active_vertex_shader_hash;
  g_snapshot.latest_signature.active_pixel_shader_hash = active_pixel_shader_hash;

  if (REXCVAR_GET(ac6_backend_debug_swap)) {
    REXLOG_INFO(
        "AC6 swap source={} guest={}x{} source={}x{} scaled={} vs={:016X} ps={:016X}",
        ToString(swap_source), guest_output_width, guest_output_height, source_width,
        source_height, swap_source_scaled ? "yes" : "no", active_vertex_shader_hash,
        active_pixel_shader_hash);
  }
}

BackendDiagnosticsSnapshot GetDiagnosticsSnapshot() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot;
}

void ShutdownDiagnostics() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  g_snapshot = {};
  g_signature_hits.clear();
}

const char* ToString(const SwapSourceType swap_source) {
  switch (swap_source) {
    case SwapSourceType::kGuestSwapTexture:
      return "guest_swap_texture";
    case SwapSourceType::kDirectDisplayFallback:
      return "direct_display_fallback";
    case SwapSourceType::kExperimentalReplayOverride:
      return "experimental_replay_override";
    case SwapSourceType::kUnknown:
    default:
      return "unknown";
  }
}

}  // namespace ac6::backend
