// ac6-nr2-trace-draw — M3 slice 2 headless native-draw harness.
//
// Drives the nr2 native draw path (src/ac6_native_renderer2/native_renderer.cpp)
// from a captured GPU trace instead of the live game, so the first native draw
// can be executed and oracle-diffed fully offscreen:
//
//   scan TRACE.xtr [OUT.tsv]
//     Replays the trace command by command on the emulated command processor
//     (same TracePlayer the rexgpu-vulkan-trace-dump oracle uses) and dumps
//     one TSV row of decoded draw state per command: primitive type, index
//     count, VS/PS ucode hashes + texture binding counts, blend/depth/mode
//     control registers, RT0 info. Used to choose a suitable slice-2 draw
//     (untextured, no blend, no depth/stencil, plain topology) with evidence.
//
//   draw TRACE.xtr COMMAND_INDEX OUT.ppm [PIPELINE_STORE [SHADER_DIR]]
//     Replays the trace through COMMAND_INDEX (inclusive - the register file
//     then holds exactly the state that draw executed with), snapshots the
//     emulated GPU state (registers + active shader identities + guest
//     physical memory host pointer) into an ac6::nr2::DrawSnapshot, and hands
//     it to ac6::nr2::ExecuteSnapshotOffscreen - the same code path the
//     in-game ac6_native_capture_draw trigger uses (guest memory imported
//     zero-copy via VK_EXT_external_memory_host where possible, the five
//     set-1 constant UBOs rebuilt from the register snapshot, offscreen
//     dynamic-rendering draw with a slice-1 pipeline + M1 SPIR-V). The
//     native result is written to OUT.ppm.
//
//   resolve TRACE.xtr CMD OUT.bin [ADDR_HEX BYTES]
//     Replays through command CMD with vulkan_readback_resolve=true, so any
//     resolve (copy) draw inside the replayed range writes its output back
//     into CPU-visible guest memory, then dumps the copy destination (from
//     RB_COPY_DEST_BASE/PITCH at CMD, or an explicit ADDR/BYTES). Seeking to
//     the game's own resolve command right after a chosen draw yields the
//     emulated renderer's raw resolved RT bytes - the working oracle for
//     mid-frame draws (the REX_TRACE_CHECKPOINT_TRACE_DIR EDRAM-snapshot
//     path livelocks in this tree; see docs/M3_SLICE2_REPORT.md).
//
//   frame-throughput TRACE.xtr [WARMUP_FRAMES [MEASURED_FRAMES]]
//     Replays the complete captured frame through its final swap with the live
//     renderer settings and readback_resolve=none. Before every sample, an
//     untimed prefix restores the trace's initial memory, register, and EDRAM
//     state. The timed body begins at the first GPU packet and includes all
//     subsequent dynamic records through the final swap, while retaining host
//     shader and pipeline caches. Warm-up frames are excluded from the result.
//     Each frame is followed by one explicit vkDeviceWaitIdle, producing an
//     isolated end-to-end renderer latency measurement rather than timing
//     trace hydration or only command recording while GPU work remains queued.
//     REX_TRACE_PRIMITIVE_CACHE_MIN_INDICES may select the exact 0 or 4096
//     primitive-processor cache threshold before Vulkan setup. Other values
//     fail closed so a mislabeled A/B arm cannot run.
//
//   edram CHECKPOINT.xtr OUT.bin
//     Extracts the decompressed 10MB EDRAM snapshot from a checkpoint trace
//     (produced by rexgpu-vulkan-trace-dump with REX_TRACE_CHECKPOINT_TRACE_DIR
//     set: the initial kEdramSnapshot holds the EDRAM contents at the seeked
//     command). Two such dumps (at N-1 and N) give the raw render-target-level
//     oracle for draw N - the presented-image oracle cannot see mid-frame
//     draws because the presentation tail composites from guest memory, which
//     only the (never-replayed) frontbuffer resolve at the end of the frame
//     updates. Untiling/diffing happens offline (m3-slice2-artifacts scripts).
//
// The emulated oracle image for the same command comes from the existing
// rexgpu-vulkan-trace-dump tool; the slice-2 parity diff compares the two.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "native_renderer.h"

extern "C" int rex_gpu_replay_trace(const char* trace_path,
                                     const char* output_path, int frame_index,
                                     int command_index);

namespace {

using namespace rex::graphics;

#if !defined(_WIN32)
// RenderDoc is injected at process startup (LD_PRELOAD) so it can hook Vulkan
// device creation. Keep this tiny ABI shim local to the trace-only harness;
// SteamOS's renderdoc-minimal package installs the library but may omit its
// development header. The function-table ordering is stable in API 1.6.0.
using RenderDocGetApi = int (*)(int version, void** out_api);
using RenderDocSetCapturePath = void (*)(const char* path_template);
using RenderDocStartCapture = void (*)(void* device, void* window);
using RenderDocEndCapture = uint32_t (*)(void* device, void* window);

struct RenderDocApiShim {
  void* entries[22];
};

RenderDocApiShim* GetRenderDocApi() {
  auto get_api = reinterpret_cast<RenderDocGetApi>(
      dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"));
  if (!get_api) {
    std::fprintf(stderr,
                 "AC6_NR2_RENDERDOC_CAPTURE requested, but RenderDoc is not "
                 "injected (RENDERDOC_GetAPI missing)\n");
    return nullptr;
  }
  RenderDocApiShim* api = nullptr;
  constexpr int kRenderDocApiVersion160 = 10600;
  if (!get_api(kRenderDocApiVersion160, reinterpret_cast<void**>(&api)) ||
      !api) {
    std::fprintf(stderr, "RenderDoc API 1.6.0 is unavailable\n");
    return nullptr;
  }
  return api;
}
#endif

// EDRAM snapshot extractor: subclasses TraceReader for Open/DecompressMemory
// and walks the raw trace stream for the first kEdramSnapshot command.
class EdramSnapshotReader : public TraceReader {
 public:
  int DumpFirstSnapshot(const char* out_path) {
    const uint8_t* trace_ptr = trace_data_ + sizeof(TraceHeader);
    const uint8_t* trace_end = trace_data_ + trace_size_;
    while (trace_ptr < trace_end) {
      auto type = static_cast<TraceCommandType>(
          *reinterpret_cast<const uint32_t*>(trace_ptr));
      switch (type) {
        case TraceCommandType::kPrimaryBufferStart: {
          auto cmd = reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kPrimaryBufferEnd:
          trace_ptr += sizeof(PrimaryBufferEndCommand);
          break;
        case TraceCommandType::kIndirectBufferStart: {
          auto cmd = reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kIndirectBufferEnd:
          trace_ptr += sizeof(IndirectBufferEndCommand);
          break;
        case TraceCommandType::kPacketStart: {
          auto cmd = reinterpret_cast<const PacketStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kPacketEnd:
          trace_ptr += sizeof(PacketEndCommand);
          break;
        case TraceCommandType::kMemoryRead:
        case TraceCommandType::kMemoryWrite: {
          auto cmd = reinterpret_cast<const MemoryCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          break;
        }
        case TraceCommandType::kEdramSnapshot: {
          auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd);
          std::vector<uint8_t> edram(xenos::kEdramSizeBytes);
          if (!DecompressMemory(cmd->encoding_format, trace_ptr, cmd->encoded_length,
                                edram.data(), edram.size())) {
            std::fprintf(stderr, "EDRAM snapshot decompression failed\n");
            return 8;
          }
          FILE* f = std::fopen(out_path, "wb");
          if (!f || std::fwrite(edram.data(), edram.size(), 1, f) != 1) {
            if (f) std::fclose(f);
            std::fprintf(stderr, "Cannot write EDRAM dump '%s'\n", out_path);
            return 8;
          }
          std::fclose(f);
          std::fprintf(stderr, "EDRAM snapshot (%zu bytes) written to %s\n",
                       edram.size(), out_path);
          return 0;
        }
        case TraceCommandType::kEvent:
          trace_ptr += sizeof(EventCommand);
          break;
        case TraceCommandType::kRegisters: {
          auto cmd = reinterpret_cast<const RegistersCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          break;
        }
        case TraceCommandType::kGammaRamp: {
          auto cmd = reinterpret_cast<const GammaRampCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          break;
        }
        default:
          std::fprintf(stderr, "Unknown trace command type %u at offset %zd\n",
                       uint32_t(type), trace_ptr - trace_data_);
          return 8;
      }
    }
    std::fprintf(stderr, "No EDRAM snapshot found in the trace\n");
    return 8;
  }
};

// Mirror of the trace replay harness defaults (trace_replay_entry.cpp
// ApplyAc6MacReplayDefaults) so the emulated command processor behaves the
// same way here as in the rexgpu-vulkan-trace-dump oracle runs.
void ApplyReplayDefaults(bool force_live_renderer_settings = false) {
  if (const char* dump = std::getenv("AC6_NR2_DUMP_SHADERS")) {
    rex::cvar::SetFlagByName("dump_shaders", dump);
  }
  const auto env_flag = [](const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
  };
  // Shared-authority pipelines are deliberately restricted to dynamic
  // rendering. Keep an explicit dynamic-only control mode so the emulator
  // oracle and shared run can use identical render-scope configuration.
  const bool shared_authority =
      env_flag("AC6_NR2_SHARED_AUTHORITY_TRACE");
  const bool live_renderer_settings =
      force_live_renderer_settings || shared_authority ||
      env_flag("AC6_NR2_LIVE_RENDERER_SETTINGS_TRACE");
  const bool dynamic_rendering =
      force_live_renderer_settings || shared_authority ||
      env_flag("AC6_NR2_DYNAMIC_RENDERING_TRACE") ||
      env_flag("REX_TRACE_DYNAMIC_RENDERING");
  const bool batch_primary_submissions =
      env_flag("AC6_NR2_BATCH_PRIMARY_SUBMISSIONS");
  const bool disable_3d_to_2d_texture =
      env_flag("AC6_NR2_DISABLE_3D_TO_2D_TEXTURE");
  const bool selective_texture_invalidation =
      env_flag("AC6_NR2_SELECTIVE_TEXTURE_INVALIDATION");
  const char* rect_clear_env =
      std::getenv("AC6_NR2_RECT_CLEAR_LOWERING");
  const bool rect_clear_requested =
      rect_clear_env && std::strcmp(rect_clear_env, "1") == 0;
  rex::cvar::SetFlagByName("async_shader_compilation", "false");
  rex::cvar::SetFlagByName("vulkan_pipeline_creation_threads", "1");
  rex::cvar::SetFlagByName("vulkan_dynamic_rendering",
                           dynamic_rendering ? "true" : "false");
  // Let the deterministic full-frame harness A/B command submission granularity
  // without changing the shipped default or adding trace-tool CLI arguments.
  rex::cvar::SetFlagByName("vulkan_submit_on_primary_buffer_end",
                           batch_primary_submissions ? "false" : "true");
  rex::cvar::SetFlagByName("gpu_3d_to_2d_texture",
                           disable_3d_to_2d_texture ? "false" : "true");
  rex::cvar::SetFlagByName("texture_cache_selective_binding_invalidation",
                           selective_texture_invalidation ? "true" : "false");
  if (const char* gpu_profile_interval =
          std::getenv("AC6_NR2_GPU_PASS_PROFILE_INTERVAL")) {
    rex::cvar::SetFlagByName("vulkan_gpu_pass_profile_interval",
                             gpu_profile_interval);
  }
  rex::cvar::SetFlagByName("occlusion_query_enable", "false");
  rex::cvar::SetFlagByName("direct_host_resolve", "false");
  rex::cvar::SetFlagByName("depth_float24_round", "false");
  // The preserved legacy oracle uses pixel-shader float24 conversion, while
  // the Deck gameplay configuration and its offline SPIR-V corpus use the
  // render-target conversion path. Keep the legacy default for the existing
  // regression benchmark, but let shared-authority and explicit live-setting
  // controls exercise the exact shipped configuration.
  rex::cvar::SetFlagByName("depth_float24_convert_in_pixel_shader",
                           live_renderer_settings ? "false" : "true");
  rex::cvar::SetFlagByName("anisotropic_override", "1");
  rex::cvar::SetFlagByName("primitive_processor_cache_min_indices", "4096");
  rex::cvar::SetFlagByName("vulkan_ac6_clear_discard_transfers",
      env_flag("AC6_NR2_CLEAR_DISCARD_TRANSFERS") ? "true" : "false");
  const bool rect_clear_set = rex::cvar::SetFlagByName(
      "vulkan_ac6_rect_clear_lowering",
      rect_clear_requested ? "true" : "false");
  const bool rect_clear_oracle_set = rex::cvar::SetFlagByName(
      "vulkan_ac6_rect_clear_oracle_telemetry", "false");
  const std::string rect_clear_effective =
      rex::cvar::GetFlagByName("vulkan_ac6_rect_clear_lowering");
  const std::string rect_clear_oracle_effective = rex::cvar::GetFlagByName(
      "vulkan_ac6_rect_clear_oracle_telemetry");
  // Set before Runtime::Setup / VulkanPipelineCache construction so freshly
  // generated XPSo records carry the same immutable modification bit as live
  // AC6. The generic trace override is retained for deliberate true/false A/B.
  const char* vfetch_rounding_bias =
      std::getenv("REX_TRACE_VFETCH_ROUNDING_BIAS");
  rex::cvar::SetFlagByName(
      "vfetch_index_rounding_bias",
      vfetch_rounding_bias ? vfetch_rounding_bias : "true");
  const char* hoisted_gradient_hashes =
      std::getenv("REX_TRACE_HOISTED_GRADIENT_HASHES");
  rex::cvar::SetFlagByName(
      "ac6_fix_hoisted_fetch_gradients_hashes",
      hoisted_gradient_hashes ? hoisted_gradient_hashes : "");
  rex::cvar::SetFlagByName("ac6_neutralize_deswizzle_hashes",
                           "7d22894002d16018, 17e5e4ac3e713245:4");
  std::fprintf(stderr, "Effective vfetch rounding bias before setup: %s\n",
               REXCVAR_GET(vfetch_index_rounding_bias) ? "true" : "false");
  std::fprintf(stderr,
               "Effective hoisted-gradient allowlist before setup: '%s'\n",
               REXCVAR_GET(ac6_fix_hoisted_fetch_gradients_hashes).c_str());
  std::fprintf(stderr, "Effective anisotropic override before setup: %d\n",
               REXCVAR_GET(anisotropic_override));
  std::fprintf(
      stderr,
      "Trace rectangle-clear lowering before setup: "
      "AC6_NR2_RECT_CLEAR_LOWERING='%s' requested=%s set=%s effective='%s' "
      "oracle_set=%s oracle_effective='%s'\n",
      rect_clear_env ? rect_clear_env : "",
      rect_clear_requested ? "true" : "false",
      rect_clear_set ? "true" : "false", rect_clear_effective.c_str(),
      rect_clear_oracle_set ? "true" : "false", rect_clear_oracle_effective.c_str());
}

bool ApplyWaterVrsTraceOverride() {
  const char* env_value = std::getenv("AC6_NR2_WATER_VRS");
  const bool requested = env_value && std::strcmp(env_value, "1") == 0;
  const bool set = rex::cvar::SetFlagByName(
      "ac6_native_water_vrs", requested ? "true" : "false");
  const std::string effective =
      rex::cvar::GetFlagByName("ac6_native_water_vrs");
  std::fprintf(
      stderr,
      "Trace exact-water VRS before setup: AC6_NR2_WATER_VRS='%s' "
      "requested=%s set=%s effective='%s'\n",
      env_value ? env_value : "", requested ? "true" : "false",
      set ? "true" : "false", effective.c_str());
  if (!requested) {
    return effective == "false";
  }
  return set && effective == "true";
}

bool ParseInt(std::string_view text, int& value_out) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, value_out);
  return result.ec == std::errc() && result.ptr == end;
}

bool ApplyPrimitiveCacheThresholdTraceOverride() {
  const char* env_value =
      std::getenv("REX_TRACE_PRIMITIVE_CACHE_MIN_INDICES");
  const std::string_view requested =
      env_value ? std::string_view(env_value) : std::string_view("4096");
  if (requested != "0" && requested != "4096") {
    std::fprintf(
        stderr,
        "Invalid REX_TRACE_PRIMITIVE_CACHE_MIN_INDICES='%s'; expected "
        "exactly 0 or 4096\n",
        env_value ? env_value : "");
    return false;
  }

  const std::string requested_string(requested);
  const bool set = rex::cvar::SetFlagByName(
      "primitive_processor_cache_min_indices", requested_string.c_str());
  const std::string effective =
      rex::cvar::GetFlagByName("primitive_processor_cache_min_indices");
  std::fprintf(
      stderr,
      "frame-throughput effective: primitive_cache_min_indices=%s "
      "requested='%s' set=%s\n",
      effective.c_str(), requested_string.c_str(), set ? "true" : "false");
  return set && effective == requested_string;
}

void CopyShaderInfo(Shader* shader, ac6::nr2::DrawSnapshotShader& dst) {
  if (!shader) {
    return;
  }
  dst.valid = true;
  dst.ucode_hash = shader->ucode_data_hash();
  dst.ucode_analyzed = shader->is_ucode_analyzed();
  if (dst.ucode_analyzed) {
    const Shader::ConstantRegisterMap& map = shader->constant_register_map();
    dst.float_count = map.float_count;
    dst.float_dynamic_addressing = map.float_dynamic_addressing;
    static_assert(sizeof(dst.float_bitmap) == sizeof(map.float_bitmap));
    std::memcpy(dst.float_bitmap, map.float_bitmap, sizeof(dst.float_bitmap));
    static_assert(sizeof(dst.vertex_fetch_bitmap) ==
                  sizeof(map.vertex_fetch_bitmap));
    std::memcpy(dst.vertex_fetch_bitmap, map.vertex_fetch_bitmap,
                sizeof(dst.vertex_fetch_bitmap));
    dst.writes_depth = shader->writes_depth();
    if (auto* spirv_shader = dynamic_cast<SpirvShader*>(shader)) {
      for (const auto& binding :
           spirv_shader->GetTextureBindingsAfterTranslation()) {
        dst.texture_bindings.push_back(
            {uint8_t(binding.fetch_constant), uint8_t(binding.dimension),
             bool(binding.is_signed)});
      }
      for (const auto& binding :
           spirv_shader->GetSamplerBindingsAfterTranslation()) {
        dst.sampler_bindings.push_back(
            {uint8_t(binding.fetch_constant), uint8_t(binding.mag_filter),
             uint8_t(binding.min_filter), uint8_t(binding.mip_filter),
             uint8_t(binding.aniso_filter)});
      }
    }
  }
}

void PrintTextureBindings(const char* stage, Shader* shader,
                          const RegisterFile& register_file) {
  if (!shader || !shader->is_ucode_analyzed()) {
    return;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader->constant_register_map();
  std::fprintf(stderr,
               "%s float constants: count=%u dynamic=%u bitmap="
               "%016llx,%016llx,%016llx,%016llx\n",
               stage, constant_map.float_count,
               uint32_t(constant_map.float_dynamic_addressing),
               static_cast<unsigned long long>(constant_map.float_bitmap[0]),
               static_cast<unsigned long long>(constant_map.float_bitmap[1]),
               static_cast<unsigned long long>(constant_map.float_bitmap[2]),
               static_cast<unsigned long long>(constant_map.float_bitmap[3]));
  const uint32_t float_register_base =
      stage[0] == 'V' ? uint32_t(XE_GPU_REG_SHADER_CONSTANT_000_X)
                      : uint32_t(XE_GPU_REG_SHADER_CONSTANT_256_X);
  uint32_t constants_printed = 0;
  for (uint32_t group = 0; group < 4 && constants_printed < 16; ++group) {
    uint64_t remaining = constant_map.float_bitmap[group];
    while (remaining && constants_printed < 16) {
      const uint32_t bit = uint32_t(__builtin_ctzll(remaining));
      remaining &= remaining - 1;
      const uint32_t guest_index = (group << 6) + bit;
      const uint32_t reg = float_register_base + guest_index * 4;
      std::fprintf(stderr,
                   "%s c%u = %.9g, %.9g, %.9g, %.9g\n", stage,
                   guest_index, register_file.Get<float>(reg + 0),
                   register_file.Get<float>(reg + 1),
                   register_file.Get<float>(reg + 2),
                   register_file.Get<float>(reg + 3));
      ++constants_printed;
    }
  }
  for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
    const uint32_t fetch_index = binding.fetch_constant;
    const xenos::xe_gpu_texture_fetch_t fetch =
        register_file.GetTextureFetch(fetch_index);
    std::fprintf(stderr,
                 "%s texture binding %zu: fetch=%u op_dim=%u raw="
                 "%08x,%08x,%08x,%08x,%08x,%08x type=%u data_dim=%u "
                 "format=%u tiled=%u pitch=%u base=%05x size2d=%ux%u "
                 "mips=%u..%u mipbase=%05x swizzle=%03x endian=%u\n",
                 stage, binding.binding_index, fetch_index,
                 uint32_t(binding.fetch_instr.dimension), fetch.dword_0,
                 fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                 fetch.dword_5, uint32_t(fetch.type), uint32_t(fetch.dimension),
                 uint32_t(fetch.format), uint32_t(fetch.tiled),
                 uint32_t(fetch.pitch) << 5, uint32_t(fetch.base_address),
                 uint32_t(fetch.size_2d.width) + 1,
                 uint32_t(fetch.size_2d.height) + 1,
                 uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
                 uint32_t(fetch.mip_address), uint32_t(fetch.swizzle),
                 uint32_t(fetch.endianness));
  }
  if (auto* spirv_shader = dynamic_cast<SpirvShader*>(shader)) {
    size_t binding_index = 0;
    for (const auto& binding :
         spirv_shader->GetSamplerBindingsAfterTranslation()) {
      const xenos::xe_gpu_texture_fetch_t fetch =
          register_file.GetTextureFetch(binding.fetch_constant);
      std::fprintf(stderr,
                   "%s sampler binding %zu: fetch=%u shader_filters=%u/%u/%u "
                   "aniso=%u fetch_filters=%u/%u/%u aniso=%u clamp=%u/%u/%u "
                   "border=%u\n",
                   stage, binding_index++, binding.fetch_constant,
                   uint32_t(binding.mag_filter), uint32_t(binding.min_filter),
                   uint32_t(binding.mip_filter), uint32_t(binding.aniso_filter),
                   uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
                   uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter),
                   uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y),
                   uint32_t(fetch.clamp_z), uint32_t(fetch.border_color));
    }
  }
}

void DumpShaderTranslations(const char* stage, Shader* shader,
                            const char* output_dir) {
  if (!shader || !output_dir || !output_dir[0]) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec) {
    std::fprintf(stderr, "Cannot create shader dump directory '%s': %s\n",
                 output_dir, ec.message().c_str());
    return;
  }
  for (const auto& [modification, translation] : shader->translations()) {
    if (!translation || !translation->is_valid()) {
      continue;
    }
    const auto& binary = translation->translated_binary();
    char filename[128];
    std::snprintf(filename, sizeof(filename), "%016llx.%s.%016llx.spv",
                  static_cast<unsigned long long>(shader->ucode_data_hash()),
                  stage, static_cast<unsigned long long>(modification));
    const std::filesystem::path path =
        std::filesystem::path(output_dir) / filename;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file || (!binary.empty() &&
                  std::fwrite(binary.data(), binary.size(), 1, file) != 1)) {
      if (file) {
        std::fclose(file);
      }
      std::fprintf(stderr, "Cannot write emulator shader '%s'\n",
                   path.c_str());
      continue;
    }
    std::fclose(file);
    std::fprintf(stderr,
                 "Emulator %s shader translation %016llx dumped to %s "
                 "(%zu bytes)\n",
                 stage, static_cast<unsigned long long>(modification),
                 path.c_str(), binary.size());
  }
}

int ScanCommands(TracePlayer& player, GraphicsSystem* graphics, const char* out_path) {
  FILE* out = out_path ? std::fopen(out_path, "w") : stdout;
  if (!out) {
    std::fprintf(stderr, "Cannot open scan output '%s'\n", out_path);
    return 7;
  }
  std::fprintf(out,
               "idx\ttype\tprim\tnum_idx\tsrc_sel\tedram_mode\tvs_hash\tvs_tex\t"
               "ps_hash\tps_tex\tblend0\tcolor_mask\tcolorcontrol\tdepthcontrol\t"
               "rt0_info\trt1_info\trt2_info\trt3_info\tdepth_info\t"
               "surface_info\tvte_cntl\tsu_mode\tscissor_tl\tscissor_br\t"
               "copy_control\tcopy_dest_base\tcopy_dest_pitch\tcopy_dest_info\t"
               "copy_v0x\tcopy_v0y\tcopy_v1x\tcopy_v1y\tcopy_v2x\tcopy_v2y\n");
  const TraceReader::Frame* frame = player.current_frame();
  const int command_count = int(frame->commands.size());
  const char* rect_clear_oracle_value =
      std::getenv("AC6_NR2_RECT_CLEAR_LOWERING");
  const bool rect_clear_oracle =
      rect_clear_oracle_value && std::strcmp(rect_clear_oracle_value, "1") == 0;
  for (int i = 0; i < command_count; ++i) {
    if (frame->commands[i].type == TraceReader::Frame::Command::Type::kSwap) {
      // Seeking into a swap command breaks playback without signaling the
      // playback event (kBreakOnSwap early return) - stop the scan here.
      std::fprintf(out, "%d\tswap\n", i);
      continue;
    }
    if (rect_clear_oracle) {
      std::fprintf(stderr,
                   "AC6_RECT_CLEAR_ORACLE_COMMAND phase=begin command=%d\n", i);
      std::fflush(stderr);
    }
    player.SeekCommand(i);
    player.WaitOnPlayback();
    if (rect_clear_oracle) {
      std::fprintf(stderr,
                   "AC6_RECT_CLEAR_ORACLE_COMMAND phase=end command=%d\n", i);
      std::fflush(stderr);
    }
    const RegisterFile& regs = *graphics->register_file();
    auto vgt = regs.Get<reg::VGT_DRAW_INITIATOR>();
    Shader* vs = graphics->command_processor()->active_vertex_shader();
    Shader* ps = graphics->command_processor()->active_pixel_shader();
    const uint64_t vs_hash = vs ? vs->ucode_data_hash() : 0;
    const uint64_t ps_hash = ps ? ps->ucode_data_hash() : 0;
    const int vs_tex =
        (vs && vs->is_ucode_analyzed()) ? int(vs->texture_bindings().size()) : -1;
    const int ps_tex =
        (ps && ps->is_ucode_analyzed()) ? int(ps->texture_bindings().size()) : -1;
    float copy_vertices[6] = {};
    if ((regs[XE_GPU_REG_RB_MODECONTROL] & 0x7u) == 6) {
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
      if (fetch.type == xenos::FetchConstantType::kVertex && fetch.size >= 6) {
        const float* guest_vertices = reinterpret_cast<const float*>(
            graphics->memory()->physical_membase() + fetch.address * sizeof(uint32_t));
        for (uint32_t vertex_component = 0; vertex_component < 6;
             ++vertex_component) {
          copy_vertices[vertex_component] =
              xenos::GpuSwap(guest_vertices[vertex_component], fetch.endian);
        }
      }
    }
    std::fprintf(out,
                 "%d\tdraw\t%u\t%u\t%u\t%u\t%016llx\t%d\t%016llx\t%d\t"
                 "%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t"
                 "%08x\t%08x\t%08x\t%08x\t"
                 "%08x\t%08x\t%08x\t%08x\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\n",
                 i, uint32_t(vgt.prim_type), uint32_t(vgt.num_indices),
                 uint32_t(vgt.source_select),
                 regs[XE_GPU_REG_RB_MODECONTROL] & 0x7u,
                 (unsigned long long)vs_hash, vs_tex, (unsigned long long)ps_hash,
                 ps_tex, regs[XE_GPU_REG_RB_BLENDCONTROL0],
                 regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_COLORCONTROL],
                 regs[XE_GPU_REG_RB_DEPTHCONTROL],
                 regs[reg::RB_COLOR_INFO::rt_register_indices[0]],
                 regs[reg::RB_COLOR_INFO::rt_register_indices[1]],
                 regs[reg::RB_COLOR_INFO::rt_register_indices[2]],
                 regs[reg::RB_COLOR_INFO::rt_register_indices[3]],
                 regs[XE_GPU_REG_RB_DEPTH_INFO],
                 regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                 regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
                 regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                 regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
                 regs[XE_GPU_REG_RB_COPY_CONTROL],
                 regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                 regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
                 regs[XE_GPU_REG_RB_COPY_DEST_INFO], copy_vertices[0],
                 copy_vertices[1], copy_vertices[2], copy_vertices[3],
                 copy_vertices[4], copy_vertices[5]);
  }
  if (out != stdout) {
    std::fclose(out);
    std::fprintf(stderr, "Scan of %d commands written to %s\n", command_count,
                 out_path);
  }
  return 0;
}

struct FrameThroughputTiming {
  double hydration_seconds = 0.0;
  double replay_seconds = 0.0;
  double gpu_wait_seconds = 0.0;
  double total_seconds = 0.0;
  std::vector<
      rex::graphics::vulkan::VulkanCommandProcessor::Ac6GpuPassProfileBucket>
      gpu_pass_profile_buckets;
};

int RunFrameThroughput(TracePlayer& player, GraphicsSystem* graphics,
                       int warmup_frames, int measured_frames) {
  const TraceReader::Frame* frame = player.current_frame();
  const int command_count = int(frame->commands.size());
  int final_swap_index = -1;
  for (int command_index = 0; command_index < command_count; ++command_index) {
    const auto command_type = frame->commands[command_index].type;
    if (command_type == TraceReader::Frame::Command::Type::kSwap) {
      final_swap_index = command_index;
    }
  }
  if (final_swap_index < 0) {
    std::fprintf(stderr,
                 "Frame-throughput trace has no recorded final swap\n");
    return 6;
  }
  int final_draw_index = -1;
  for (int command_index = final_swap_index - 1; command_index >= 0;
       --command_index) {
    if (frame->commands[command_index].type ==
        TraceReader::Frame::Command::Type::kDraw) {
      final_draw_index = command_index;
      break;
    }
  }
  if (final_draw_index < 0) {
    std::fprintf(stderr,
                 "Frame-throughput trace has no draw/resolve before its final "
                 "swap (draw=%d swap=%d)\n",
                 final_draw_index, final_swap_index);
    return 6;
  }
  const auto& first_command = frame->commands.front();
  const auto& final_swap = frame->commands[final_swap_index];
  if (first_command.head_ptr <= frame->start_ptr ||
      final_swap.end_ptr <= first_command.head_ptr) {
    std::fprintf(stderr, "Invalid frame hydration or replay byte span\n");
    return 6;
  }
  const size_t hydration_size =
      size_t(first_command.head_ptr - frame->start_ptr);
  const size_t replay_size =
      size_t(final_swap.end_ptr - first_command.head_ptr);

  auto* vulkan_command_processor =
      static_cast<rex::graphics::vulkan::VulkanCommandProcessor*>(
          graphics->command_processor());
  auto* vulkan_device = vulkan_command_processor->GetVulkanDevice();
  if (!vulkan_device || vulkan_device->device() == VK_NULL_HANDLE) {
    std::fprintf(stderr,
                 "Frame-throughput cannot access the Vulkan device\n");
    return 8;
  }
  const auto& vulkan_instance_functions =
      vulkan_device->vulkan_instance()->functions();
  const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      vulkan_instance_functions.vkGetDeviceProcAddr(
          vulkan_device->device(), "vkDeviceWaitIdle"));
  if (!device_wait_idle) {
    std::fprintf(stderr,
                 "Frame-throughput cannot load vkDeviceWaitIdle\n");
    return 8;
  }
  const bool shared_authority = ac6::nr2::SharedAuthorityEnabled();

  using Clock = std::chrono::steady_clock;
  const auto replay_frame = [&](const char* phase, int phase_index,
                                int phase_count,
                                FrameThroughputTiming& timing_out) {
    // Restore the self-contained captured frame-start state outside the
    // renderer timer. This includes the EDRAM snapshot and the first command's
    // memory/register setup, which exist only to make trace replay reproducible
    // and aren't repeated by the live game every frame.
    const auto hydration_start = Clock::now();
    player.ReplayRange(frame->start_ptr, hydration_size, false);
    player.WaitOnPlayback();
    const VkResult hydration_wait_result =
        device_wait_idle(vulkan_device->device());
    const auto hydration_complete = Clock::now();
    timing_out.hydration_seconds =
        std::chrono::duration<double>(hydration_complete - hydration_start)
            .count();
    if (hydration_wait_result != VK_SUCCESS) {
      std::fprintf(stderr,
                   "frame-throughput %s %d/%d: hydration "
                   "vkDeviceWaitIdle failed with VkResult %d\n",
                   phase, phase_index, phase_count,
                   int(hydration_wait_result));
      return false;
    }

    const auto frame_start = Clock::now();
    // Keep skip_memory_reads false in the timed body: later commands may carry
    // multiple versions of dynamic guest data, and replaying only the final
    // version left by the previous iteration is not equivalent. ReplayRange
    // also uses kUntilEnd, unlike SeekCommand on a swap, so the final resolve
    // and XE_SWAP packet both execute and signal completion.
    player.ReplayRange(first_command.head_ptr, replay_size, false);
    player.WaitOnPlayback();
    const auto replay_complete = Clock::now();

    // Wait exactly once after this frame. WaitOnPlayback only means the
    // command-processor thread finished recording/submitting the trace; this
    // explicit device wait makes the timing include the resulting GPU work.
    const VkResult wait_result = device_wait_idle(vulkan_device->device());
    const auto gpu_complete = Clock::now();
    timing_out.replay_seconds =
        std::chrono::duration<double>(replay_complete - frame_start).count();
    timing_out.gpu_wait_seconds =
        std::chrono::duration<double>(gpu_complete - replay_complete).count();
    timing_out.total_seconds =
        std::chrono::duration<double>(gpu_complete - frame_start).count();
    if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr,
                   "frame-throughput %s %d/%d: vkDeviceWaitIdle failed "
                   "with VkResult %d\n",
                   phase, phase_index, phase_count, int(wait_result));
      return false;
    }
    vulkan_command_processor->ReadAc6GpuPassProfile(
        timing_out.gpu_pass_profile_buckets);
    std::fprintf(stderr,
                 "frame-throughput %s %d/%d hydrate=%.6fs replay=%.6fs "
                 "gpu_wait=%.6fs total=%.6fs fps=%.3f\n",
                 phase, phase_index, phase_count,
                 timing_out.hydration_seconds, timing_out.replay_seconds,
                 timing_out.gpu_wait_seconds, timing_out.total_seconds,
                 timing_out.total_seconds > 0.0
                     ? 1.0 / timing_out.total_seconds
                     : 0.0);
    return true;
  };

  std::fprintf(stderr,
               "frame-throughput setup: commands=%d hydrate_bytes=%zu "
               "timed_bytes=%zu final_draw=%d "
               "final_swap=%d warmups=%d measured=%d "
               "readback_resolve=none dynamic_rendering=true "
               "shared_authority=%s\n",
               command_count, hydration_size, replay_size, final_draw_index,
               final_swap_index, warmup_frames, measured_frames,
               shared_authority ? "true" : "false");
  for (int warmup_index = 0; warmup_index < warmup_frames; ++warmup_index) {
    FrameThroughputTiming warmup_timing;
    if (!replay_frame("warmup", warmup_index + 1, warmup_frames,
                      warmup_timing)) {
      return 8;
    }
    if (warmup_index == 0) {
      const uint32_t terminal_edram_mode =
          (*graphics->register_file())[XE_GPU_REG_RB_MODECONTROL] & 0x7u;
      if (terminal_edram_mode != uint32_t(xenos::EdramMode::kCopy)) {
        std::fprintf(
            stderr,
            "Frame-throughput final draw %d is not a resolve/copy after "
            "full replay (RB_MODECONTROL mode %u)\n",
            final_draw_index, terminal_edram_mode);
        return 6;
      }
      std::fprintf(stderr,
                   "frame-throughput terminal state: final draw %d is "
                   "resolve/copy mode %u and swap %d executed\n",
                   final_draw_index, terminal_edram_mode, final_swap_index);
    }
  }

  std::vector<FrameThroughputTiming> timings(
      static_cast<size_t>(measured_frames));
  for (int measured_index = 0; measured_index < measured_frames;
       ++measured_index) {
    if (!replay_frame("measured", measured_index + 1, measured_frames,
                      timings[size_t(measured_index)])) {
      return 8;
    }
  }

  std::vector<double> total_seconds;
  total_seconds.reserve(timings.size());
  double replay_sum = 0.0;
  double hydration_sum = 0.0;
  double gpu_wait_sum = 0.0;
  double total_sum = 0.0;
  for (const FrameThroughputTiming& timing : timings) {
    total_seconds.push_back(timing.total_seconds);
    hydration_sum += timing.hydration_seconds;
    replay_sum += timing.replay_seconds;
    gpu_wait_sum += timing.gpu_wait_seconds;
    total_sum += timing.total_seconds;
  }
  std::sort(total_seconds.begin(), total_seconds.end());
  const size_t frame_count = total_seconds.size();
  const double median_seconds =
      (frame_count & 1) != 0
          ? total_seconds[frame_count / 2]
          : (total_seconds[frame_count / 2 - 1] +
             total_seconds[frame_count / 2]) *
                0.5;
  const size_t p95_index = (frame_count * 95 + 99) / 100 - 1;
  const double mean_seconds = total_sum / double(frame_count);
  const double p95_seconds = total_seconds[p95_index];
  const double aggregate_fps = double(frame_count) / total_sum;
  const double p95_fps = p95_seconds > 0.0 ? 1.0 / p95_seconds : 0.0;
  std::fprintf(
      stderr,
      "frame-throughput-summary: warmups=%d measured=%zu "
      "measured_gpu_waits=%zu warmup_gpu_waits=%d shared_authority=%s "
      "total=%.6fs "
      "hydration_mean=%.6fs replay_mean=%.6fs "
      "gpu_wait_mean=%.6fs frame_mean=%.6fs frame_median=%.6fs "
      "frame_p95=%.6fs frame_min=%.6fs frame_max=%.6fs fps=%.3f "
      "p95_fps=%.3f target_60=%s target_30=%s\n",
      warmup_frames, frame_count, frame_count, warmup_frames,
      shared_authority ? "true" : "false", total_sum,
      hydration_sum / double(frame_count), replay_sum / double(frame_count),
      gpu_wait_sum / double(frame_count),
      mean_seconds, median_seconds, p95_seconds, total_seconds.front(),
      total_seconds.back(), aggregate_fps, p95_fps,
      p95_seconds <= 1.0 / 60.0 ? "pass" : "fail",
      p95_seconds <= 1.0 / 30.0 ? "pass" : "fail");

  const auto& profile_layout = timings.front().gpu_pass_profile_buckets;
  if (!profile_layout.empty()) {
    bool profile_consistent = true;
    for (const FrameThroughputTiming& timing : timings) {
      if (timing.gpu_pass_profile_buckets.size() != profile_layout.size()) {
        profile_consistent = false;
        break;
      }
      for (size_t bucket_index = 0; bucket_index < profile_layout.size();
           ++bucket_index) {
        const auto& expected = profile_layout[bucket_index];
        const auto& actual = timing.gpu_pass_profile_buckets[bucket_index];
        if (actual.first_command != expected.first_command ||
            actual.last_command != expected.last_command ||
            actual.draw_count != expected.draw_count ||
            actual.copy_count != expected.copy_count ||
            actual.includes_swap != expected.includes_swap) {
          profile_consistent = false;
          break;
        }
      }
      if (!profile_consistent) {
        break;
      }
    }

    if (!profile_consistent) {
      std::fprintf(
          stderr,
          "gpu-pass-profile-summary: inconsistent command buckets across "
          "measured frames; results suppressed\n");
    } else {
      std::vector<double> bucket_sums(profile_layout.size(), 0.0);
      std::vector<double> bucket_mins(profile_layout.size(), 0.0);
      std::vector<double> bucket_maxs(profile_layout.size(), 0.0);
      std::vector<double> bucket_cpu_sums(profile_layout.size(), 0.0);
      std::vector<double> bucket_cpu_mins(profile_layout.size(), 0.0);
      std::vector<double> bucket_cpu_maxs(profile_layout.size(), 0.0);
      using VulkanCommandProcessor =
          rex::graphics::vulkan::VulkanCommandProcessor;
      std::vector<std::array<
          double, VulkanCommandProcessor::kAc6Nr2HotpathPhaseCount>>
          bucket_cpu_phase_sums(profile_layout.size());
      for (size_t bucket_index = 0; bucket_index < profile_layout.size();
           ++bucket_index) {
        bucket_mins[bucket_index] =
            timings.front().gpu_pass_profile_buckets[bucket_index]
                .gpu_milliseconds;
        bucket_maxs[bucket_index] = bucket_mins[bucket_index];
        bucket_cpu_mins[bucket_index] =
            timings.front().gpu_pass_profile_buckets[bucket_index]
                .cpu_issue_draw_milliseconds;
        bucket_cpu_maxs[bucket_index] = bucket_cpu_mins[bucket_index];
      }
      for (const FrameThroughputTiming& timing : timings) {
        for (size_t bucket_index = 0; bucket_index < profile_layout.size();
             ++bucket_index) {
          const double milliseconds =
              timing.gpu_pass_profile_buckets[bucket_index]
                  .gpu_milliseconds;
          bucket_sums[bucket_index] += milliseconds;
          bucket_mins[bucket_index] =
              std::min(bucket_mins[bucket_index], milliseconds);
          bucket_maxs[bucket_index] =
              std::max(bucket_maxs[bucket_index], milliseconds);
          const double cpu_milliseconds =
              timing.gpu_pass_profile_buckets[bucket_index]
                  .cpu_issue_draw_milliseconds;
          bucket_cpu_sums[bucket_index] += cpu_milliseconds;
          bucket_cpu_mins[bucket_index] =
              std::min(bucket_cpu_mins[bucket_index], cpu_milliseconds);
          bucket_cpu_maxs[bucket_index] =
              std::max(bucket_cpu_maxs[bucket_index], cpu_milliseconds);
          for (size_t phase = 0;
               phase < bucket_cpu_phase_sums[bucket_index].size(); ++phase) {
            bucket_cpu_phase_sums[bucket_index][phase] +=
                timing.gpu_pass_profile_buckets[bucket_index]
                    .cpu_issue_draw_phase_milliseconds[phase];
          }
        }
      }
      double gpu_span_mean_ms = 0.0;
      double issue_draw_cpu_mean_ms = 0.0;
      for (double sum : bucket_sums) {
        gpu_span_mean_ms += sum / double(frame_count);
      }
      for (double sum : bucket_cpu_sums) {
        issue_draw_cpu_mean_ms += sum / double(frame_count);
      }
      std::fprintf(stderr,
                   "gpu-pass-profile-summary: frames=%zu buckets=%zu "
                   "gpu_span_mean_ms=%.6f issue_draw_cpu_mean_ms=%.6f\n",
                   frame_count, profile_layout.size(), gpu_span_mean_ms,
                   issue_draw_cpu_mean_ms);
      for (size_t bucket_index = 0; bucket_index < profile_layout.size();
           ++bucket_index) {
        const auto& bucket = profile_layout[bucket_index];
        const double bucket_mean_ms =
            bucket_sums[bucket_index] / double(frame_count);
        const double bucket_cpu_mean_ms =
            bucket_cpu_sums[bucket_index] / double(frame_count);
        std::fprintf(
            stderr,
            "gpu-pass-profile-bucket: index=%zu first=%u last=%u draws=%u "
            "copies=%u swap=%s mean_ms=%.6f min_ms=%.6f max_ms=%.6f "
            "span_percent=%.3f cpu_mean_ms=%.6f cpu_min_ms=%.6f "
            "cpu_max_ms=%.6f\n",
            bucket_index, bucket.first_command, bucket.last_command,
            bucket.draw_count, bucket.copy_count,
            bucket.includes_swap ? "true" : "false", bucket_mean_ms,
            bucket_mins[bucket_index], bucket_maxs[bucket_index],
            gpu_span_mean_ms > 0.0
                ? bucket_mean_ms * 100.0 / gpu_span_mean_ms
                : 0.0,
            bucket_cpu_mean_ms, bucket_cpu_mins[bucket_index],
            bucket_cpu_maxs[bucket_index]);
        // Phase rows are intentionally sparse. The per-command interval=1
        // profile already emits over a thousand bucket rows, and the expensive
        // CPU commands are the only ones this diagnostic needs to classify.
        if (bucket_cpu_mean_ms >= 0.25) {
          const auto& phase_sums = bucket_cpu_phase_sums[bucket_index];
          std::array<double,
                     VulkanCommandProcessor::kAc6Nr2HotpathPhaseCount>
              phase_means = {};
          double phase_accounted_mean_ms = 0.0;
          for (size_t phase = 0; phase < phase_means.size(); ++phase) {
            phase_means[phase] = phase_sums[phase] / double(frame_count);
            phase_accounted_mean_ms += phase_means[phase];
          }
          const double phase_other_mean_ms =
              std::max(0.0,
                       bucket_cpu_mean_ms - phase_accounted_mean_ms);
          std::fprintf(
              stderr,
              "gpu-pass-profile-cpu-phases: schema=2 index=%zu "
              "shader_analysis_ms=%.6f submission_begin_ms=%.6f "
              "primitive_process_ms=%.6f shader_modification_ms=%.6f "
              "shader_translate_ms=%.6f sampler_acquisition_ms=%.6f "
              "texture_request_ms=%.6f render_target_update_ms=%.6f "
              "configure_pipeline_ms=%.6f native_prepare_ms=%.6f "
              "state_binding_ms=%.6f update_bindings_ms=%.6f "
              "memory_residency_ms=%.6f draw_record_ms=%.6f "
              "post_draw_ms=%.6f other_ms=%.6f\n",
              bucket_index, phase_means[0], phase_means[1],
              phase_means[2], phase_means[3], phase_means[4],
              phase_means[5], phase_means[6], phase_means[7],
              phase_means[8], phase_means[9], phase_means[10],
              phase_means[11], phase_means[12], phase_means[13],
              phase_means[14], phase_other_mean_ms);
        }
      }

      // The deterministic Gracemeria trace has two 332-draw tiled world
      // ranges separated and followed by four copy packets. When profiling at
      // interval 1 (or with another layout that happens to respect all of
      // these boundaries), print exact per-frame sums for the ranges under
      // investigation. Suppress a range rather than attributing a straddling
      // bucket fraction heuristically.
      struct NamedProfileRange {
        const char* name;
        uint32_t first;
        uint32_t last;
      };
      static constexpr NamedProfileRange kNamedProfileRanges[] = {
          {"world_a", 125, 456},
          {"between_copies", 457, 460},
          {"world_b", 461, 792},
          {"after_copies", 793, 796},
      };
      std::vector<bool> bucket_is_named(profile_layout.size(), false);
      for (const NamedProfileRange& named_range : kNamedProfileRanges) {
        bool exact_cover = true;
        uint32_t covered_commands = 0;
        std::vector<bool> range_buckets(profile_layout.size(), false);
        for (size_t bucket_index = 0; bucket_index < profile_layout.size();
             ++bucket_index) {
          const auto& bucket = profile_layout[bucket_index];
          const bool overlaps =
              bucket.last_command >= named_range.first &&
              bucket.first_command <= named_range.last;
          if (!overlaps) {
            continue;
          }
          if (bucket.first_command < named_range.first ||
              bucket.last_command > named_range.last ||
              bucket.includes_swap) {
            exact_cover = false;
            break;
          }
          range_buckets[bucket_index] = true;
          covered_commands +=
              bucket.last_command - bucket.first_command + 1;
        }
        if (!exact_cover ||
            covered_commands != named_range.last - named_range.first + 1) {
          std::fprintf(
              stderr,
              "gpu-pass-profile-range: name=%s first=%u last=%u "
              "status=straddled (use interval=1)\n",
              named_range.name, named_range.first, named_range.last);
          continue;
        }
        std::vector<double> frame_range_ms(frame_count, 0.0);
        std::vector<double> frame_range_cpu_ms(frame_count, 0.0);
        for (size_t bucket_index = 0; bucket_index < profile_layout.size();
             ++bucket_index) {
          if (!range_buckets[bucket_index]) {
            continue;
          }
          bucket_is_named[bucket_index] = true;
          for (size_t frame_index = 0; frame_index < frame_count;
               ++frame_index) {
            frame_range_ms[frame_index] +=
                timings[frame_index]
                    .gpu_pass_profile_buckets[bucket_index]
                    .gpu_milliseconds;
            frame_range_cpu_ms[frame_index] +=
                timings[frame_index]
                    .gpu_pass_profile_buckets[bucket_index]
                    .cpu_issue_draw_milliseconds;
          }
        }
        const double range_sum_ms =
            std::accumulate(frame_range_ms.begin(), frame_range_ms.end(),
                            0.0);
        const auto range_minmax = std::minmax_element(
            frame_range_ms.begin(), frame_range_ms.end());
        const double range_mean_ms = range_sum_ms / double(frame_count);
        const double range_cpu_sum_ms = std::accumulate(
            frame_range_cpu_ms.begin(), frame_range_cpu_ms.end(), 0.0);
        const auto range_cpu_minmax = std::minmax_element(
            frame_range_cpu_ms.begin(), frame_range_cpu_ms.end());
        const double range_cpu_mean_ms =
            range_cpu_sum_ms / double(frame_count);
        std::fprintf(
            stderr,
            "gpu-pass-profile-range: name=%s first=%u last=%u "
            "mean_ms=%.6f min_ms=%.6f max_ms=%.6f span_percent=%.3f "
            "cpu_mean_ms=%.6f cpu_min_ms=%.6f cpu_max_ms=%.6f\n",
            named_range.name, named_range.first, named_range.last,
            range_mean_ms, *range_minmax.first, *range_minmax.second,
            gpu_span_mean_ms > 0.0
                ? range_mean_ms * 100.0 / gpu_span_mean_ms
                : 0.0,
            range_cpu_mean_ms, *range_cpu_minmax.first,
            *range_cpu_minmax.second);
      }

      // Everything outside the four named ranges, including the presentation
      // tail carried by the final timestamp bucket.
      std::vector<double> frame_remainder_ms(frame_count, 0.0);
      std::vector<double> frame_remainder_cpu_ms(frame_count, 0.0);
      for (size_t bucket_index = 0; bucket_index < profile_layout.size();
           ++bucket_index) {
        if (bucket_is_named[bucket_index]) {
          continue;
        }
        for (size_t frame_index = 0; frame_index < frame_count;
             ++frame_index) {
          frame_remainder_ms[frame_index] +=
              timings[frame_index]
                  .gpu_pass_profile_buckets[bucket_index]
                  .gpu_milliseconds;
          frame_remainder_cpu_ms[frame_index] +=
              timings[frame_index]
                  .gpu_pass_profile_buckets[bucket_index]
                  .cpu_issue_draw_milliseconds;
        }
      }
      const double remainder_sum_ms =
          std::accumulate(frame_remainder_ms.begin(),
                          frame_remainder_ms.end(), 0.0);
      const auto remainder_minmax = std::minmax_element(
          frame_remainder_ms.begin(), frame_remainder_ms.end());
      const double remainder_mean_ms = remainder_sum_ms / double(frame_count);
      const double remainder_cpu_sum_ms = std::accumulate(
          frame_remainder_cpu_ms.begin(), frame_remainder_cpu_ms.end(), 0.0);
      const auto remainder_cpu_minmax = std::minmax_element(
          frame_remainder_cpu_ms.begin(), frame_remainder_cpu_ms.end());
      const double remainder_cpu_mean_ms =
          remainder_cpu_sum_ms / double(frame_count);
      std::fprintf(
          stderr,
          "gpu-pass-profile-range: name=remainder mean_ms=%.6f "
          "min_ms=%.6f max_ms=%.6f span_percent=%.3f "
          "cpu_mean_ms=%.6f cpu_min_ms=%.6f cpu_max_ms=%.6f\n",
          remainder_mean_ms, *remainder_minmax.first,
          *remainder_minmax.second,
          gpu_span_mean_ms > 0.0
              ? remainder_mean_ms * 100.0 / gpu_span_mean_ms
              : 0.0,
          remainder_cpu_mean_ms, *remainder_cpu_minmax.first,
          *remainder_cpu_minmax.second);
    }
  }
  return 0;
}

int RunResolveDump(TracePlayer& player, GraphicsSystem* graphics, int command_index,
                   const char* out_path, uint32_t addr_override,
                   uint32_t bytes_override) {
  const TraceReader::Frame* frame = player.current_frame();
  const int command_count = int(frame->commands.size());
  if (command_index < 0 || command_index >= command_count ||
      frame->commands[command_index].type != TraceReader::Frame::Command::Type::kDraw) {
    std::fprintf(stderr, "Command %d is not a draw command\n", command_index);
    return 5;
  }
  int replay_count = 1;
  if (const char* replay_count_string =
          std::getenv("AC6_NR2_RESOLVE_REPEATS")) {
    char* replay_count_end = nullptr;
    const long parsed_replay_count =
        std::strtol(replay_count_string, &replay_count_end, 10);
    if (!replay_count_end || *replay_count_end != '\0' ||
        parsed_replay_count < 1 || parsed_replay_count > 100) {
      std::fprintf(stderr,
                   "AC6_NR2_RESOLVE_REPEATS must be an integer in [1, 100]\n");
      return 6;
    }
    replay_count = int(parsed_replay_count);
  }
  if (replay_count > 1 &&
      std::getenv("AC6_NR2_INVALIDATE_TEXTURES_BEFORE_COMMAND")) {
    std::fprintf(stderr,
                 "Repeated resolve replay cannot be combined with the "
                 "single-draw texture invalidation diagnostics\n");
    return 6;
  }
  bool replayed_diagnostic_range = false;
  if (const char* invalidate_before =
          std::getenv("AC6_NR2_INVALIDATE_TEXTURES_BEFORE_COMMAND")) {
    const int invalidate_command =
        int(std::strtol(invalidate_before, nullptr, 10));
    if (invalidate_command > 0 && invalidate_command <= command_index) {
      player.SeekCommand(invalidate_command - 1);
      player.WaitOnPlayback();
      if (const char* exposure_millionths =
              std::getenv("AC6_NR2_REFERENCE_EXPOSURE_MILLIONTHS")) {
        const auto& draw_command = frame->commands[invalidate_command];
        // Replay the draw's captured memory/register setup, stopping before
        // its actual packet. This lets the override happen after the trace's
        // memory-read records instead of being overwritten by them.
        player.ReplayRange(
            draw_command.start_ptr,
            size_t(draw_command.head_ptr - draw_command.start_ptr), false);
        player.WaitOnPlayback();
        const float exposure =
            float(std::strtol(exposure_millionths, nullptr, 10)) /
            1000000.0f;
        uint32_t exposure_bits;
        std::memcpy(&exposure_bits, &exposure, sizeof(exposure_bits));
        // Relocate draw 1643 fetch 19 to a scratch physical page so the
        // texture-cache key must change (marking the original page dirty may
        // legitimately retain a resolve-backed GPU alias). It is a tiled 1x1
        // k32_float texture with k8in32 endianness; tiled (0,0) is offset zero.
        constexpr uint32_t kExposureScratchPage = 0x1FF00u;
        constexpr uint32_t kExposureScratchAddress =
            kExposureScratchPage << 12;
        const uint32_t fetch19_word1 =
            uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) + 19u * 6u + 1u;
        const uint32_t relocated_fetch19_word1 =
            ((*graphics->register_file())[fetch19_word1] & 0xFFFu) |
            (kExposureScratchPage << 12);
        graphics->command_processor()->RestoreRegisters(
            fetch19_word1, &relocated_fetch19_word1, 1, true);
        if (const char* rt_base_string =
                std::getenv("AC6_NR2_REFERENCE_RT_BASE")) {
          const uint32_t rt_base =
              uint32_t(std::strtoul(rt_base_string, nullptr, 0)) & 0xFFFu;
          const uint32_t rt0_register =
              reg::RB_COLOR_INFO::rt_register_indices[0];
          const uint32_t relocated_rt0 =
              ((*graphics->register_file())[rt0_register] & ~0xFFFu) |
              rt_base;
          graphics->command_processor()->RestoreRegisters(
              rt0_register, &relocated_rt0, 1, true);
          std::fprintf(stderr,
                       "Relocated reference draw-1643 RT0 to EDRAM base %03x\n",
                       rt_base);
        }
        exposure_bits = __builtin_bswap32(exposure_bits);
        std::memcpy(graphics->memory()->physical_membase() +
                        kExposureScratchAddress,
                    &exposure_bits, sizeof(exposure_bits));
        graphics->command_processor()->TracePlaybackWroteMemory(
            kExposureScratchAddress, sizeof(exposure_bits));
        std::fprintf(stderr,
                     "Overrode reference draw-1643 exposure texel with %.9g\n",
                     exposure);
        // Execute only the draw packet, then the following commands through
        // the requested resolve. Existing render-target cache contents stay
        // intact, while texture 19 is marked dirty by the write above.
        player.ReplayRange(draw_command.head_ptr,
                           size_t(draw_command.end_ptr -
                                  draw_command.head_ptr),
                           true);
        player.WaitOnPlayback();
        const auto& resolve_command = frame->commands[command_index];
        if (const char* rt_base_string =
                std::getenv("AC6_NR2_REFERENCE_RT_BASE")) {
          // Let the resolve's captured setup restore its registers first, then
          // redirect the copy source immediately before executing the packet.
          // Otherwise that setup replaces the relocated draw target with the
          // original target and makes the diagnostic resolve vacuous.
          player.ReplayRange(
              resolve_command.start_ptr,
              size_t(resolve_command.head_ptr - resolve_command.start_ptr),
              false);
          player.WaitOnPlayback();
          const uint32_t rt_base =
              uint32_t(std::strtoul(rt_base_string, nullptr, 0)) & 0xFFFu;
          const uint32_t rt0_register =
              reg::RB_COLOR_INFO::rt_register_indices[0];
          const uint32_t relocated_rt0 =
              ((*graphics->register_file())[rt0_register] & ~0xFFFu) |
              rt_base;
          graphics->command_processor()->RestoreRegisters(
              rt0_register, &relocated_rt0, 1, true);
          std::fprintf(
              stderr,
              "Relocated reference resolve-%d RT0 to EDRAM base %03x\n",
              command_index, rt_base);
          player.ReplayRange(
              resolve_command.head_ptr,
              size_t(resolve_command.end_ptr - resolve_command.head_ptr),
              false);
        } else {
          player.ReplayRange(
              resolve_command.start_ptr,
              size_t(resolve_command.end_ptr - resolve_command.start_ptr),
              false);
        }
        replayed_diagnostic_range = true;
      } else {
        graphics->InvalidateGpuMemory();
        if (std::getenv("AC6_NR2_CLEAR_CACHES_BEFORE_COMMAND")) {
          graphics->ClearCaches();
          std::fprintf(stderr,
                       "Cleared emulator graphics caches before command %d\n",
                       invalidate_command);
        }
        std::fprintf(stderr,
                     "Invalidated emulator GPU memory before command %d\n",
                     invalidate_command);
      }
    }
  }
  if (!replayed_diagnostic_range) {
    for (int replay_index = 0; replay_index < replay_count; ++replay_index) {
      if (replay_index) {
        // Moving to -1 makes the following seek replay from the captured
        // frame start (and restore its memory/register state) while keeping
        // the Vulkan device and both SDK/native pipeline caches alive.
        player.SeekCommand(-1);
      }
      const auto replay_start = std::chrono::steady_clock::now();
      player.SeekCommand(command_index);
      player.WaitOnPlayback();
      const double replay_seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        replay_start)
              .count();
      std::fprintf(stderr, "resolve replay %d/%d seconds=%.6f\n",
                   replay_index + 1, replay_count, replay_seconds);
    }
  } else {
    player.WaitOnPlayback();
  }
  if (std::getenv("AC6_NR2_REFERENCE_EXPOSURE_MILLIONTHS")) {
    uint32_t exposure_bits;
    std::memcpy(&exposure_bits,
                graphics->memory()->physical_membase() + 0x1FF00000u,
                sizeof(exposure_bits));
    exposure_bits = __builtin_bswap32(exposure_bits);
    float exposure;
    std::memcpy(&exposure, &exposure_bits, sizeof(exposure));
    std::fprintf(stderr,
                 "Reference exposure guest texel after replay: %.9g\n",
                 exposure);
    const uint32_t fetch19_word1 =
        uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) + 19u * 6u + 1u;
    std::fprintf(stderr, "Reference fetch19 word1 after replay: %08x\n",
                 (*graphics->register_file())[fetch19_word1]);
    std::fprintf(stderr, "Reference RT0 info after replay: %08x\n",
                 (*graphics->register_file())[
                     reg::RB_COLOR_INFO::rt_register_indices[0]]);
  }
  const RegisterFile& regs = *graphics->register_file();
  const uint32_t copy_control = regs[XE_GPU_REG_RB_COPY_CONTROL];
  const uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFFu;
  const uint32_t dest_pitch_raw = regs[XE_GPU_REG_RB_COPY_DEST_PITCH];
  const uint32_t dest_info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
  const uint32_t dest_pitch = dest_pitch_raw & 0x3FFFu;
  const uint32_t dest_height = (dest_pitch_raw >> 16) & 0x3FFFu;
  const uint32_t addr = addr_override ? addr_override : dest_base;
  const uint32_t bytes =
      bytes_override ? bytes_override : dest_pitch * dest_height * 4;
  std::fprintf(stderr,
               "resolve state at cmd %d: RB_COPY_CONTROL=%08x DEST_BASE=%08x "
               "DEST_PITCH=%u DEST_HEIGHT=%u DEST_INFO=%08x -> dumping %u bytes "
               "at guest 0x%08x\n",
               command_index, copy_control, dest_base, dest_pitch, dest_height,
               dest_info, bytes, addr);
  if (!bytes || bytes > (UINT32_C(1) << 27)) {
    std::fprintf(stderr, "Implausible dump size\n");
    return 6;
  }
  const uint8_t* membase = graphics->memory()->physical_membase();
  FILE* f = std::fopen(out_path, "wb");
  if (!f || std::fwrite(membase + addr, bytes, 1, f) != 1) {
    if (f) std::fclose(f);
    std::fprintf(stderr, "Cannot write '%s'\n", out_path);
    return 7;
  }
  std::fclose(f);
  std::fprintf(stderr, "Wrote %u bytes to %s\n", bytes, out_path);
  return 0;
}

int RunNativeDraw(TracePlayer& player, GraphicsSystem* graphics, int command_index,
                  const char* out_ppm) {
  const TraceReader::Frame* frame = player.current_frame();
  const int command_count = int(frame->commands.size());
  if (command_index < 0 || command_index >= command_count) {
    std::fprintf(stderr, "Command index %d is outside [0, %d)\n", command_index,
                 command_count);
    return 5;
  }
  if (frame->commands[command_index].type !=
      TraceReader::Frame::Command::Type::kDraw) {
    std::fprintf(stderr, "Command %d is not a draw command\n", command_index);
    return 5;
  }
  std::fprintf(stderr, "Replaying frame 0 through command %d of %d\n", command_index,
               command_count);

#if !defined(_WIN32)
  RenderDocApiShim* renderdoc_api = nullptr;
  void* renderdoc_device = nullptr;
  const bool capture_reference_draw =
      std::getenv("AC6_NR2_RENDERDOC_CAPTURE") != nullptr;
  if (capture_reference_draw) {
    renderdoc_api = GetRenderDocApi();
    if (!renderdoc_api) {
      return 8;
    }
    if (command_index > 0) {
      player.SeekCommand(command_index - 1);
      player.WaitOnPlayback();
    }
    const char* capture_path = std::getenv("AC6_NR2_RENDERDOC_PATH");
    if (!capture_path || !capture_path[0]) {
      capture_path = "/tmp/ac6-reference-draw";
    }
    auto set_path = reinterpret_cast<RenderDocSetCapturePath>(
        renderdoc_api->entries[11]);
    auto start_capture = reinterpret_cast<RenderDocStartCapture>(
        renderdoc_api->entries[19]);
    if (!set_path || !start_capture) {
      std::fprintf(stderr, "RenderDoc API function table is incomplete\n");
      return 8;
    }
    auto* vulkan_provider = dynamic_cast<rex::ui::vulkan::VulkanProvider*>(
        graphics->provider());
    if (!vulkan_provider || !vulkan_provider->vulkan_instance()) {
      std::fprintf(stderr,
                   "Unable to obtain the trace replay Vulkan instance for "
                   "RenderDoc\n");
      return 8;
    }
    // RenderDoc's Vulkan device pointer is the dispatch-table pointer stored
    // at the start of VkInstance, not VkInstance or VkDevice themselves.
    const VkInstance vk_instance =
        vulkan_provider->vulkan_instance()->instance();
    renderdoc_device = *reinterpret_cast<void**>(vk_instance);
    set_path(capture_path);
    std::fprintf(stderr, "Starting RenderDoc capture for command %d at %s\n",
                 command_index, capture_path);
    start_capture(renderdoc_device, nullptr);
  }
#endif
  player.SeekCommand(command_index);
  player.WaitOnPlayback();
#if !defined(_WIN32)
  if (renderdoc_api) {
    // The replay batches ordinary draws in a Vulkan command buffer. Execute
    // the following resolve while the capture is open so its synchronous
    // readback closes and submits the batch containing the requested draw.
    int capture_end_command = command_index + 1;
    if (const char* capture_end =
            std::getenv("AC6_NR2_RENDERDOC_END_COMMAND")) {
      capture_end_command = int(std::strtol(capture_end, nullptr, 10));
    }
    if (capture_end_command > command_index &&
        capture_end_command < command_count &&
        frame->commands[capture_end_command].type ==
            TraceReader::Frame::Command::Type::kDraw) {
      player.SeekCommand(capture_end_command);
      player.WaitOnPlayback();
    }
    auto end_capture = reinterpret_cast<RenderDocEndCapture>(
        renderdoc_api->entries[21]);
    if (!end_capture || !end_capture(renderdoc_device, nullptr)) {
      std::fprintf(stderr, "RenderDoc failed to finish the command capture\n");
      return 8;
    }
    std::fprintf(stderr, "RenderDoc command capture complete\n");
    // Restore the register/shader/memory snapshot requested by draw mode.
    player.SeekCommand(command_index);
    player.WaitOnPlayback();
  }
#endif

  ac6::nr2::DrawSnapshot snap;
  snap.capture_index = uint64_t(command_index);
  const RegisterFile* register_file = graphics->register_file();
  snap.regs.assign(register_file->values,
                   register_file->values + RegisterFile::kRegisterCount);
  CommandProcessor* command_processor = graphics->command_processor();
  CopyShaderInfo(command_processor->active_vertex_shader(), snap.vs);
  CopyShaderInfo(command_processor->active_pixel_shader(), snap.ps);
  snap.physical_membase = graphics->memory()->physical_membase();
  snap.physical_size = UINT32_C(1) << 29;  // SharedMemory::kBufferSize (512MB)
  snap.tool_relaxed = true;
  auto vgt = register_file->Get<reg::VGT_DRAW_INITIATOR>();
  snap.record.primitive_type = uint32_t(vgt.prim_type);
  snap.record.count = vgt.num_indices;

  PrintTextureBindings("VS", command_processor->active_vertex_shader(),
                       *register_file);
  PrintTextureBindings("PS", command_processor->active_pixel_shader(),
                       *register_file);
  if (const char* shader_dump_dir =
          std::getenv("AC6_NR2_DUMP_EMULATOR_SPIRV_DIR")) {
    DumpShaderTranslations("v", command_processor->active_vertex_shader(),
                           shader_dump_dir);
    DumpShaderTranslations("p", command_processor->active_pixel_shader(),
                           shader_dump_dir);
  }
  if (const char* constants_path =
          std::getenv("AC6_NR2_DUMP_EMULATOR_SYSTEM_CONSTANTS")) {
    auto* vulkan_command_processor =
        dynamic_cast<rex::graphics::vulkan::VulkanCommandProcessor*>(
            command_processor);
    if (vulkan_command_processor) {
      const auto& constants =
          vulkan_command_processor->GetSystemConstantsForTraceDiagnostic();
      FILE* constants_file = std::fopen(constants_path, "wb");
      if (constants_file &&
          std::fwrite(&constants, sizeof(constants), 1, constants_file) == 1) {
        std::fprintf(stderr,
                     "Emulator system constants dumped to %s (%zu bytes)\n",
                     constants_path, sizeof(constants));
      } else {
        std::fprintf(stderr, "Cannot dump emulator system constants to %s\n",
                     constants_path);
      }
      if (constants_file) {
        std::fclose(constants_file);
      }
    }
  }

  // Diagnostic: show vertex fetch constant 0 and the first vertices it points
  // at (guest big-endian floats) so draw geometry disputes can be settled.
  {
    const uint32_t fetch0 = (*register_file)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
    const uint32_t fetch1 = (*register_file)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1];
    const uint32_t vb_addr = fetch0 & 0x1FFFFFFCu;
    std::fprintf(stderr, "fetch00: %08x %08x (vb guest addr 0x%08x)\n", fetch0,
                 fetch1, vb_addr);
    const uint8_t* vb = snap.physical_membase + vb_addr;
    for (int v = 0; v < 4; ++v) {
      float f[4];
      for (int c = 0; c < 4; ++c) {
        uint32_t raw;
        std::memcpy(&raw, vb + (v * 4 + c) * 4, 4);
        raw = __builtin_bswap32(raw);
        std::memcpy(&f[c], &raw, 4);
      }
      std::fprintf(stderr, "  v%d: %.3f %.3f %.3f %.3f\n", v, f[0], f[1], f[2], f[3]);
    }
  }

  // The fullscreen postprocess VS in this trace reads its four-float vertex
  // records from the final fetch group (the translated UBO vec4 index 47).
  // Print that group and the first records so primitive-expansion coordinate
  // issues can be distinguished from texture sampling issues.
  {
    const uint32_t fetch_base =
        uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) + 31u * 6u;
    std::fprintf(stderr,
                 "fetch31: %08x %08x %08x %08x %08x %08x\n",
                 (*register_file)[fetch_base + 0],
                 (*register_file)[fetch_base + 1],
                 (*register_file)[fetch_base + 2],
                 (*register_file)[fetch_base + 3],
                 (*register_file)[fetch_base + 4],
                 (*register_file)[fetch_base + 5]);
    // UBO vec4 47 is raw fetch dwords 188..191, or group 31 dwords 2..5.
    // The shader reads vec4 components 2/3, therefore group dwords 4/5.
    const uint32_t vb_addr = (*register_file)[fetch_base + 4] & 0x1FFFFFFCu;
    const uint32_t endian = (*register_file)[fetch_base + 5] & 3u;
    std::fprintf(stderr, "fetch31 vb guest addr 0x%08x endian=%u\n", vb_addr,
                 endian);
    if (vb_addr <= snap.physical_size - 3u * 4u * sizeof(uint32_t)) {
      for (uint32_t v = 0; v < 3; ++v) {
        float values[4];
        for (uint32_t c = 0; c < 4; ++c) {
          uint32_t raw;
          std::memcpy(&raw, snap.physical_membase + vb_addr +
                                 (v * 4u + c) * sizeof(uint32_t),
                      sizeof(raw));
          if (endian == 1u || endian == 2u) {
            raw = ((raw & 0xFF00FF00u) >> 8) |
                  ((raw & 0x00FF00FFu) << 8);
          }
          if (endian == 2u || endian == 3u) {
            raw = (raw >> 16) | (raw << 16);
          }
          std::memcpy(&values[c], &raw, sizeof(raw));
        }
        std::fprintf(stderr, "  fetch31 v%u: %.9g %.9g %.9g %.9g\n", v,
                     values[0], values[1], values[2], values[3]);
      }
    }
  }

  rex::cvar::SetFlagByName("ac6_native_draw_output", out_ppm);
  const int rc = ac6::nr2::ExecuteSnapshotOffscreen(snap);
  static const char* const kResultNames[] = {"EXECUTED", "REJECTED", "FATAL"};
  std::fprintf(stderr, "Native draw for trace command %d: %s (rc=%d)%s%s\n",
               command_index, rc >= 0 && rc <= 2 ? kResultNames[rc] : "?", rc,
               rc == 0 ? ", output: " : "", rc == 0 ? out_ppm : "");
  return rc == 0 ? 0 : 10 + rc;
}

int RunNativeRange(TracePlayer& player, GraphicsSystem* graphics, int first_command,
                   int last_command, const char* out_ppm) {
  const TraceReader::Frame* frame = player.current_frame();
  const int command_count = int(frame->commands.size());
  if (first_command < 0 || last_command < first_command ||
      last_command >= command_count) {
    std::fprintf(stderr, "Command range %d:%d is outside [0, %d)\n",
                 first_command, last_command, command_count);
    return 5;
  }
  rex::cvar::SetFlagByName("ac6_native_draw_output", out_ppm);
  // Trace-only experiment knobs. Production defaults stay byte-exact, while
  // dependency-chain tests can quantify whether visually equivalent UNORM
  // rounding is safe to feed into later native passes.
  if (const char* value = std::getenv("AC6_NR2_RESOLVE_MATCH_PERCENT")) {
    rex::cvar::SetFlagByName("ac6_native_resolve_min_oracle_match_percent",
                            value);
  }
  if (const char* value = std::getenv("AC6_NR2_RESOLVE_TOLERANCE")) {
    rex::cvar::SetFlagByName("ac6_native_resolve_oracle_tolerance", value);
  }
  if (const char* value = std::getenv("AC6_NR2_RESOLVE_UNDERLAY")) {
    rex::cvar::SetFlagByName("ac6_native_resolve_underlay", value);
  }
  if (const char* value =
          std::getenv("AC6_NR2_RESOLVE_ORACLE_COMPOSE")) {
    rex::cvar::SetFlagByName("ac6_native_resolve_oracle_compose", value);
  }
  if (const char* value = std::getenv("AC6_NR2_VALIDATE_RESOLVES")) {
    rex::cvar::SetFlagByName("ac6_native_validate_resolves", value);
  }
  if (const char* value =
          std::getenv("AC6_NR2_PROPAGATE_UNVALIDATED")) {
    rex::cvar::SetFlagByName("ac6_native_propagate_unvalidated", value);
  }
  if (const char* value =
          std::getenv("AC6_NR2_PROPAGATE_AFTER_SWAPS")) {
    rex::cvar::SetFlagByName("ac6_native_propagate_after_swaps", value);
  }
  if (const char* value = std::getenv("AC6_NR2_ZERO_TEXTURE_FETCH")) {
    rex::cvar::SetFlagByName("ac6_native_zero_texture_fetch", value);
  }
  if (const char* value = std::getenv("AC6_NR2_ZERO_TEXTURE_FETCH_2")) {
    rex::cvar::SetFlagByName("ac6_native_zero_texture_fetch_2", value);
  }
  if (const char* value = std::getenv("AC6_NR2_TEXTURE_OFFSET_FETCH")) {
    rex::cvar::SetFlagByName("ac6_native_texture_offset_fetch", value);
  }
  if (const char* value = std::getenv("AC6_NR2_TEXTURE_OFFSET_X")) {
    rex::cvar::SetFlagByName("ac6_native_texture_offset_x", value);
  }
  if (const char* value = std::getenv("AC6_NR2_TEXTURE_OFFSET_Y")) {
    rex::cvar::SetFlagByName("ac6_native_texture_offset_y", value);
  }
  if (const char* value = std::getenv("AC6_NR2_TEXTURE_OVERRIDE_FETCH")) {
    rex::cvar::SetFlagByName("ac6_native_texture_override_fetch", value);
  }
  if (const char* value =
          std::getenv("AC6_NR2_TEXTURE_OVERRIDE_MILLIONTHS")) {
    rex::cvar::SetFlagByName("ac6_native_texture_override_millionths", value);
  }
  using BenchmarkClock = std::chrono::steady_clock;
  const auto session_begin_start = BenchmarkClock::now();
  if (!ac6::nr2::BeginSnapshotOffscreenSession(
          true, graphics->command_processor())) {
    std::fprintf(stderr, "Unable to start persistent native session\n");
    return 8;
  }
  const auto session_begin_end = BenchmarkClock::now();

  int executed = 0;
  int rejected = 0;
  int fatal = 0;
  int resolved = 0;
  int resolve_fallbacks = 0;
  std::chrono::duration<double> trace_replay_time{};
  std::chrono::duration<double> native_draw_time{};
  std::chrono::duration<double> native_resolve_time{};
  const char* dump_draw_dir = std::getenv("AC6_NR2_DUMP_DRAW_DIR");
  for (int command_index = first_command; command_index <= last_command;
       ++command_index) {
    if (frame->commands[command_index].type !=
        TraceReader::Frame::Command::Type::kDraw) {
      continue;
    }
    const auto replay_start = BenchmarkClock::now();
    player.SeekCommand(command_index);
    player.WaitOnPlayback();
    trace_replay_time += BenchmarkClock::now() - replay_start;
    const RegisterFile* register_file = graphics->register_file();
    const uint32_t edram_mode =
        (*register_file)[XE_GPU_REG_RB_MODECONTROL] & 0x7u;
    // EDRAM mode 6 is a resolve/copy boundary. Feed it to the native session
    // before later draws snapshot their render-to-texture inputs. Unsupported
    // depth/float copies deliberately retain the emulator-produced bytes.
    if (edram_mode == uint32_t(xenos::EdramMode::kCopy)) {
      ac6::nr2::DrawSnapshot resolve_snap;
      resolve_snap.capture_index = uint64_t(command_index);
      resolve_snap.regs.assign(register_file->values,
                               register_file->values + RegisterFile::kRegisterCount);
      resolve_snap.physical_membase = graphics->memory()->physical_membase();
      resolve_snap.physical_size = UINT32_C(1) << 29;
      resolve_snap.tool_relaxed = true;
      const auto native_resolve_start = BenchmarkClock::now();
      const int resolve_rc =
          ac6::nr2::ResolveSnapshotInOffscreenSession(resolve_snap);
      native_resolve_time +=
          BenchmarkClock::now() - native_resolve_start;
      if (resolve_rc == 0) {
        ++resolved;
      } else if (resolve_rc == 1) {
        ++resolve_fallbacks;
      } else {
        ++fatal;
        break;
      }
      continue;
    }
    // EDRAM mode 4 is ordinary color/depth drawing. Depth-only helper draws
    // and other modes are boundaries, not draws into the persistent target.
    if (edram_mode != uint32_t(xenos::EdramMode::kColorDepth)) {
      continue;
    }

    ac6::nr2::DrawSnapshot snap;
    snap.capture_index = uint64_t(command_index);
    snap.regs.assign(register_file->values,
                     register_file->values + RegisterFile::kRegisterCount);
    CommandProcessor* command_processor = graphics->command_processor();
    CopyShaderInfo(command_processor->active_vertex_shader(), snap.vs);
    CopyShaderInfo(command_processor->active_pixel_shader(), snap.ps);
    snap.physical_membase = graphics->memory()->physical_membase();
    snap.physical_size = UINT32_C(1) << 29;
    snap.tool_relaxed = true;
    std::string dump_draw_path;
    if (dump_draw_dir && dump_draw_dir[0]) {
      dump_draw_path = std::string(dump_draw_dir) + "/draw-" +
                       std::to_string(command_index) + ".ppm";
      rex::cvar::SetFlagByName("ac6_native_draw_output",
                              dump_draw_path.c_str());
      snap.tool_suppress_output = false;
    } else {
      snap.tool_suppress_output = command_index != last_command;
    }
    auto vgt = register_file->Get<reg::VGT_DRAW_INITIATOR>();
    snap.record.primitive_type = uint32_t(vgt.prim_type);
    snap.record.count = vgt.num_indices;

    const auto native_draw_start = BenchmarkClock::now();
    const int rc = ac6::nr2::ExecuteSnapshotInOffscreenSession(snap);
    native_draw_time += BenchmarkClock::now() - native_draw_start;
    if (rc == 0) {
      ++executed;
    } else if (rc == 1) {
      ++rejected;
    } else {
      ++fatal;
      break;
    }
  }
  const auto session_end_start = BenchmarkClock::now();
  ac6::nr2::EndSnapshotOffscreenSession();
  const auto session_end_end = BenchmarkClock::now();
  const double session_begin_seconds =
      std::chrono::duration<double>(session_begin_end -
                                    session_begin_start)
          .count();
  const double session_end_seconds =
      std::chrono::duration<double>(session_end_end - session_end_start)
          .count();
  std::fprintf(stderr,
               "Native range %d:%d complete: %d executed, %d rejected, %d fatal; "
               "%d native resolves, %d resolve fallbacks; final output: %s\n",
               first_command, last_command, executed, rejected, fatal, resolved,
               resolve_fallbacks, out_ppm);
  std::fprintf(
      stderr,
      "nr2-benchmark-phases: init=%.6fs trace_replay=%.6fs "
      "native_draw=%.6fs native_resolve=%.6fs teardown=%.6fs\n",
      session_begin_seconds, trace_replay_time.count(),
      native_draw_time.count(), native_resolve_time.count(),
      session_end_seconds);
  return fatal ? 12 : executed ? 0 : 11;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view mode = argc >= 2 ? argv[1] : "";
  const bool is_scan = mode == "scan";
  const bool is_draw = mode == "draw";
  const bool is_range = mode == "range";
  const bool is_edram = mode == "edram";
  const bool is_resolve = mode == "resolve";
  const bool is_present = mode == "present";
  const bool is_frame_throughput = mode == "frame-throughput";
  if ((!is_scan && !is_draw && !is_range && !is_edram && !is_resolve &&
       !is_present && !is_frame_throughput) ||
      (is_scan && argc < 3) ||
      (is_draw && argc < 5) || (is_edram && argc < 4) ||
      (is_resolve && argc < 5) ||
      (is_present && (argc < 4 || argc > 5)) ||
      (is_frame_throughput && (argc < 3 || argc > 5))) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s scan TRACE.xtr [OUT.tsv]\n"
                 "  %s draw TRACE.xtr COMMAND_INDEX OUT.ppm [PIPELINE_STORE "
                 "[SHADER_DIR]]\n"
                 "  %s range TRACE.xtr FIRST_COMMAND LAST_COMMAND OUT.ppm "
                 "[PIPELINE_STORE [SHADER_DIR]]\n"
                 "  %s resolve TRACE.xtr COMMAND_INDEX OUT.bin [ADDR_HEX BYTES]\n"
                 "  %s present TRACE.xtr OUT.ppm [COMMAND_INDEX=-1]\n"
                 "  %s frame-throughput TRACE.xtr [WARMUP_FRAMES "
                 "[MEASURED_FRAMES]]\n"
                 "  %s edram CHECKPOINT.xtr OUT.bin\n",
                 argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
                 argv[0]);
    return 2;
  }
  if (is_present) {
    // present returns through the generic replay entry point before
    // ApplyReplayDefaults, so map the trace-only switch here as well.
    const char* rect_clear_env =
        std::getenv("AC6_NR2_RECT_CLEAR_LOWERING");
    const bool rect_clear_requested =
        rect_clear_env && std::strcmp(rect_clear_env, "1") == 0;
    const bool rect_clear_set = rex::cvar::SetFlagByName(
        "vulkan_ac6_rect_clear_lowering",
        rect_clear_requested ? "true" : "false");
    const bool rect_clear_oracle_set = rex::cvar::SetFlagByName(
        "vulkan_ac6_rect_clear_oracle_telemetry", "false");
    const std::string rect_clear_effective =
        rex::cvar::GetFlagByName("vulkan_ac6_rect_clear_lowering");
    const std::string rect_clear_oracle_effective = rex::cvar::GetFlagByName(
        "vulkan_ac6_rect_clear_oracle_telemetry");
    std::fprintf(
        stderr,
        "Trace present rectangle-clear lowering: requested=%s set=%s "
        "effective='%s' oracle_set=%s oracle_effective='%s'\n",
        rect_clear_requested ? "true" : "false",
        rect_clear_set ? "true" : "false", rect_clear_effective.c_str(),
        rect_clear_oracle_set ? "true" : "false", rect_clear_oracle_effective.c_str());
    int present_command = -1;
    if (argc == 5 && !ParseInt(argv[4], present_command)) {
      std::fprintf(stderr, "COMMAND_INDEX must be an integer\n");
      return 2;
    }
    return rex_gpu_replay_trace(argv[2], argv[3], 0, present_command);
  }
  if (is_edram) {
    // Pure file-to-file extraction; no graphics runtime required.
    EdramSnapshotReader reader;
    if (!reader.Open(argv[2])) {
      std::fprintf(stderr, "Unable to open trace: %s\n", argv[2]);
      return 4;
    }
    return reader.DumpFirstSnapshot(argv[3]);
  }
  const char* trace_path = argv[2];
  int command_index = -1;
  int range_last_command = -1;
  if ((is_draw || is_resolve) && !ParseInt(argv[3], command_index)) {
    std::fprintf(stderr, "COMMAND_INDEX must be an integer\n");
    return 2;
  }
  if (is_draw && argc >= 6) {
    rex::cvar::SetFlagByName("ac6_native_pipeline_store", argv[5]);
  }
  if (is_draw && argc >= 7) {
    rex::cvar::SetFlagByName("ac6_native_shader_dir", argv[6]);
  }
  if (is_range) {
    if (argc < 6 || !ParseInt(argv[3], command_index) ||
        !ParseInt(argv[4], range_last_command)) {
      std::fprintf(stderr, "range requires integer FIRST_COMMAND LAST_COMMAND\n");
      return 2;
    }
    if (argc >= 7) {
      rex::cvar::SetFlagByName("ac6_native_pipeline_store", argv[6]);
    }
    if (argc >= 8) {
      rex::cvar::SetFlagByName("ac6_native_shader_dir", argv[7]);
    }
  }
  uint32_t resolve_addr = 0, resolve_bytes = 0;
  if (is_resolve && argc >= 7) {
    resolve_addr = uint32_t(std::strtoul(argv[5], nullptr, 16));
    resolve_bytes = uint32_t(std::strtoul(argv[6], nullptr, 0));
  }
  int throughput_warmup_frames = 2;
  int throughput_measured_frames = 10;
  if (is_frame_throughput) {
    if ((argc >= 4 && !ParseInt(argv[3], throughput_warmup_frames)) ||
        (argc >= 5 && !ParseInt(argv[4], throughput_measured_frames)) ||
        throughput_warmup_frames < 1 || throughput_warmup_frames > 100 ||
        throughput_measured_frames < 1 ||
        throughput_measured_frames > 1000) {
      std::fprintf(stderr,
                   "frame-throughput requires WARMUP_FRAMES in [1, 100] "
                   "and MEASURED_FRAMES in [1, 1000]\n");
      return 2;
    }
  }

  ApplyReplayDefaults(is_frame_throughput);
  if (is_frame_throughput &&
      !ApplyPrimitiveCacheThresholdTraceOverride()) {
    std::fprintf(stderr,
                 "Unable to apply the primitive-cache threshold trace "
                 "override\n");
    return 2;
  }
  if (!ApplyWaterVrsTraceOverride()) {
    std::fprintf(stderr,
                 "Unable to apply the exact-water VRS trace override\n");
    return 2;
  }
  if (is_frame_throughput) {
    // Force both the shared string mode and the Vulkan legacy alias off. A
    // config-provided legacy boolean must not silently turn "none" into the
    // one-frame-delayed fast readback mode and contaminate this measurement.
    rex::cvar::SetFlagByName("readback_resolve", "none");
    rex::cvar::SetFlagByName("vulkan_readback_resolve", "false");
  }
  if (is_resolve || is_range ||
      (!is_frame_throughput &&
       std::getenv("AC6_NR2_RENDERDOC_CAPTURE") != nullptr)) {
    // Resolves must land in CPU-visible guest memory for standalone dumps and
    // for native range dependency handoff / byte-oracle comparison.
    // Use immediate synchronization here. The legacy Vulkan boolean maps to
    // the one-frame-delayed "fast" mode, which makes a standalone trace dump
    // observe an older resolve and invalidates byte-oracle comparisons.
    rex::cvar::SetFlagByName("readback_resolve", "full");
  }

  auto graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  auto* graphics_ptr = graphics.get();
  if (XFAILED(graphics_ptr->SetupPresentation(nullptr))) {
    std::fprintf(stderr, "Unable to create offscreen Vulkan presenter\n");
    return 3;
  }
  auto runtime = std::make_unique<rex::Runtime>(std::filesystem::path());
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  if (XFAILED(runtime->Setup(std::move(config)))) {
    std::fprintf(stderr, "Unable to initialize trace replay runtime\n");
    return 3;
  }

  int result = 0;
  {
    TracePlayer player(graphics_ptr);
    if (!player.Open(trace_path)) {
      std::fprintf(stderr, "Unable to open trace: %s\n", trace_path);
      result = 4;
    } else if (player.frame_count() == 0 || !player.current_frame()) {
      std::fprintf(stderr, "Trace contains no frames\n");
      result = 5;
    } else if (is_scan) {
      result = ScanCommands(player, graphics_ptr, argc >= 4 ? argv[3] : nullptr);
    } else if (is_resolve) {
      result = RunResolveDump(player, graphics_ptr, command_index, argv[4],
                              resolve_addr, resolve_bytes);
    } else if (is_range) {
      result = RunNativeRange(player, graphics_ptr, command_index,
                              range_last_command, argv[5]);
    } else if (is_frame_throughput) {
      result = RunFrameThroughput(player, graphics_ptr,
                                  throughput_warmup_frames,
                                  throughput_measured_frames);
    } else {
      result = RunNativeDraw(player, graphics_ptr, command_index, argv[4]);
    }
  }
  runtime.reset();
  return result;
}
