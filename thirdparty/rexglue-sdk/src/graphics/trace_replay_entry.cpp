#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>

// NOTE (Linux port of the macOS trace replay harness):
// - The SDK's src/graphics/flags.cpp already defines the
//   ac6_neutralize_deswizzle_hashes and ac6_fix_hoisted_fetch_gradients_hashes
//   cvars in this static-library build, so unlike the macOS GPU-plugin build
//   this file must NOT define them again (duplicate cvar registration).
// - The macOS CoreGraphics/ImageIO PNG writer is replaced with a dependency-free
//   binary PPM (P6) writer.

namespace {

void ApplyAc6MacReplayDefaults() {
  // Mirror the current AC6 macOS launcher defaults. Trace replay otherwise
  // uses the backend's generic defaults and is not comparable to gameplay.
  rex::cvar::SetFlagByName("async_shader_compilation", "false");
  rex::cvar::SetFlagByName("vulkan_pipeline_creation_threads", "1");
  rex::cvar::SetFlagByName("vulkan_dynamic_rendering", "false");
  rex::cvar::SetFlagByName("occlusion_query_enable", "false");
  rex::cvar::SetFlagByName("direct_host_resolve", "false");
  rex::cvar::SetFlagByName("depth_float24_round", "false");
  rex::cvar::SetFlagByName("depth_float24_convert_in_pixel_shader", "true");
  // depth_float24_sample_rate and vulkan_vertex_buffer_residency_cache do not
  // exist in this SDK tree; the macOS harness sets them, this port drops them.
  rex::cvar::SetFlagByName("anisotropic_override", "1");
  rex::cvar::SetFlagByName("primitive_processor_cache_min_indices", "4096");
  // AC6 requires this before Runtime::Setup constructs the pipeline cache.
  // Keep one explicit override for true/false cache-key regression tests.
  rex::cvar::SetFlagByName("vfetch_index_rounding_bias", "true");
  if (const char* value = std::getenv("REX_TRACE_NATIVE_2X_MSAA")) {
    rex::cvar::SetFlagByName("native_2x_msaa", value);
  }
  if (const char* value = std::getenv("REX_TRACE_PRIMITIVE_CACHE_MIN_INDICES")) {
    rex::cvar::SetFlagByName("primitive_processor_cache_min_indices", value);
  }
  if (const char* value = std::getenv("REX_TRACE_VFETCH_ROUNDING_BIAS")) {
    rex::cvar::SetFlagByName("vfetch_index_rounding_bias", value);
  }
  const char* hoisted_gradient_hashes =
      std::getenv("REX_TRACE_HOISTED_GRADIENT_HASHES");
  rex::cvar::SetFlagByName("ac6_fix_hoisted_fetch_gradients_hashes",
                          hoisted_gradient_hashes ? hoisted_gradient_hashes : "");
  if (const char* value = std::getenv("REX_TRACE_RENDER_TARGET_PATH")) {
    rex::cvar::SetFlagByName("render_target_path_vulkan", value);
  }
  if (const char* value = std::getenv("REX_TRACE_DEPTH_FLOAT24_CONVERT")) {
    rex::cvar::SetFlagByName("depth_float24_convert_in_pixel_shader", value);
  }
  if (const char* value = std::getenv("REX_TRACE_DYNAMIC_RENDERING")) {
    rex::cvar::SetFlagByName("vulkan_dynamic_rendering", value);
  }
  if (const char* value = std::getenv("REX_TRACE_DIRECT_HOST_RESOLVE")) {
    rex::cvar::SetFlagByName("direct_host_resolve", value);
  }
  const char* hashes = std::getenv("REX_TRACE_DESWIZZLE_HASHES");
  rex::cvar::SetFlagByName("ac6_neutralize_deswizzle_hashes",
                          hashes ? hashes : "7d22894002d16018, 17e5e4ac3e713245:4");
  if (const char* dump_path = std::getenv("REX_TRACE_DUMP_SHADERS")) {
    rex::cvar::SetFlagByName("dump_shaders", dump_path);
  }
  std::fprintf(stderr, "Effective AC6 de-swizzle allowlist: '%s'\n",
               REXCVAR_GET(ac6_neutralize_deswizzle_hashes).c_str());
  std::fprintf(stderr, "Effective hoisted-gradient allowlist: '%s'\n",
               REXCVAR_GET(ac6_fix_hoisted_fetch_gradients_hashes).c_str());
  std::fprintf(stderr, "Shader dump path: '%s'\n", REXCVAR_GET(dump_shaders).c_str());
  std::fprintf(stderr, "render_target_path_vulkan='%s', direct_host_resolve=%s, "
                       "depth_float24_convert_in_pixel_shader=%s, "
                       "vulkan_dynamic_rendering=%s, native 2x MSAA=%s, "
                       "primitive cache minimum=%d, "
                       "vfetch rounding bias=%s\n",
               REXCVAR_GET(render_target_path_vulkan).c_str(),
               REXCVAR_GET(direct_host_resolve) ? "true" : "false",
               REXCVAR_GET(depth_float24_convert_in_pixel_shader) ? "true" : "false",
               REXCVAR_GET(vulkan_dynamic_rendering) ? "true" : "false",
               REXCVAR_GET(native_2x_msaa) ? "true" : "false",
               REXCVAR_GET(primitive_processor_cache_min_indices),
               REXCVAR_GET(vfetch_index_rounding_bias) ? "true" : "false");
}

// Binary PPM (P6) writer: RawImage is R8 G8 B8 X8 with rows at `stride`
// bytes; alpha is dropped. Returns the path actually written via
// `actual_path_out`.
bool WritePpm(const std::filesystem::path& requested_path,
              const rex::ui::RawImage& image,
              std::filesystem::path& actual_path_out) {
  std::filesystem::path path = requested_path;
  if (path.extension() != ".ppm") {
    path.replace_extension(".ppm");
  }
  actual_path_out = path;
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      std::fprintf(stderr, "Unable to create output directory: %s\n",
                   error.message().c_str());
      return false;
    }
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file << "P6\n" << image.width << " " << image.height << "\n255\n";
  std::vector<uint8_t> row(size_t(image.width) * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[size_t(x) * 3 + 0] = src[size_t(x) * 4 + 0];
      row[size_t(x) * 3 + 1] = src[size_t(x) * 4 + 1];
      row[size_t(x) * 3 + 2] = src[size_t(x) * 4 + 2];
    }
    file.write(reinterpret_cast<const char*>(row.data()),
               std::streamsize(row.size()));
  }
  file.flush();
  return file.good();
}

}  // namespace

extern "C" int rex_gpu_replay_trace(const char* trace_path,
                                    const char* output_path,
                                    int frame_index,
                                    int command_index) {
  if (!trace_path || !trace_path[0] || !output_path || !output_path[0]) {
    std::fprintf(stderr, "Trace and output paths are required\n");
    return 2;
  }

  using Clock = std::chrono::steady_clock;
  const auto time_start = Clock::now();
  ApplyAc6MacReplayDefaults();

  auto graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  auto* graphics_ptr = graphics.get();
  // Set up an offscreen presenter before guest-GPU setup. Vulkan presentation
  // support and UI samplers must be selected when the provider is created.
  if (XFAILED(graphics_ptr->SetupPresentation(nullptr))) {
    std::fprintf(stderr, "Unable to create offscreen Vulkan presenter\n");
    return 3;
  }

  const char* trace_cache_root = std::getenv("REX_TRACE_CACHE_ROOT");
  auto runtime = std::make_unique<rex::Runtime>(std::filesystem::path());
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  if (XFAILED(runtime->Setup(std::move(config)))) {
    std::fprintf(stderr, "Unable to initialize trace replay runtime\n");
    return 3;
  }
  const auto time_setup_complete = Clock::now();
  if (trace_cache_root && trace_cache_root[0]) {
    graphics_ptr->InitializeShaderStorage(trace_cache_root, 0x4E4D07D1, true);
    std::fprintf(stderr, "Trace replay shader and pipeline cache: %s\n", trace_cache_root);
  }

  int result = 0;
  {
    rex::graphics::TracePlayer player(graphics_ptr);
    if (!player.Open(trace_path)) {
      std::fprintf(stderr, "Unable to open trace: %s\n", trace_path);
      result = 4;
    } else if (frame_index != 0) {
      // The captured AC6 trace is a single-frame trace. TracePlayer's legacy
      // SeekFrame API eagerly replays a frame, so reject other indices rather
      // than silently producing a misleading image.
      std::fprintf(stderr, "This bounded replay harness currently supports frame 0 only "
                           "(trace contains %d frame(s))\n",
                   player.frame_count());
      result = 5;
    } else if (player.frame_count() == 0 || !player.current_frame()) {
      std::fprintf(stderr, "Trace contains no frames\n");
      result = 5;
    } else {
      const int command_count = static_cast<int>(player.current_frame()->commands.size());
      const int target_command = command_index < 0 ? command_count - 1 : command_index;
      if (target_command < 0 || target_command >= command_count) {
        std::fprintf(stderr, "Command index %d is outside [0, %d)\n", target_command,
                     command_count);
        result = 5;
      } else {
        std::fprintf(stderr, "Replaying frame 0 command %d of %d\n", target_command,
                     command_count);
        const auto time_replay_start = Clock::now();
        player.SeekCommand(target_command);
        player.WaitOnPlayback();
        const auto time_checkpoint_complete = Clock::now();

        // A draw command by itself doesn't publish an image to the presenter.
        // Execute the final command's full trace range, including generic
        // resolve/copy packets recorded between the last draw and XE_SWAP.
        // This intentionally skips intervening draws, projecting the checkpoint
        // render targets through the real final presentation tail.
        if (player.current_frame()->commands[target_command].type !=
            rex::graphics::TraceReader::Frame::Command::Type::kSwap) {
          const auto swap = std::find_if(
              player.current_frame()->commands.rbegin(),
              player.current_frame()->commands.rend(), [](const auto& command) {
                return command.type ==
                       rex::graphics::TraceReader::Frame::Command::Type::kSwap;
              });
          if (swap == player.current_frame()->commands.rend()) {
            std::fprintf(stderr, "Trace has no recorded final presentation tail\n");
            result = 6;
          } else {
            const char* checkpoint_trace_dir =
                std::getenv("REX_TRACE_CHECKPOINT_TRACE_DIR");
            if (checkpoint_trace_dir && checkpoint_trace_dir[0]) {
              std::filesystem::create_directories(checkpoint_trace_dir);
              graphics_ptr->command_processor()->RequestFrameTrace(checkpoint_trace_dir);
            }
            player.ReplayRange(swap->start_ptr, size_t(swap->end_ptr - swap->start_ptr),
                               true);
            player.WaitOnPlayback();
            if (checkpoint_trace_dir && checkpoint_trace_dir[0]) {
              // The first swap opens a one-frame trace and snapshots current
              // host render targets into its initial EDRAM state. A second
              // swap closes the trace, leaving a self-contained checkpoint.
              player.ReplayRange(swap->start_ptr, size_t(swap->end_ptr - swap->start_ptr),
                                 true);
              player.WaitOnPlayback();
              std::fprintf(stderr, "Saved checkpoint EDRAM trace under %s\n",
                           checkpoint_trace_dir);
            }
          }
        }
        const auto time_presentation_complete = Clock::now();

        rex::ui::RawImage raw_image;
        rex::ui::Presenter* presenter = graphics_ptr->presenter();
        std::filesystem::path actual_output_path;
        if (!result && (!presenter || !presenter->CaptureGuestOutput(raw_image))) {
          std::fprintf(stderr,
                       "No guest output is available at command %d. Arbitrary draw capture "
                       "requires a backend render-target readback; commands through the final "
                       "swap are capturable.\n",
                       target_command);
          result = 6;
        } else if (!result && !WritePpm(output_path, raw_image, actual_output_path)) {
          std::fprintf(stderr, "Failed to write PPM: %s\n",
                       actual_output_path.c_str());
          result = 7;
        } else if (!result) {
          std::fprintf(stderr, "Saved %ux%u guest output to %s\n", raw_image.width,
                       raw_image.height, actual_output_path.c_str());
        }
        const auto time_capture_complete = Clock::now();
        const auto milliseconds = [](Clock::duration duration) {
          return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::fprintf(stderr,
                     "Replay timing: setup %.3f ms; checkpoint %.3f ms; "
                     "presentation %.3f ms; capture %.3f ms; elapsed %.3f ms\n",
                     milliseconds(time_setup_complete - time_start),
                     milliseconds(time_checkpoint_complete - time_replay_start),
                     milliseconds(time_presentation_complete - time_checkpoint_complete),
                     milliseconds(time_capture_complete - time_presentation_complete),
                     milliseconds(time_capture_complete - time_start));
      }
    }
  }

  runtime.reset();
  return result;
}
