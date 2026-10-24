// AC6 Native Renderer v2 — M2 scaffold (RenderCommand queue + render thread +
// guest Present hook, observe/parallel mode) plus M3 slice 1: the pipeline
// prover. On render-thread startup (cvar on), a minimal PRIVATE headless
// Vulkan context is created (its own instance/device — the SDK's device is
// never touched), the offline-converted SPIR-V blobs from
// ac6_native_shader_dir are loaded as VkShaderModules, and the stored
// pipeline descriptions from ac6_native_pipeline_store (the runtime's
// .fbo.vk.xpso, opened READ-ONLY) are translated to native VkPipelines to
// prove the offline shader pipeline + stored state are sufficient for native
// pipeline creation on this driver. See native_renderer.h and
// docs/NATIVE_RENDERER_V2_PLAN.md.
//
// Key facts this slice relies on (verified against the SDK):
// - The SDK's translated shaders consume vertices via storage-buffer vertex
//   pulling (the only vertex-stage Input is gl_VertexIndex; vfetch reads
//   xe_shared_memory, set 0 binding 0). Pipelines therefore need NO vertex
//   input state — VulkanPipelineCache::EnsurePipelineCreated passes an empty
//   VkPipelineVertexInputStateCreateInfo, and so do we.
// - Descriptor sets follow SpirvShaderTranslator::DescriptorSet: set 0 shared
//   memory (SSBO), set 1 the five constant UBOs, set 2 vertex textures,
//   set 3 pixel textures (sampled images first, then samplers). Rather than
//   hardcoding per-shader texture counts, we reflect (set, binding,
//   descriptor type, count) directly from each SPIR-V binary and build
//   compatible set/pipeline layouts from the merged VS+PS interface.
// - The M1 conversion (xsh2spv) used SpirvShaderTranslator::Features(true):
//   SPIR-V 1.5 (needs a Vulkan 1.2+ device), FBO path (no EDRAM interlock),
//   single shared-memory binding, float-controls capabilities, clip/cull
//   distances, stores in both stages, sample-rate shading.
// - .vk.xpso record layout is PipelineStoredDescription
//   (thirdparty/rexglue-sdk/include/rex/graphics/vulkan/pipeline_cache.h):
//   u64 XXH3 of the 58-byte PipelineDescription, then the description
//   itself: 4x u64 shader hash/modification, u32 RenderPassKey, 45 bits of
//   packed state in 6 bytes, 4x u32 PipelineRenderTarget blend state.
//
// M3 slice 2 — first native draw, offscreen. A new integer cvar
// ac6_native_capture_draw arms a one-shot trigger: from the Nth captured
// guest DrawCallRecord onward, the capture layer hands nr2 a DrawSnapshot
// (shadow state + a copy of the emulated GPU RegisterFile + the active
// emulated shader identities/constant maps + the guest physical memory host
// pointer). The render thread then:
// - imports the live 512MB guest physical memory window into the private
//   device via VK_EXT_external_memory_host as the set-0 xe_shared_memory
//   SSBO (zero-copy; one-time full copy into a host-visible buffer as the
//   fallback when the import path is unavailable),
// - matches the snapshot's (vs,ps) ucode hashes + topology against the
//   stored pipeline descriptions to select an already-created native
//   VkPipeline,
// - reconstructs the five set-1 constant UBOs the way the SDK's
//   VulkanCommandProcessor::UpdateSystemConstantValues + constant upload
//   paths do (system constants from the register snapshot via
//   draw_util::GetHostViewportInfo etc., float constants packed per the
//   shader constant maps, bool/loop + fetch constants copied raw),
// - executes the draw with dynamic rendering into an offscreen color target
//   matching the stored render-pass key formats, and
// - reads the target back and writes /tmp/m3-test/native-draw.ppm, logging
//   'nr2: native draw executed, N pixels covered'.
// If a snapshot is unsuitable (unsupported topology, no matching pipeline,
// shaders not yet analyzed by the emulated renderer, ...), it is rejected
// and the trigger re-arms for the next captured draw.
//
// The same draw-execution path also serves the fully offscreen M3 slice 2
// trace harness (the ac6-nr2-trace-draw tool, trace_draw_main.cpp, built
// with AC6_NR2_TRACE_TOOL which drops the guest Present hook below): the
// tool replays a captured GPU trace up to a chosen draw command with the
// SDK's TracePlayer, builds the DrawSnapshot from the trace runtime's
// register file / active shaders / guest memory, and calls
// ExecuteSnapshotOffscreen. Snapshots built that way carry tool_relaxed,
// which permits clip-disabled giant-viewport draws (offscreen RT capped at
// 1280x720) and zero-coverage results - parity for the harness is judged
// externally against the emulated renderer's resolved-RT bytes.

#include "native_renderer.h"
#include "frame_interval_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/vulkan/api.h>  // Vulkan types only (VK_NO_PROTOTYPES).
#include <rex/ui/vulkan/device.h>

// M3 slice 2: emulated-GPU state access for the draw-capture snapshot (guest
// thread) and register decoding on the render thread. The SDK's translated
// draw helpers (draw_util) operate on a plain RegisterFile, so the snapshot
// copy can be fed straight through them.
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(ac6_native_renderer, false, "AC6/NativeRenderer",
                    "Enable the native renderer v2 module (M2 scaffold: observe mode - logs a "
                    "native present tick per guest swap; emulated renderer still presents. "
                    "M3 slice 1: proves native pipeline creation from the offline shader "
                    "cache on a private headless Vulkan device at render-thread startup)");

REXCVAR_DEFINE_BOOL(ac6_native_takeover, false, "AC6/NativeRenderer",
                    "Execute every decoded Vulkan draw and copy through the persistent native "
                    "renderer session. The emulator remains the presentation and safety "
                    "fallback path; only native resolves that pass the configured oracle gate "
                    "are propagated to guest memory");

REXCVAR_DEFINE_BOOL(
    ac6_native_shared_authority, false, "AC6/NativeRenderer",
    "Create exact native pipelines with the live SDK shaders on the "
    "emulator-owned Vulkan device "
    "and draw directly into its authoritative render targets. Unsupported draws "
    "continue through the emulator pipeline; the private duplicate renderer is "
    "disabled while this mode is active.");

REXCVAR_DEFINE_BOOL(
    ac6_native_shared_point_expansion, false, "AC6/NativeRenderer",
    "Allow shared-authority pipelines for point host-vertex expansion. "
    "When enabled, the exact live SDK vertex module is borrowed; disabled by "
    "default as a rollout safety switch.");

REXCVAR_DEFINE_BOOL(
    ac6_native_shared_rectangle_expansion, false, "AC6/NativeRenderer",
    "Allow shared-authority pipelines for rectangle host-vertex expansion. "
    "When enabled, the exact live SDK vertex module is borrowed; disabled by "
    "default as a rollout safety switch.");

REXCVAR_DEFINE_BOOL(
    ac6_native_water_vrs, false, "AC6/NativeRenderer",
    "Use 2x2 pipeline fragment shading rate only for the exact profiled AC6 "
    "water pipeline when the active logical device exposes the required "
    "VK_KHR_fragment_shading_rate feature and 2-sample rate")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(
    ac6_native_takeover_enable_marker, "", "AC6/NativeRenderer",
    "At a GPU swap, consume this marker file and enable live native takeover. "
    "This lets automated tests reach gameplay through the normal renderer, then "
    "start the expensive experimental path without restarting or repeating menus.");
REXCVAR_DEFINE_STRING(
    ac6_native_takeover_disable_marker, "", "AC6/NativeRenderer",
    "At a GPU swap, consume this marker file and disable live native takeover.");

REXCVAR_DEFINE_BOOL(ac6_native_validate_resolves, true, "AC6/NativeRenderer",
                    "Synchronously read emulator resolves back for native byte-oracle "
                    "validation. Disable for non-blocking live observation; native draws and "
                    "resolves still execute, but native resolve data is not propagated");

REXCVAR_DEFINE_BOOL(ac6_native_propagate_unvalidated, false,
                    "AC6/NativeRenderer",
                    "Opt in to propagating supported native RGBA8 resolves without the "
                    "synchronous emulator oracle. Experimental live-takeover mode; the "
                    "default remains fail-safe observation only");

REXCVAR_DEFINE_BOOL(
    ac6_native_gpu_resolve, true, "AC6/NativeRenderer",
    "Write eligible unvalidated native RGBA8 resolves directly from the private "
    "Vulkan device into coherently imported guest memory. Falls back to the "
    "existing readback/CPU-retile path when any safety condition is not met.");

REXCVAR_DEFINE_BOOL(
    ac6_native_verbose_execution_logs, false, "AC6/NativeRenderer",
    "Emit per-draw, per-resolve, and per-present native execution records. "
    "Disabled by default because live takeover can execute thousands of draws "
    "per frame; the trace tool retains these records for regression checks.");

REXCVAR_DEFINE_INT32(ac6_native_propagate_after_swaps, 1,
                     "AC6/NativeRenderer",
                     "Defer unvalidated live propagation until this many exact GPU swaps "
                     "have completed, allowing startup render-target state to warm up");

REXCVAR_DEFINE_STRING(ac6_native_shader_dir, "/home/deck/Projects/native-shader-pipeline/spv-out",
                      "AC6/NativeRenderer",
                      "Directory of offline-converted SPIR-V shader blobs "
                      "(<ucode_hash>.{v|p}.<modification>.spv + manifest.tsv, M1 output)");

REXCVAR_DEFINE_STRING(ac6_native_pipeline_store,
                      "/home/deck/Games/ACE6-Recomp-Native-Test/cache/shaders/shareable/"
                      "4E4D07D1.fbo.vk.xpso",
                      "AC6/NativeRenderer",
                      "Stored pipeline description file (.fbo.vk.xpso) to prove native "
                      "VkPipeline creation against (opened read-only)");

REXCVAR_DEFINE_BOOL(
    ac6_native_lazy_pipeline_creation, true, "AC6/NativeRenderer",
    "Parse and retain the native pipeline store at session startup, but defer "
    "private VkPipeline creation until a matching captured draw first uses it");

REXCVAR_DEFINE_INT32(ac6_native_capture_draw, 0, "AC6/NativeRenderer",
                     "M3 slice 2: when > 0 (and ac6_native_renderer + ac6_render_capture are "
                     "on), snapshot the Nth captured guest draw (and subsequent draws until "
                     "one is suitable) and execute it natively offscreen, writing "
                     "/tmp/m3-test/native-draw.ppm");

REXCVAR_DEFINE_STRING(ac6_native_draw_output, "/tmp/m3-test/native-draw.ppm",
                      "AC6/NativeRenderer",
                      "Output PPM path for the M3 slice 2 offscreen native draw");

REXCVAR_DEFINE_INT32(ac6_native_resolve_min_oracle_match_percent, 100,
                     "AC6/NativeRenderer",
                     "Trace-session safety gate: propagate a native RGBA8 resolve into "
                     "guest memory only when at least this percent of its texels are "
                     "within ac6_native_resolve_oracle_tolerance of the emulator oracle");

REXCVAR_DEFINE_INT32(ac6_native_resolve_oracle_tolerance, 0,
                     "AC6/NativeRenderer",
                     "Maximum absolute error per RGBA8 channel counted as a match by "
                     "the trace-session resolve safety gate (set 0 with a 100 percent "
                     "gate for byte-exact-only propagation)");

REXCVAR_DEFINE_BOOL(ac6_native_edram_alias_experimental, false,
                    "AC6/NativeRenderer",
                    "Experimentally reuse/seed native color targets across Xenos EDRAM "
                    "surface-view changes; disabled until pitch-view remapping is exact");

REXCVAR_DEFINE_BOOL(ac6_native_resolve_underlay, true,
                    "AC6/NativeRenderer",
                    "Allow resolve-time zero-pixel fill from an older compatible EDRAM "
                    "surface view. This is a diagnostic compatibility heuristic; disable "
                    "to prevent legitimate zero pixels from reviving stale alias contents");

REXCVAR_DEFINE_BOOL(ac6_native_resolve_oracle_compose, false,
                    "AC6/NativeRenderer",
                    "Diagnostic only: when synchronous emulator validation is active, "
                    "select the closer of the primary and compatible alias texels. This "
                    "quantifies alias mapping independently of the unresolved native "
                    "color/depth ownership policy");

REXCVAR_DEFINE_INT32(ac6_native_zero_texture_fetch, -1, "AC6/NativeRenderer",
                     "Trace diagnostic: replace this texture fetch constant's uploaded "
                     "base level with zeroes (-1 disables)");
REXCVAR_DEFINE_INT32(ac6_native_zero_texture_fetch_2, -1, "AC6/NativeRenderer",
                     "Trace diagnostic: optional second texture fetch constant whose "
                     "uploaded base level is replaced with zeroes (-1 disables)");

REXCVAR_DEFINE_INT32(ac6_native_texture_offset_fetch, -1, "AC6/NativeRenderer",
                     "Trace diagnostic: apply ac6_native_texture_offset_x/y while "
                     "uploading this fetch constant (-1 disables)");
REXCVAR_DEFINE_INT32(ac6_native_texture_offset_x, 0, "AC6/NativeRenderer",
                     "Trace diagnostic texture source X offset, in texels/blocks");
REXCVAR_DEFINE_INT32(ac6_native_texture_offset_y, 0, "AC6/NativeRenderer",
                     "Trace diagnostic texture source Y offset, in texels/blocks");
REXCVAR_DEFINE_INT32(ac6_native_texture_override_fetch, -1, "AC6/NativeRenderer",
                     "Trace diagnostic: replace an R32_FLOAT fetch with "
                     "ac6_native_texture_override_millionths (-1 disables)");
REXCVAR_DEFINE_INT32(ac6_native_texture_override_millionths, 0,
                     "AC6/NativeRenderer",
                     "Trace diagnostic R32_FLOAT replacement value in millionths");

namespace {
const rex::LogCategoryId kLogGPU = rex::log::GPU;
}  // namespace

namespace ac6::nr2 {
namespace {

// Producer overrun guard only; the consumer just logs, so in practice the
// queue stays near-empty. Overflow drops the command and counts it.
constexpr size_t kMaxQueuedCommands = 1024;

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<RenderCommand> g_queue;

std::thread g_render_thread;
std::atomic<bool> g_running{false};   // render thread should keep consuming
std::atomic<bool> g_active{false};    // module enabled + thread started
std::atomic<uint64_t> g_dropped_commands{0};

// M3 slice 2 — one-shot draw-capture trigger state.
std::atomic<bool> g_capture_armed{false};      // cvar > 0, no successful draw yet
std::atomic<bool> g_capture_inflight{false};   // one snapshot in flight at a time
std::atomic<uint64_t> g_captured_draws_seen{0};
std::atomic<uint64_t> g_snapshot_attempts{0};
std::atomic<uint64_t> g_live_gpu_swap_count{0};
constexpr uint64_t kMaxSnapshotAttempts = 20000;
std::mutex g_pending_snapshot_mutex;
std::unique_ptr<DrawSnapshot> g_pending_snapshot;

bool DetailedExecutionLogsEnabled(const DrawSnapshot& snap) {
#if defined(AC6_NR2_TRACE_TOOL)
    // The deterministic trace benchmark derives its resolve signature and
    // command counts from these records.
    return true;
#else
    return !snap.tool_suppress_output ||
           REXCVAR_GET(ac6_native_verbose_execution_logs);
#endif
}

bool HotTextureExecutionLogsEnabled(const DrawSnapshot& snap) {
    // Trace signatures depend on draw/resolve result records, not on one log
    // per reflected texture binding. Suppress those high-volume records for
    // tool-suppressed continuous sessions unless explicitly requested.
    return !snap.tool_suppress_output ||
           REXCVAR_GET(ac6_native_verbose_execution_logs);
}

// The common AC6 resolve is an RGBA8 render target copied into a 32x32-tiled
// guest texture. Keeping this operation on the GPU removes a full-surface
// image readback plus hundreds of thousands of CPU address/packing operations
// at every copy boundary. The address calculation is the exact bit layout in
// texture_util::GetTiledOffset2D for bytes_per_block_log2 == 2.
constexpr std::string_view kGpuResolveShaderSource = R"glsl(
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(set = 0, binding = 1, std430) buffer GuestMemory {
    uint words[];
} guest_memory;

layout(push_constant) uniform ResolveParams {
    uvec4 rect;          // x0, y0, x1, y1
    uvec4 dimensions;    // source width/height, destination pitch/height
    uvec4 destination;   // guest base, output-buffer guest base, flags, size
} params;

uint TiledOffset2DRgba8(uint x, uint y, uint pitch) {
    pitch = (pitch + 31u) & ~31u;
    uint macro_offset =
        ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << 9u;
    uint micro_offset = ((x & 7u) + ((y & 14u) << 2u)) << 2u;
    uint offset = macro_offset + ((micro_offset & ~15u) << 1u) +
                  (micro_offset & 15u) + ((y & 1u) << 4u);
    return ((offset & ~511u) << 3u) + ((y & 16u) << 7u) +
           ((offset & 448u) << 2u) +
           (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) +
           (offset & 63u);
}

void main() {
    uvec2 first_xy = params.rect.xy +
        uvec2(gl_GlobalInvocationID.x << 2u, gl_GlobalInvocationID.y);
    if (first_xy.y >= params.rect.w ||
        first_xy.y >= params.dimensions.y ||
        first_xy.y >= params.dimensions.w) {
        return;
    }

    uint base = params.destination.x;
    uint output_base = params.destination.y;
    uint output_size = params.destination.w;
    if (output_size < 4u) {
        return;
    }
    bool swap_rb = (params.destination.z & 1u) != 0u;
    bool endian_8in32 = (params.destination.z & 2u) != 0u;
    for (uint pixel = 0u; pixel < 4u; ++pixel) {
        uvec2 xy = first_xy + uvec2(pixel, 0u);
        if (xy.x >= params.rect.z || xy.x >= params.dimensions.x ||
            xy.x >= params.dimensions.z) {
            continue;
        }
        uint tiled_offset = TiledOffset2DRgba8(
            xy.x, xy.y, params.dimensions.z);
        uint address = base + tiled_offset;
        if (address < base || address < output_base) {
            continue;
        }
        uint output_offset = address - output_base;
        if (output_offset > output_size ||
            output_size - output_offset < 4u) {
            continue;
        }
        uvec4 rgba = uvec4(clamp(
            texelFetch(source_image, ivec2(xy), 0) * 255.0 + vec4(0.5),
            vec4(0.0), vec4(255.0)));
        uint packed;
        if (endian_8in32) {
            packed = rgba.a |
                     ((swap_rb ? rgba.r : rgba.b) << 8u) |
                     (rgba.g << 16u) |
                     ((swap_rb ? rgba.b : rgba.r) << 24u);
        } else {
            packed = (swap_rb ? rgba.b : rgba.r) |
                     (rgba.g << 8u) |
                     ((swap_rb ? rgba.r : rgba.b) << 16u) |
                     (rgba.a << 24u);
        }
        guest_memory.words[output_offset >> 2u] = packed;
    }
}
)glsl";

// ---------------------------------------------------------------------------
// M3 slice 1 — native pipeline prover
// ---------------------------------------------------------------------------

// Minimal function table loaded via dlopen(libvulkan.so.1) +
// vkGetInstanceProcAddr; the SDK's own loader/instance/device are not touched.
struct VulkanFns {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    // M3 slice 2 — draw execution.
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkResetCommandPool ResetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering CmdEndRendering = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdSetDepthBias CmdSetDepthBias = nullptr;
    PFN_vkCmdSetBlendConstants CmdSetBlendConstants = nullptr;
    PFN_vkCmdSetStencilCompareMask CmdSetStencilCompareMask = nullptr;
    PFN_vkCmdSetStencilWriteMask CmdSetStencilWriteMask = nullptr;
    PFN_vkCmdSetStencilReference CmdSetStencilReference = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
};

// One descriptor binding reflected from a SPIR-V binary.
struct ReflectedBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t count = 1;
    // For image descriptors: SPIR-V Dim (0=1D 1=2D 2=3D 3=Cube), arrayedness
    // and multisampledness - needed to build compatible placeholder views.
    uint32_t image_dim = UINT32_MAX;
    uint32_t image_arrayed = 0;
    uint32_t image_ms = 0;
};

// Minimal SPIR-V reflection: descriptor (set, binding, type, count) only.
// Handles the interface the SDK's SpirvShaderTranslator emits: UBOs
// (Uniform + Block), SSBOs (StorageBuffer + Block or Uniform + BufferBlock),
// sampled images, samplers, and arrays thereof.
bool ReflectSpirvBindings(const uint32_t* words, size_t word_count,
                          std::vector<ReflectedBinding>& out) {
    constexpr uint32_t kMagic = 0x07230203;
    if (word_count < 5 || words[0] != kMagic) {
        return false;
    }
    enum class TypeKind : uint8_t {
        kOther,
        kSampler,
        kImage,
        kSampledImage,
        kStruct,
        kArray,
        kRuntimeArray,
        kPointer,
    };
    struct TypeInfo {
        TypeKind kind = TypeKind::kOther;
        uint32_t element = 0;        // array/runtime array/pointer element type
        uint32_t length_id = 0;      // array length constant id
        uint32_t image_sampled = 0;  // OpTypeImage "Sampled" operand
        uint32_t image_dim = 0;      // OpTypeImage "Dim" operand
        uint32_t image_arrayed = 0;  // OpTypeImage "Arrayed" operand
        uint32_t image_ms = 0;       // OpTypeImage "MS" operand
        uint32_t storage_class = 0;  // pointer storage class
    };
    struct VarInfo {
        uint32_t type_id = 0;
        uint32_t storage_class = 0;
    };
    std::unordered_map<uint32_t, TypeInfo> types;
    std::unordered_map<uint32_t, uint32_t> constants;  // id -> low 32-bit value
    std::unordered_map<uint32_t, VarInfo> variables;
    std::unordered_map<uint32_t, uint32_t> dec_set, dec_binding;
    std::unordered_map<uint32_t, bool> dec_buffer_block;

    constexpr uint32_t kOpTypeImage = 25, kOpTypeSampler = 26, kOpTypeSampledImage = 27,
                       kOpTypeArray = 28, kOpTypeRuntimeArray = 29, kOpTypeStruct = 30,
                       kOpTypePointer = 32, kOpConstant = 43, kOpVariable = 59, kOpDecorate = 71;
    constexpr uint32_t kDecBlock = 2, kDecBufferBlock = 3, kDecBinding = 33, kDecDescriptorSet = 34;
    constexpr uint32_t kStorageUniformConstant = 0, kStorageUniform = 2, kStorageStorageBuffer = 12;

    for (size_t i = 5; i < word_count;) {
        const uint32_t insn = words[i];
        const uint32_t opcode = insn & 0xFFFFu;
        const uint32_t len = insn >> 16;
        if (len == 0 || i + len > word_count) {
            return false;  // malformed
        }
        const uint32_t* op = &words[i];
        switch (opcode) {
            case kOpDecorate:
                if (len >= 3) {
                    const uint32_t target = op[1];
                    const uint32_t decoration = op[2];
                    if (decoration == kDecDescriptorSet && len >= 4) {
                        dec_set[target] = op[3];
                    } else if (decoration == kDecBinding && len >= 4) {
                        dec_binding[target] = op[3];
                    } else if (decoration == kDecBufferBlock || decoration == kDecBlock) {
                        if (decoration == kDecBufferBlock) {
                            dec_buffer_block[target] = true;
                        }
                    }
                }
                break;
            case kOpTypeSampler:
                types[op[1]].kind = TypeKind::kSampler;
                break;
            case kOpTypeImage:
                if (len >= 8) {
                    TypeInfo& t = types[op[1]];
                    t.kind = TypeKind::kImage;
                    t.image_dim = op[3];
                    t.image_arrayed = op[5];
                    t.image_ms = op[6];
                    t.image_sampled = op[7];
                }
                break;
            case kOpTypeSampledImage:
                if (len >= 3) {
                    TypeInfo& t = types[op[1]];
                    t.kind = TypeKind::kSampledImage;
                    t.element = op[2];
                }
                break;
            case kOpTypeStruct:
                types[op[1]].kind = TypeKind::kStruct;
                break;
            case kOpTypeArray:
                if (len >= 4) {
                    TypeInfo& t = types[op[1]];
                    t.kind = TypeKind::kArray;
                    t.element = op[2];
                    t.length_id = op[3];
                }
                break;
            case kOpTypeRuntimeArray:
                if (len >= 3) {
                    TypeInfo& t = types[op[1]];
                    t.kind = TypeKind::kRuntimeArray;
                    t.element = op[2];
                }
                break;
            case kOpTypePointer:
                if (len >= 4) {
                    TypeInfo& t = types[op[1]];
                    t.kind = TypeKind::kPointer;
                    t.storage_class = op[2];
                    t.element = op[3];
                }
                break;
            case kOpConstant:
                if (len >= 4) {
                    constants[op[2]] = op[3];
                }
                break;
            case kOpVariable:
                if (len >= 4) {
                    variables[op[2]] = VarInfo{op[1], op[3]};
                }
                break;
            default:
                break;
        }
        i += len;
    }

    for (const auto& [var_id, var] : variables) {
        auto set_it = dec_set.find(var_id);
        auto binding_it = dec_binding.find(var_id);
        if (set_it == dec_set.end() || binding_it == dec_binding.end()) {
            continue;  // not a descriptor-backed variable
        }
        ReflectedBinding rb;
        rb.set = set_it->second;
        rb.binding = binding_it->second;
        // Resolve pointer -> (arrays of) resource type.
        auto type_it = types.find(var.type_id);
        if (type_it == types.end() || type_it->second.kind != TypeKind::kPointer) {
            continue;
        }
        uint32_t elem_id = type_it->second.element;
        uint32_t count = 1;
        for (;;) {
            auto it = types.find(elem_id);
            if (it == types.end()) {
                break;
            }
            if (it->second.kind == TypeKind::kArray) {
                auto len_it = constants.find(it->second.length_id);
                count *= (len_it != constants.end() && len_it->second) ? len_it->second : 1;
                elem_id = it->second.element;
                continue;
            }
            if (it->second.kind == TypeKind::kRuntimeArray) {
                elem_id = it->second.element;
                continue;
            }
            break;
        }
        rb.count = count;
        auto elem_it = types.find(elem_id);
        const TypeKind elem_kind =
            elem_it != types.end() ? elem_it->second.kind : TypeKind::kOther;
        switch (elem_kind) {
            case TypeKind::kSampler:
                rb.type = VK_DESCRIPTOR_TYPE_SAMPLER;
                break;
            case TypeKind::kSampledImage: {
                rb.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                auto image_it = types.find(elem_it->second.element);
                if (image_it != types.end() && image_it->second.kind == TypeKind::kImage) {
                    rb.image_dim = image_it->second.image_dim;
                    rb.image_arrayed = image_it->second.image_arrayed;
                    rb.image_ms = image_it->second.image_ms;
                }
                break;
            }
            case TypeKind::kImage:
                rb.type = elem_it->second.image_sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                             : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                rb.image_dim = elem_it->second.image_dim;
                rb.image_arrayed = elem_it->second.image_arrayed;
                rb.image_ms = elem_it->second.image_ms;
                break;
            case TypeKind::kStruct:
                if (var.storage_class == kStorageStorageBuffer ||
                    (var.storage_class == kStorageUniform && dec_buffer_block[elem_id])) {
                    rb.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                } else if (var.storage_class == kStorageUniform) {
                    rb.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                } else {
                    continue;
                }
                break;
            default:
                continue;
        }
        (void)kStorageUniformConstant;
        out.push_back(rb);
    }
    return true;
}

// ---- .vk.xpso decoding ----------------------------------------------------
// Mirrors VulkanPipelineCache::PipelineDescription (pipeline_cache.h:164-233).
// Decoded manually with shifts to be independent of bitfield ABI.

// RenderPassKey (rex/graphics/vulkan/render_target_cache.h):
//   bits 0-1  msaa_samples (xenos::MsaaSamples, 1 << n samples)
//   bits 2-6  depth_and_color_used (bit 2 depth, bits 3-6 color RT 0-3)
//   bit  7    depth_format (xenos::DepthRenderTargetFormat)
//   bits 8-11/12-15/16-19/20-23  color 0-3 view format
//   bit  24   color_rts_use_transfer_formats
struct RenderPassKeyBits {
    uint32_t key = 0;
    uint32_t msaa_samples() const { return key & 0x3u; }
    uint32_t depth_and_color_used() const { return (key >> 2) & 0x1Fu; }
    bool depth_used() const { return (depth_and_color_used() & 0b1) != 0; }
    uint32_t color_rts_used() const { return depth_and_color_used() >> 1; }
    uint32_t depth_format() const { return (key >> 7) & 0x1u; }
    uint32_t color_view_format(uint32_t rt) const { return (key >> (8 + rt * 4)) & 0xFu; }
};

// The 45 bits of packed pipeline state (6 bytes on disk, bit 0 first).
struct PipelineStateBits {
    uint64_t bits = 0;
    uint32_t Get(uint32_t shift, uint32_t width) const {
        return uint32_t((bits >> shift) & ((1ull << width) - 1));
    }
    uint32_t geometry_shader() const { return Get(0, 2); }        // PipelineGeometryShader
    uint32_t primitive_topology() const { return Get(2, 3); }     // PipelinePrimitiveTopology
    uint32_t primitive_restart() const { return Get(5, 1); }
    uint32_t tessellation_mode() const { return Get(6, 2); }
    uint32_t depth_clamp_enable() const { return Get(8, 1); }
    uint32_t polygon_mode() const { return Get(9, 2); }           // PipelinePolygonMode
    uint32_t cull_front() const { return Get(11, 1); }
    uint32_t cull_back() const { return Get(12, 1); }
    uint32_t front_face_clockwise() const { return Get(13, 1); }
    uint32_t rasterizer_discard() const { return Get(14, 1); }
    uint32_t depth_write_enable() const { return Get(15, 1); }
    uint32_t depth_compare_op() const { return Get(16, 3); }      // xenos::CompareFunction
    uint32_t stencil_test_enable() const { return Get(19, 1); }
    uint32_t stencil_front_fail_op() const { return Get(20, 3); }  // xenos::StencilOp
    uint32_t stencil_front_pass_op() const { return Get(23, 3); }
    uint32_t stencil_front_depth_fail_op() const { return Get(26, 3); }
    uint32_t stencil_front_compare_op() const { return Get(29, 3); }
    uint32_t stencil_back_fail_op() const { return Get(32, 3); }
    uint32_t stencil_back_pass_op() const { return Get(35, 3); }
    uint32_t stencil_back_depth_fail_op() const { return Get(38, 3); }
    uint32_t stencil_back_compare_op() const { return Get(41, 3); }
    uint32_t sample_rate_shading() const { return Get(44, 1); }
};

// PipelineRenderTarget (26 bits packed in a u32).
struct RenderTargetBits {
    uint32_t bits = 0;
    uint32_t src_color_blend_factor() const { return bits & 0xFu; }         // PipelineBlendFactor
    uint32_t dst_color_blend_factor() const { return (bits >> 4) & 0xFu; }
    uint32_t color_blend_op() const { return (bits >> 8) & 0x7u; }          // xenos::BlendOp
    uint32_t src_alpha_blend_factor() const { return (bits >> 11) & 0xFu; }
    uint32_t dst_alpha_blend_factor() const { return (bits >> 15) & 0xFu; }
    uint32_t alpha_blend_op() const { return (bits >> 19) & 0x7u; }
    uint32_t color_write_mask() const { return (bits >> 22) & 0xFu; }
};

constexpr size_t kPipelineDescriptionSize = 58;
constexpr uint32_t kNativeShaderManifestFormatVersion = 2;
constexpr uint32_t kRequiredNativePipelineStoreVersion =
    rex::graphics::SpirvShaderTranslator::Modification::kVersion;
constexpr uint64_t kRequiredNativeShaderModificationMask =
    rex::graphics::SpirvShaderTranslator::Modification::
        kVfetchIndexRoundingBiasMask;
static_assert(kRequiredNativePipelineStoreVersion > 0x20260228);

constexpr uint32_t ByteSwap32(uint32_t value) {
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) |
           ((value & 0xFF000000u) >> 24);
}

struct StoredPipeline {
    uint64_t description_hash = 0;
    uint64_t vs_hash = 0;
    uint64_t vs_modification = 0;
    uint64_t ps_hash = 0;  // 0 = no pixel shader
    uint64_t ps_modification = 0;
    RenderPassKeyBits render_pass_key;
    PipelineStateBits state;
    RenderTargetBits render_targets[4];
    std::array<uint8_t, kPipelineDescriptionSize> serialized_description{};
    bool hash_ok = false;
};

constexpr uint64_t kAc6WaterVrsDescriptionHash =
    UINT64_C(0xdcf4cdd327387e16);
constexpr std::array<uint8_t, kPipelineDescriptionSize>
    kAc6WaterVrsSerializedDescription = {
        0x70, 0xf3, 0xd3, 0xda, 0xfa, 0x34, 0x2f, 0x04,
        0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x24, 0x16, 0x48, 0x8a, 0xb4, 0x0c, 0xa8, 0x35,
        0x3f, 0x00, 0x21, 0x00, 0x00, 0x40, 0x04, 0x00,
        0x8d, 0x00, 0x00, 0x00,
        0x0c, 0x90, 0x06, 0x00, 0x00, 0x00,
        0x76, 0xb0, 0xc3, 0x03,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
static_assert(kAc6WaterVrsSerializedDescription.size() ==
              kPipelineDescriptionSize);

bool IsExactAc6WaterVrsPipeline(const StoredPipeline& pipeline) {
    // Check both the hash-validated original bytes and decoded fields. The
    // latter prevents a generated topology variant that copied the original
    // byte array from inheriting the optimization after mutating state.
    return pipeline.hash_ok &&
           pipeline.description_hash == kAc6WaterVrsDescriptionHash &&
           pipeline.serialized_description ==
               kAc6WaterVrsSerializedDescription &&
           pipeline.vs_hash == UINT64_C(0x042f34fadad3f370) &&
           pipeline.vs_modification == UINT64_C(0x000400000000003f) &&
           pipeline.ps_hash == UINT64_C(0x35a80cb48a481624) &&
           pipeline.ps_modification == UINT64_C(0x000440000021003f) &&
           pipeline.render_pass_key.key == UINT32_C(0x0000008d) &&
           pipeline.state.bits == UINT64_C(0x00000006900c) &&
           pipeline.render_targets[0].bits == UINT32_C(0x03c3b076) &&
           pipeline.render_targets[1].bits == 0 &&
           pipeline.render_targets[2].bits == 0 &&
           pipeline.render_targets[3].bits == 0;
}

#if defined(VK_KHR_fragment_shading_rate)
bool HasFragmentShadingRate2x2Sample2(
    VkPhysicalDevice physical_device,
    PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR get_rates) {
    if (physical_device == VK_NULL_HANDLE || !get_rates) {
        return false;
    }
    // A rate list can grow between count and data calls. Retry a bounded
    // number of times, and fail closed unless a complete enumeration succeeds.
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        uint32_t rate_count = 0;
        VkResult result = get_rates(physical_device, &rate_count, nullptr);
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
            !rate_count) {
            return false;
        }
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(rate_count);
        for (auto& rate : rates) {
            rate.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        }
        result = get_rates(physical_device, &rate_count, rates.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }
        if (result != VK_SUCCESS || rate_count > rates.size()) {
            return false;
        }
        for (uint32_t rate_index = 0; rate_index < rate_count; ++rate_index) {
            const auto& rate = rates[rate_index];
            if (rate.fragmentSize.width == 2 &&
                rate.fragmentSize.height == 2 &&
                (rate.sampleCounts & VK_SAMPLE_COUNT_2_BIT)) {
                return true;
            }
        }
        return false;
    }
    return false;
}
#endif

bool DecodePipelineDescription(uint64_t expected_hash,
                               const uint8_t* description,
                               size_t description_size,
                               StoredPipeline& pipeline_out) {
    if (!description || description_size != kPipelineDescriptionSize ||
        XXH3_64bits(description, description_size) != expected_hash) {
        return false;
    }

    StoredPipeline pipeline;
    pipeline.description_hash = expected_hash;
    std::memcpy(&pipeline.vs_hash, description, 8);
    std::memcpy(&pipeline.vs_modification, description + 8, 8);
    std::memcpy(&pipeline.ps_hash, description + 16, 8);
    std::memcpy(&pipeline.ps_modification, description + 24, 8);
    std::memcpy(&pipeline.render_pass_key.key, description + 32, 4);
    uint64_t state_bits = 0;
    std::memcpy(&state_bits, description + 36, 6);
    pipeline.state.bits = state_bits;
    for (uint32_t rt = 0; rt < 4; ++rt) {
        std::memcpy(&pipeline.render_targets[rt].bits,
                    description + 42 + rt * 4, 4);
    }
    std::memcpy(pipeline.serialized_description.data(), description,
                description_size);
    pipeline.hash_ok = true;
    pipeline_out = std::move(pipeline);
    return true;
}

// PipelineGeometryShader / PipelinePrimitiveTopology / PipelinePolygonMode /
// PipelineBlendFactor values (VulkanPipelineCache, pipeline_cache.h:111-151).
enum : uint32_t {
    kGeometryShaderNone = 0,
    kTopologyPointList = 0,
    kTopologyLineList = 1,
    kTopologyLineStrip = 2,
    kTopologyTriangleList = 3,
    kTopologyTriangleStrip = 4,
    kTopologyTriangleFan = 5,
    kTopologyLineListWithAdjacency = 6,
    kTopologyPatchList = 7,
    kPolygonModeFill = 0,
    kPolygonModeLine = 1,
    kPolygonModePoint = 2,
    kBlendFactorOne = 1,   // PipelineBlendFactor::kOne
    kBlendFactorZero = 0,  // PipelineBlendFactor::kZero
};

uint32_t GetExpectedRenderTargetBits(
    rex::graphics::reg::RB_BLENDCONTROL blend_control, uint32_t write_mask) {
    // Mirrors VulkanPipelineCache::WritePipelineRenderTargetDescription.
    // RADV Vangogh exposes constantAlphaColorBlendFactors, so the canonical
    // one-to-one mapping used by the Deck pipeline store applies here.
    static constexpr uint32_t kBlendFactorMap[32] = {
        0, 1, 0, 0, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14,
    };
    uint32_t src_color = kBlendFactorOne;
    uint32_t dst_color = kBlendFactorZero;
    uint32_t color_op = 0;
    uint32_t src_alpha = kBlendFactorOne;
    uint32_t dst_alpha = kBlendFactorZero;
    uint32_t alpha_op = 0;
    if (write_mask) {
        src_color = kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
        dst_color = kBlendFactorMap[uint32_t(blend_control.color_destblend)];
        color_op = uint32_t(blend_control.color_comb_fcn);
        src_alpha = kBlendFactorMap[uint32_t(blend_control.alpha_srcblend)];
        dst_alpha = kBlendFactorMap[uint32_t(blend_control.alpha_destblend)];
        alpha_op = uint32_t(blend_control.alpha_comb_fcn);
    }
    return src_color | (dst_color << 4) | (color_op << 8) |
           (src_alpha << 11) | (dst_alpha << 15) | (alpha_op << 19) |
           ((write_mask & 0xFu) << 22);
}

// ---------------------------------------------------------------------------
// M3 slice 2 — mirror of SpirvShaderTranslator::SystemConstants
// (thirdparty/rexglue-sdk/include/rex/graphics/pipeline/shader/spirv_translator.h:215).
// Replicated locally (identical member order/types => identical layout)
// instead of including spirv_translator.h, which drags in the SPIR-V builder
// headers that are private to the SDK graphics library build.

struct XeSystemConstants {
    uint32_t flags;
    uint32_t vertex_index_load_address;
    uint32_t vertex_index_endian;  // xenos::Endian
    uint32_t line_loop_closing_index;
    int32_t vertex_base_index;
    uint32_t vertex_index_reset;
    uint32_t compute_memexport_vertex_count;
    float user_clip_planes[6][4];
    float ndc_scale[3];
    float point_vertex_diameter_min;
    float ndc_offset[3];
    float point_vertex_diameter_max;
    float point_constant_diameter[2];
    float point_screen_diameter_to_ndc_radius[2];
    uint32_t textures_resolution_scaled;
    uint32_t texture_swizzled_signs[8];
    uint32_t texture_swizzles[16];
    float alpha_test_reference;
    uint32_t alpha_to_mask;
    uint32_t edram_32bpp_tile_pitch_dwords_scaled;
    uint32_t edram_depth_base_dwords_scaled;
    float color_exp_bias[4];
    float edram_poly_offset_front_scale;
    float edram_poly_offset_back_scale;
    float edram_poly_offset_front_offset;
    float edram_poly_offset_back_offset;
    uint32_t edram_stencil_front_reference_masks;
    uint32_t edram_stencil_front_func_ops;
    uint32_t edram_stencil_back_reference_masks;
    uint32_t edram_stencil_back_func_ops;
    uint32_t edram_rt_base_dwords_scaled[4];
    uint32_t edram_rt_format_flags[4];
    uint32_t edram_rt_blend_factors_ops[4];
    uint32_t edram_rt_keep_mask[4][2];
    float edram_rt_clamp[4][4];
    float edram_blend_constant[4];
    uint32_t vertex_index_min;
    uint32_t vertex_index_max;
    float tessellation_factor_range_min;
    float tessellation_factor_range_max;
};

// One dword of padding difference would silently break every constant read -
// keep the mirror aligned with the original's size (verified against the
// SDK's struct: 7 + 24 + 8 + 4 + 1 + 8 + 16 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 +
// 16 + 4 + 4 dwords = 508 bytes... the static size is what matters for the
// descriptor range; layout is guaranteed by identical member sequence).
static_assert(sizeof(XeSystemConstants) % 16 == 0, "std140 vec4 multiple expected");

// SpirvShaderTranslator system-constant flag bits (spirv_translator.h:107).
enum : uint32_t {
    kXeSysFlag_ComputeOrPrimitiveVertexIndexLoad = 1u << 1,
    kXeSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit = 1u << 2,
    kXeSysFlag_XYDividedByW = 1u << 8,
    kXeSysFlag_ZDividedByW = 1u << 9,
    kXeSysFlag_WNotReciprocal = 1u << 10,
    kXeSysFlag_PrimitivePolygonal = 1u << 11,
    kXeSysFlag_PrimitiveLine = 1u << 12,
    kXeSysFlag_MsaaSamples_Shift = 13,  // 2 bits (xenos::kMsaaSamplesBits)
    kXeSysFlag_DepthFloat24 = 1u << 15,
    kXeSysFlag_AlphaPassIfLess_Shift = 16,  // 3-bit xenos::CompareFunction
    kXeSysFlag_ConvertColor0ToGamma_Shift = 19,
};

// SpirvShaderTranslator::ConstantBuffer order == set-1 binding indices.
enum : uint32_t {
    kXeConstantBufferSystem = 0,
    kXeConstantBufferFloatVertex = 1,
    kXeConstantBufferFloatPixel = 2,
    kXeConstantBufferBoolLoop = 3,
    kXeConstantBufferFetch = 4,
    kXeConstantBufferCount = 5,
};

// Loaded offline shader translation.
struct LoadedShader {
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<ReflectedBinding> bindings;
};

// A created native pipeline plus everything needed to bind and draw with it
// (M3 slice 2). Private-mode layout/set-layout handles are owned by the prover
// caches; a forced shared-mode pipeline layout is borrowed from the SDK.
struct NativePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // Non-owning. In shared-authority mode these are generated and retained
    // by the SDK VulkanPipelineCache and are recorded only to enforce stable
    // same-description reuse. Vulkan permits the SDK to destroy the modules
    // after pipeline creation, though in practice it retains them until
    // VulkanPipelineCache shutdown.
    VkShaderModule borrowed_vertex_shader = VK_NULL_HANDLE;
    VkShaderModule borrowed_pixel_shader = VK_NULL_HANDLE;
    VkShaderModule borrowed_geometry_shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layouts[4] = {};
    StoredPipeline desc;
    const LoadedShader* vs = nullptr;
    const LoadedShader* ps = nullptr;
    bool ac6_water_vrs_2x2 = false;
};

// Default-off N=1 cache of the last successfully selected shared pipeline.
// The non-owning NativePipeline pointer is stable across unordered_map rehash
// and is invalidated before the shared pipeline map is cleared in Destroy.
struct SharedPipelinePlanCacheEntry {
    bool valid = false;
    uint64_t description_hash = 0;
    std::array<uint8_t, kPipelineDescriptionSize> serialized_description{};
    uint32_t host_vertex_shader_type = 0;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule pixel_shader = VK_NULL_HANDLE;
    VkShaderModule geometry_shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const NativePipeline* native_pipeline = nullptr;
};

struct SharedPipelinePlanCacheStats {
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t miss_no_entry = 0;
    uint64_t miss_hash = 0;
    uint64_t miss_invalid_payload = 0;
    uint64_t miss_description = 0;
    uint64_t miss_host_type = 0;
    uint64_t miss_layout = 0;
    uint64_t miss_vertex_shader = 0;
    uint64_t miss_pixel_shader = 0;
    uint64_t miss_geometry_shader = 0;
    uint64_t miss_stale_pipeline = 0;
    uint64_t slow_successes = 0;
    uint64_t stores = 0;
    uint64_t store_reject_invalid_payload = 0;
    uint64_t store_reject_inconsistent = 0;
    uint64_t fast_water_vrs_selections = 0;
};

// The whole prover. Lives on the render thread; owns a private headless
// Vulkan context (instance + physical device + logical device + one graphics
// queue, no surface) and everything created from it.
class PipelineProver {
 public:
    ~PipelineProver() { Destroy(); }

    void SetGpuResolveSpirv(std::vector<uint32_t> spirv) {
        gpu_resolve_spirv_ = std::move(spirv);
    }

    void Run() {
        reported_texture_fallbacks_.clear();
        if (!ValidateShaderCorpusManifest()) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private native renderer rejected its shader corpus; "
                "shared-authority mode is unaffected");
            return;
        }
        if (!LoadLoader() || !CreateInstance() || !PickPhysicalDevice() || !CreateDevice()) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: headless Vulkan context creation FAILED - aborting "
                                      "pipeline prove");
            return;
        }
        if (queue_supports_compute_ && !gpu_resolve_spirv_.empty() &&
            !CreateGpuResolvePipeline()) {
            REXLOG_CAT_WARN(
                kLogGPU,
                "nr2: direct GPU resolve setup failed; retaining CPU "
                "readback/retile fallback");
        }
        if (!LoadShaderModules()) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private shader corpus failed exact file validation");
            return;
        }
        ProvePipelines();
    }

    // Pipeline-cache-only mode borrowing the authoritative emulator Vulkan
    // device. The caller owns the device, instance, queues, and pipeline
    // layouts; nr2 owns only the shader modules and graphics pipelines it
    // creates through this mode.
    bool RunSharedDevice(rex::ui::vulkan::VulkanDevice* vulkan_device) {
        if (!vulkan_device || device_ != VK_NULL_HANDLE ||
            instance_ != VK_NULL_HANDLE || shared_vulkan_device_) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: shared-device pipeline cache initialization "
                             "received an invalid or already-used prover");
            return false;
        }
        const rex::ui::vulkan::VulkanInstance* vulkan_instance =
            vulkan_device->vulkan_instance();
        if (!vulkan_instance || vulkan_device->device() == VK_NULL_HANDLE ||
            vulkan_device->physical_device() == VK_NULL_HANDLE) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: authoritative Vulkan device is incomplete");
            return false;
        }
        const auto& device_functions = vulkan_device->functions();
        const auto& instance_functions = vulkan_instance->functions();
        if (!device_functions.vkCreateShaderModule ||
            !device_functions.vkDestroyShaderModule ||
            !device_functions.vkCreateGraphicsPipelines ||
            !device_functions.vkDestroyPipeline ||
            !instance_functions.vkGetPhysicalDeviceFormatProperties) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: authoritative Vulkan function tables are incomplete");
            return false;
        }
        const auto& properties = vulkan_device->properties();
        if (properties.apiVersion < VK_API_VERSION_1_2 ||
            !properties.dynamicRendering) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: authoritative Vulkan device lacks SPIR-V 1.5 or "
                "dynamic-rendering support");
            return false;
        }

        shared_vulkan_device_ = vulkan_device;
        instance_ = vulkan_instance->instance();
        physical_device_ = vulkan_device->physical_device();
        device_ = vulkan_device->device();
        fns_.CreateShaderModule = device_functions.vkCreateShaderModule;
        fns_.DestroyShaderModule = device_functions.vkDestroyShaderModule;
        fns_.CreateGraphicsPipelines =
            device_functions.vkCreateGraphicsPipelines;
        fns_.DestroyPipeline = device_functions.vkDestroyPipeline;

        framebuffer_color_sample_counts_ =
            properties.framebufferColorSampleCounts;
        framebuffer_depth_sample_counts_ =
            properties.framebufferDepthSampleCounts;
        max_viewport_dimensions_[0] = properties.maxViewportDimensions[0];
        max_viewport_dimensions_[1] = properties.maxViewportDimensions[1];
        min_uniform_buffer_offset_alignment_ = std::max<VkDeviceSize>(
            properties.minUniformBufferOffsetAlignment, 16);
        depth_clamp_enabled_ = properties.depthClamp;
        fill_mode_non_solid_enabled_ = properties.fillModeNonSolid;

        auto optimal_features = [&](VkFormat format) {
            VkFormatProperties format_properties = {};
            instance_functions.vkGetPhysicalDeviceFormatProperties(
                physical_device_, format, &format_properties);
            return format_properties.optimalTilingFeatures;
        };
        constexpr VkFormatFeatureFlags kDepthNeeded =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        constexpr VkFormatFeatureFlags kColorNeeded =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        depth_d24s8_supported_ =
            (optimal_features(VK_FORMAT_D24_UNORM_S8_UINT) & kDepthNeeded) ==
            kDepthNeeded;
        rg16_snorm_supported_ =
            (optimal_features(VK_FORMAT_R16G16_SNORM) & kColorNeeded) ==
            kColorNeeded;
        rgba16_snorm_supported_ =
            (optimal_features(VK_FORMAT_R16G16B16A16_SNORM) &
             kColorNeeded) == kColorNeeded;
        const bool rg16_sfloat_supported =
            (optimal_features(VK_FORMAT_R16G16_SFLOAT) & kColorNeeded) ==
            kColorNeeded;
        const bool rgba16_sfloat_supported =
            (optimal_features(VK_FORMAT_R16G16B16A16_SFLOAT) &
             kColorNeeded) == kColorNeeded;
        const bool rgba16_unorm_supported =
            (optimal_features(VK_FORMAT_R16G16B16A16_UNORM) & kColorNeeded) ==
            kColorNeeded;
        const bool gamma_as_unorm16_requested =
            REXCVAR_GET(gamma_render_target_as_unorm16);
        gamma_as_unorm16_ =
            gamma_as_unorm16_requested && rgba16_unorm_supported;
        if ((!rg16_snorm_supported_ && !rg16_sfloat_supported) ||
            (!rgba16_snorm_supported_ && !rgba16_sfloat_supported) ||
            (gamma_as_unorm16_requested && !rgba16_unorm_supported)) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: authoritative host-render-target format fallbacks are "
                "not available on the shared Vulkan device");
            Destroy();
            return false;
        }
        shared_msaa_2x_attachments_supported_ =
            REXCVAR_GET(native_2x_msaa) &&
            (properties.framebufferColorSampleCounts &
             properties.framebufferDepthSampleCounts &
             properties.framebufferStencilSampleCounts &
             properties.sampledImageColorSampleCounts &
             properties.sampledImageDepthSampleCounts &
             properties.sampledImageStencilSampleCounts &
             VK_SAMPLE_COUNT_2_BIT) != 0;

        shared_fragment_shading_rate_2x2_sample_2_enabled_ =
            properties.pipelineFragmentShadingRate &&
            properties.fragmentShadingRate2x2Sample2;
        if (REXCVAR_GET(ac6_native_water_vrs)) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: exact water 2x2 VRS requested; shared logical-device "
                "2x2/sample2 capability {}",
                shared_fragment_shading_rate_2x2_sample_2_enabled_
                    ? "ENABLED"
                    : "unavailable (standard shading fallback)");
        }

        reported_texture_fallbacks_.clear();
        // Shared-authority pipelines borrow the exact live SDK shader
        // modules. The offline corpus remains private-device/trace-tool only;
        // loading it here would reintroduce a translator-ABI dependency and
        // needlessly duplicate every guest shader module on the live device.
        ResetSharedPipelinePlanCache(
            SharedPipelinePlanCacheRequested());
        stored_pipelines_.clear();
        shared_pipeline_rejections_.clear();
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: shared-device pipeline cache ready on '{}' (exact SDK "
            "shader modules; descriptions and pipelines accepted lazily)",
            properties.deviceName);
        return true;
    }

    bool GetOrCreateSharedPipeline(uint64_t description_hash,
                                   const uint8_t* serialized_description,
                                   size_t serialized_description_size,
                                   uint32_t expected_host_vertex_shader_type,
                                   VkPipelineLayout authoritative_layout,
                                   VkShaderModule authoritative_vertex_shader,
                                   VkShaderModule authoritative_pixel_shader,
                                   VkShaderModule generated_geometry_shader,
                                   VkPipeline& pipeline_out) {
        pipeline_out = VK_NULL_HANDLE;
        if (!is_shared_device() || authoritative_layout == VK_NULL_HANDLE) {
            return false;
        }
        if (shared_pipeline_rejections_.find(description_hash) !=
            shared_pipeline_rejections_.end()) {
            return false;
        }
        if (const NativePipeline* cached_pipeline =
                FindSharedPipelinePlanCache(
                    description_hash, serialized_description,
                    serialized_description_size,
                    expected_host_vertex_shader_type, authoritative_layout,
                    authoritative_vertex_shader, authoritative_pixel_shader,
                    generated_geometry_shader)) {
            pipeline_out = cached_pipeline->pipeline;
            if (cached_pipeline->ac6_water_vrs_2x2) {
                ++shared_pipeline_plan_cache_stats_
                      .fast_water_vrs_selections;
            }
            // A cached accepted draw is still a shared pipeline selection.
            // Preserve exact-water VRS accounting on every fast return.
            RecordSharedWaterVrsSelection(*cached_pipeline);
            return true;
        }
        StoredPipeline exported_description;
        if (!DecodePipelineDescription(
                description_hash, serialized_description,
                serialized_description_size, exported_description)) {
            shared_pipeline_rejections_.insert(description_hash);
            REXLOG_CAT_WARN(
                kLogGPU,
                "nr2: shared pipeline description {:016x} failed exact "
                "58-byte hash validation",
                description_hash);
            return false;
        }

        const uint32_t encoded_host_vertex_shader_type =
            uint32_t(exported_description.vs_modification >> 32) & 0xFu;
        const bool encoded_host_type_supported =
            encoded_host_vertex_shader_type == 0 ||
            encoded_host_vertex_shader_type == 9 ||
            encoded_host_vertex_shader_type == 10;
        const bool expanded_vertex_state_valid =
            encoded_host_vertex_shader_type == 0 ||
            (exported_description.state.geometry_shader() ==
                 kGeometryShaderNone &&
             exported_description.state.primitive_topology() ==
                 kTopologyTriangleStrip &&
             exported_description.state.primitive_restart() &&
             generated_geometry_shader == VK_NULL_HANDLE);
        const bool live_shader_modules_valid =
            authoritative_vertex_shader != VK_NULL_HANDLE &&
            exported_description.ps_hash != 0 &&
            authoritative_pixel_shader != VK_NULL_HANDLE;
        if (!encoded_host_type_supported ||
            encoded_host_vertex_shader_type !=
                expected_host_vertex_shader_type ||
            !expanded_vertex_state_valid) {
            shared_pipeline_rejections_.insert(description_hash);
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected shared pipeline {:016x}: request host type {} "
                "does not match a supported exact description host type {}",
                description_hash, expected_host_vertex_shader_type,
                encoded_host_vertex_shader_type);
            return false;
        }
        if (!live_shader_modules_valid) {
            shared_pipeline_rejections_.insert(description_hash);
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected shared pipeline {:016x}: invalid live shader "
                "module set (VS {}, PS {}, exact state PS {})",
                description_hash,
                authoritative_vertex_shader != VK_NULL_HANDLE ? "present" :
                                                                  "missing",
                authoritative_pixel_shader != VK_NULL_HANDLE ? "present" :
                                                               "missing",
                exported_description.ps_hash ? "required" :
                                               "unsupported depth-only");
            return false;
        }

        auto description_it = stored_pipelines_.find(description_hash);
        if (description_it == stored_pipelines_.end()) {
            description_it =
                stored_pipelines_
                    .emplace(description_hash, std::move(exported_description))
                    .first;
        } else if (std::memcmp(
                       description_it->second.serialized_description.data(),
                       serialized_description, kPipelineDescriptionSize) != 0) {
            // A hash match alone is not sufficient at this trust boundary.
            // Never reuse a pipeline for different exact state bytes.
            shared_pipeline_rejections_.insert(description_hash);
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected a shared pipeline description hash collision "
                "for {:016x}",
                description_hash);
            return false;
        }
        const bool description_requires_geometry_shader =
            description_it->second.state.geometry_shader() !=
            kGeometryShaderNone;
        if (description_requires_geometry_shader !=
            (generated_geometry_shader != VK_NULL_HANDLE)) {
            shared_pipeline_rejections_.insert(description_hash);
            const char* state_requirement =
                description_requires_geometry_shader ? "requires" :
                                                       "does not require";
            const char* module_presence =
                generated_geometry_shader != VK_NULL_HANDLE ? "present" :
                                                              "missing";
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected shared pipeline {:016x}: exact state {} a "
                "generated geometry shader but the borrowed SDK module is {}",
                description_hash, state_requirement, module_presence);
            return false;
        }
        auto pipeline_it = pipelines_.find(description_hash);
        if (pipeline_it != pipelines_.end()) {
            if (pipeline_it->second.layout != authoritative_layout) {
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: shared pipeline {:016x} was requested with a "
                    "different authoritative layout",
                    description_hash);
                return false;
            }
            if (pipeline_it->second.borrowed_vertex_shader !=
                    authoritative_vertex_shader ||
                pipeline_it->second.borrowed_pixel_shader !=
                    authoritative_pixel_shader) {
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: shared pipeline {:016x} was requested with "
                    "different borrowed guest-shader modules",
                    description_hash);
                return false;
            }
            if (pipeline_it->second.borrowed_geometry_shader !=
                generated_geometry_shader) {
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: shared pipeline {:016x} was requested with a "
                    "different borrowed geometry-shader module",
                    description_hash);
                return false;
            }
            pipeline_out = pipeline_it->second.pipeline;
            if (pipeline_out != VK_NULL_HANDLE) {
                RecordSharedWaterVrsSelection(pipeline_it->second);
                StoreSharedPipelinePlanCache(
                    description_hash, serialized_description,
                    serialized_description_size,
                    expected_host_vertex_shader_type, authoritative_layout,
                    authoritative_vertex_shader, authoritative_pixel_shader,
                    generated_geometry_shader, pipeline_it->second,
                    pipeline_out);
            }
            return pipeline_out != VK_NULL_HANDLE;
        }
        std::string fail_reason;
        if (!CreatePipeline(description_it->second, fail_reason, nullptr,
                            authoritative_layout, authoritative_vertex_shader,
                            authoritative_pixel_shader,
                            generated_geometry_shader)) {
            shared_pipeline_rejections_.insert(description_hash);
            REXLOG_CAT_WARN(
                kLogGPU,
                "nr2: shared pipeline {:016x} creation failed ({})",
                description_hash, fail_reason);
            return false;
        }
        pipeline_it = pipelines_.find(description_hash);
        if (pipeline_it == pipelines_.end() ||
            pipeline_it->second.layout != authoritative_layout) {
            return false;
        }
        pipeline_out = pipeline_it->second.pipeline;
        if (pipeline_out != VK_NULL_HANDLE) {
            RecordSharedWaterVrsSelection(pipeline_it->second);
            StoreSharedPipelinePlanCache(
                description_hash, serialized_description,
                serialized_description_size,
                expected_host_vertex_shader_type, authoritative_layout,
                authoritative_vertex_shader, authoritative_pixel_shader,
                generated_geometry_shader, pipeline_it->second, pipeline_out);
        }
        return pipeline_out != VK_NULL_HANDLE;
    }

    bool is_shared_device() const { return shared_vulkan_device_ != nullptr; }

    void Destroy() {
        if (!is_shared_device() &&
            (water_vrs_pipeline_count_ || private_water_vrs_draw_count_)) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private exact water 2x2 VRS totals: {} accepted "
                "pipeline(s), {} draw(s)",
                water_vrs_pipeline_count_, private_water_vrs_draw_count_);
        }

        if (is_shared_device()) {
            ReportSharedPipelinePlanCache();
            // Invalidate the non-owning NativePipeline pointer before any
            // shared pipeline can be destroyed or removed from its map.
            ResetSharedPipelinePlanCache(false);
            if (water_vrs_pipeline_count_ ||
                shared_water_vrs_selection_count_) {
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: exact water 2x2 VRS totals: {} accepted pipeline(s), "
                    "{} shared selection(s)",
                    water_vrs_pipeline_count_,
                    shared_water_vrs_selection_count_);
            }

            // The authoritative device and pipeline layouts are owned by the
            // SDK. Only release objects created by nr2, without waiting on or
            // destroying the shared device.
            for (auto& [key, native_pipeline] : pipelines_) {
                if (native_pipeline.pipeline != VK_NULL_HANDLE) {
                    fns_.DestroyPipeline(device_, native_pipeline.pipeline,
                                         nullptr);
                }
                // All borrowed shader modules are owned by the SDK pipeline
                // cache and are deliberately not destroyed here.
            }
            pipelines_.clear();
            for (auto& [key, native_pipeline] : variant_pipelines_) {
                if (native_pipeline.pipeline != VK_NULL_HANDLE) {
                    fns_.DestroyPipeline(device_, native_pipeline.pipeline,
                                         nullptr);
                }
            }
            variant_pipelines_.clear();
            for (auto& [key, shader] : shaders_) {
                if (shader.module != VK_NULL_HANDLE) {
                    fns_.DestroyShaderModule(device_, shader.module, nullptr);
                }
            }
            shaders_.clear();
            stored_pipelines_.clear();
            shared_pipeline_rejections_.clear();
            shared_vulkan_device_ = nullptr;
            instance_ = VK_NULL_HANDLE;
            physical_device_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            shared_msaa_2x_attachments_supported_ = false;
            shared_fragment_shading_rate_2x2_sample_2_enabled_ = false;
            water_vrs_pipeline_count_ = 0;
            shared_water_vrs_selection_count_ = 0;
            private_water_vrs_draw_count_ = 0;
            water_vrs_fallback_reported_ = false;
            fns_ = {};
            return;
        }
        if (device_ != VK_NULL_HANDLE) {
            if (!session_failed_) {
                SubmitDeferredDrawCommandBuffers();
            }
            if (fns_.DeviceWaitIdle) {
                fns_.DeviceWaitIdle(device_);
            }
            ReleaseDeferredDrawResources();
            DestroyDeferredBatchPools();
            DestroyHostBuffer(deferred_uniform_buffer_);
            DestroyGuestTextureCache();
            DestroyPersistentTarget();
            DestroyResolveSubmissionResources();
            DestroyGpuResolvePipeline();
            DestroyHostBuffer(resolve_readback_buffer_);
            DestroyHostBuffer(resolve_overlay_readback_buffer_);
            DestroyHostBuffer(gpu_resolve_shadow_buffer_);
            DestroyGuestMemoryBuffer();
            for (auto& [key, sampler] : guest_sampler_cache_) {
                fns_.DestroySampler(device_, sampler, nullptr);
            }
            guest_sampler_cache_.clear();
            for (auto& [key, native_pipeline] : pipelines_) {
                fns_.DestroyPipeline(device_, native_pipeline.pipeline, nullptr);
            }
            pipelines_.clear();
            for (auto& [key, native_pipeline] : variant_pipelines_) {
                fns_.DestroyPipeline(device_, native_pipeline.pipeline, nullptr);
            }
            variant_pipelines_.clear();
            for (auto& [key, layout] : pipeline_layouts_) {
                fns_.DestroyPipelineLayout(device_, layout, nullptr);
            }
            pipeline_layouts_.clear();
            for (auto& [key, layout] : set_layouts_) {
                fns_.DestroyDescriptorSetLayout(device_, layout, nullptr);
            }
            set_layouts_.clear();
            for (auto& [key, shader] : shaders_) {
                fns_.DestroyShaderModule(device_, shader.module, nullptr);
            }
            shaders_.clear();
            fns_.DestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            fns_.DestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
#if !defined(_WIN32)
        if (fns_.library) {
            dlclose(fns_.library);
            fns_.library = nullptr;
        }
#endif
#if defined(VK_KHR_fragment_shading_rate)
        private_fragment_shading_rate_extension_supported_ = false;
        private_fragment_shading_rate_properties_2x2_sample_2_supported_ =
            false;
        private_fragment_shading_rate_2x2_sample_2_supported_ = false;
        private_fragment_shading_rate_2x2_sample_2_enabled_ = false;
#endif
        water_vrs_pipeline_count_ = 0;
        shared_water_vrs_selection_count_ = 0;
        private_water_vrs_draw_count_ = 0;
        water_vrs_fallback_reported_ = false;
        ResetSharedPipelinePlanCache(false);
    }

 private:
    bool LoadLoader() {
#if defined(_WIN32)
        return false;  // Linux-only slice.
#else
        fns_.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!fns_.library) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: dlopen(libvulkan.so.1) failed");
            return false;
        }
        fns_.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(fns_.library, "vkGetInstanceProcAddr"));
        if (!fns_.GetInstanceProcAddr) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: vkGetInstanceProcAddr not found");
            return false;
        }
        fns_.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            fns_.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
        return fns_.CreateInstance != nullptr;
#endif
    }

    template <typename T>
    bool LoadFn(T& fn, const char* name) {
        fn = reinterpret_cast<T>(fns_.GetInstanceProcAddr(instance_, name));
        if (!fn) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: failed to load {}", name);
            return false;
        }
        return true;
    }

    bool CreateInstance() {
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "ac6_native_renderer2";
        app_info.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        // Headless: no surface/extensions/layers.
        VkResult result = fns_.CreateInstance(&create_info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: vkCreateInstance failed ({})", int32_t(result));
            return false;
        }
        return LoadFn(fns_.DestroyInstance, "vkDestroyInstance") &&
               LoadFn(fns_.EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices") &&
               LoadFn(fns_.GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") &&
               LoadFn(fns_.GetPhysicalDeviceQueueFamilyProperties,
                      "vkGetPhysicalDeviceQueueFamilyProperties") &&
               LoadFn(fns_.GetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2") &&
               LoadFn(fns_.GetPhysicalDeviceFormatProperties,
                      "vkGetPhysicalDeviceFormatProperties") &&
               LoadFn(fns_.CreateDevice, "vkCreateDevice") &&
               LoadFn(fns_.DestroyDevice, "vkDestroyDevice") &&
               LoadFn(fns_.GetDeviceQueue, "vkGetDeviceQueue") &&
               LoadFn(fns_.CreateShaderModule, "vkCreateShaderModule") &&
               LoadFn(fns_.DestroyShaderModule, "vkDestroyShaderModule") &&
               LoadFn(fns_.CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") &&
               LoadFn(fns_.DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") &&
               LoadFn(fns_.CreatePipelineLayout, "vkCreatePipelineLayout") &&
               LoadFn(fns_.DestroyPipelineLayout, "vkDestroyPipelineLayout") &&
               LoadFn(fns_.CreateGraphicsPipelines, "vkCreateGraphicsPipelines") &&
               LoadFn(fns_.CreateComputePipelines, "vkCreateComputePipelines") &&
               LoadFn(fns_.DestroyPipeline, "vkDestroyPipeline") &&
               LoadFn(fns_.GetDeviceProcAddr, "vkGetDeviceProcAddr") &&
               LoadFn(fns_.EnumerateDeviceExtensionProperties,
                      "vkEnumerateDeviceExtensionProperties") &&
               LoadFn(fns_.GetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2") &&
               LoadFn(fns_.GetPhysicalDeviceMemoryProperties,
                      "vkGetPhysicalDeviceMemoryProperties") &&
               LoadFn(fns_.DeviceWaitIdle, "vkDeviceWaitIdle") &&
               LoadFn(fns_.CreateBuffer, "vkCreateBuffer") &&
               LoadFn(fns_.DestroyBuffer, "vkDestroyBuffer") &&
               LoadFn(fns_.GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
               LoadFn(fns_.BindBufferMemory, "vkBindBufferMemory") &&
               LoadFn(fns_.AllocateMemory, "vkAllocateMemory") &&
               LoadFn(fns_.FreeMemory, "vkFreeMemory") &&
               LoadFn(fns_.MapMemory, "vkMapMemory") &&
               LoadFn(fns_.UnmapMemory, "vkUnmapMemory") &&
               LoadFn(fns_.CreateImage, "vkCreateImage") &&
               LoadFn(fns_.DestroyImage, "vkDestroyImage") &&
               LoadFn(fns_.GetImageMemoryRequirements, "vkGetImageMemoryRequirements") &&
               LoadFn(fns_.BindImageMemory, "vkBindImageMemory") &&
               LoadFn(fns_.CreateImageView, "vkCreateImageView") &&
               LoadFn(fns_.DestroyImageView, "vkDestroyImageView") &&
               LoadFn(fns_.CreateSampler, "vkCreateSampler") &&
               LoadFn(fns_.DestroySampler, "vkDestroySampler") &&
               LoadFn(fns_.CreateCommandPool, "vkCreateCommandPool") &&
               LoadFn(fns_.DestroyCommandPool, "vkDestroyCommandPool") &&
               LoadFn(fns_.ResetCommandPool, "vkResetCommandPool") &&
               LoadFn(fns_.AllocateCommandBuffers, "vkAllocateCommandBuffers") &&
               LoadFn(fns_.BeginCommandBuffer, "vkBeginCommandBuffer") &&
               LoadFn(fns_.EndCommandBuffer, "vkEndCommandBuffer") &&
               LoadFn(fns_.QueueSubmit, "vkQueueSubmit") &&
               LoadFn(fns_.CreateFence, "vkCreateFence") &&
               LoadFn(fns_.DestroyFence, "vkDestroyFence") &&
               LoadFn(fns_.WaitForFences, "vkWaitForFences") &&
               LoadFn(fns_.ResetFences, "vkResetFences") &&
               LoadFn(fns_.CreateDescriptorPool, "vkCreateDescriptorPool") &&
               LoadFn(fns_.DestroyDescriptorPool, "vkDestroyDescriptorPool") &&
               LoadFn(fns_.AllocateDescriptorSets, "vkAllocateDescriptorSets") &&
               LoadFn(fns_.UpdateDescriptorSets, "vkUpdateDescriptorSets") &&
               LoadFn(fns_.CmdPipelineBarrier, "vkCmdPipelineBarrier") &&
               LoadFn(fns_.CmdClearColorImage, "vkCmdClearColorImage") &&
               LoadFn(fns_.CmdBeginRendering, "vkCmdBeginRendering") &&
               LoadFn(fns_.CmdEndRendering, "vkCmdEndRendering") &&
               LoadFn(fns_.CmdBindPipeline, "vkCmdBindPipeline") &&
               LoadFn(fns_.CmdBindDescriptorSets, "vkCmdBindDescriptorSets") &&
               LoadFn(fns_.CmdPushConstants, "vkCmdPushConstants") &&
               LoadFn(fns_.CmdDispatch, "vkCmdDispatch") &&
               LoadFn(fns_.CmdBindIndexBuffer, "vkCmdBindIndexBuffer") &&
               LoadFn(fns_.CmdSetViewport, "vkCmdSetViewport") &&
               LoadFn(fns_.CmdSetScissor, "vkCmdSetScissor") &&
               LoadFn(fns_.CmdSetDepthBias, "vkCmdSetDepthBias") &&
               LoadFn(fns_.CmdSetBlendConstants, "vkCmdSetBlendConstants") &&
               LoadFn(fns_.CmdSetStencilCompareMask, "vkCmdSetStencilCompareMask") &&
               LoadFn(fns_.CmdSetStencilWriteMask, "vkCmdSetStencilWriteMask") &&
               LoadFn(fns_.CmdSetStencilReference, "vkCmdSetStencilReference") &&
               LoadFn(fns_.CmdDraw, "vkCmdDraw") &&
               LoadFn(fns_.CmdDrawIndexed, "vkCmdDrawIndexed") &&
               LoadFn(fns_.CmdCopyImage, "vkCmdCopyImage") &&
               LoadFn(fns_.CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") &&
               LoadFn(fns_.CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    }

    bool PickPhysicalDevice() {
        uint32_t count = 0;
        fns_.EnumeratePhysicalDevices(instance_, &count, nullptr);
        if (!count) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: no Vulkan physical devices");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        fns_.EnumeratePhysicalDevices(instance_, &count, devices.data());
        physical_device_ = devices[0];
        VkPhysicalDeviceProperties props;
        fns_.GetPhysicalDeviceProperties(physical_device_, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) {
            // SPIR-V 1.5 blobs (Features(true) in the M1 conversion) need 1.2+.
            REXLOG_CAT_ERROR(kLogGPU, "nr2: device API {}.{} too old for SPIR-V 1.5",
                             VK_API_VERSION_MAJOR(props.apiVersion),
                             VK_API_VERSION_MINOR(props.apiVersion));
            return false;
        }
        framebuffer_color_sample_counts_ = props.limits.framebufferColorSampleCounts;
        framebuffer_depth_sample_counts_ = props.limits.framebufferDepthSampleCounts;
        max_viewport_dimensions_[0] = props.limits.maxViewportDimensions[0];
        max_viewport_dimensions_[1] = props.limits.maxViewportDimensions[1];
        min_uniform_buffer_offset_alignment_ =
            std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
        // Device extensions and their properties. Enumeration is accepted only
        // after a complete data call; a changing list is retried, bounded.
        {
            external_memory_host_supported_ = false;
#if defined(VK_KHR_fragment_shading_rate)
            private_fragment_shading_rate_extension_supported_ = false;
            private_fragment_shading_rate_properties_2x2_sample_2_supported_ =
                false;
            private_fragment_shading_rate_2x2_sample_2_supported_ = false;
#endif
            std::vector<VkExtensionProperties> extensions;
            bool extensions_enumerated = false;
            for (uint32_t attempt = 0; attempt < 4; ++attempt) {
                uint32_t ext_count = 0;
                VkResult result = fns_.EnumerateDeviceExtensionProperties(
                    physical_device_, nullptr, &ext_count, nullptr);
                if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                    break;
                }
                if (!ext_count) {
                    if (result == VK_SUCCESS) {
                        extensions.clear();
                        extensions_enumerated = true;
                        break;
                    }
                    continue;
                }
                extensions.resize(ext_count);
                result = fns_.EnumerateDeviceExtensionProperties(
                    physical_device_, nullptr, &ext_count, extensions.data());
                if (result == VK_INCOMPLETE) {
                    continue;
                }
                if (result != VK_SUCCESS || ext_count > extensions.size()) {
                    break;
                }
                extensions.resize(ext_count);
                extensions_enumerated = true;
                break;
            }
            if (!extensions_enumerated) {
                extensions.clear();
                REXLOG_CAT_WARN(
                    kLogGPU,
                    "nr2: device extension enumeration incomplete; optional "
                    "private-device extensions disabled");
            } else {
                for (const VkExtensionProperties& ext : extensions) {
                    if (!std::strcmp(
                            ext.extensionName,
                            "VK_EXT_external_memory_host")) {
                        external_memory_host_supported_ = true;
                    }
#if defined(VK_KHR_fragment_shading_rate)
                    if (!std::strcmp(
                            ext.extensionName,
                            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
                        private_fragment_shading_rate_extension_supported_ =
                            true;
                    }
#endif
                }
            }

            VkPhysicalDeviceProperties2 props2 = {};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {};
            if (external_memory_host_supported_) {
                host_props.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
                host_props.pNext = props2.pNext;
                props2.pNext = &host_props;
            }
#if defined(VK_KHR_fragment_shading_rate)
            VkPhysicalDeviceFragmentShadingRatePropertiesKHR
                fragment_shading_rate_props = {};
            if (REXCVAR_GET(ac6_native_water_vrs) &&
                private_fragment_shading_rate_extension_supported_) {
                fragment_shading_rate_props.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
                fragment_shading_rate_props.pNext = props2.pNext;
                props2.pNext = &fragment_shading_rate_props;
            }
#endif
            if (props2.pNext) {
                fns_.GetPhysicalDeviceProperties2(physical_device_, &props2);
            }
            if (external_memory_host_supported_) {
                min_imported_host_pointer_alignment_ =
                    host_props.minImportedHostPointerAlignment;
            }
#if defined(VK_KHR_fragment_shading_rate)
            if (REXCVAR_GET(ac6_native_water_vrs) &&
                private_fragment_shading_rate_extension_supported_) {
                // Both sample properties are maxima, so 2 must not exceed
                // either one.
                private_fragment_shading_rate_properties_2x2_sample_2_supported_ =
                    fragment_shading_rate_props.maxFragmentSize.width >= 2 &&
                    fragment_shading_rate_props.maxFragmentSize.height >= 2 &&
                    fragment_shading_rate_props
                            .maxFragmentShadingRateRasterizationSamples >=
                        VK_SAMPLE_COUNT_2_BIT &&
                    fragment_shading_rate_props
                            .maxFragmentShadingRateCoverageSamples >=
                        8;
                const auto get_fragment_shading_rates =
                    reinterpret_cast<
                        PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
                        fns_.GetInstanceProcAddr(
                            instance_,
                            "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
                const bool enumerated_2x2_sample_2 =
                    HasFragmentShadingRate2x2Sample2(
                        physical_device_, get_fragment_shading_rates);
                private_fragment_shading_rate_2x2_sample_2_supported_ =
                    private_fragment_shading_rate_properties_2x2_sample_2_supported_ &&
                    enumerated_2x2_sample_2;
            }
#endif
        }
        REXLOG_CAT_INFO(kLogGPU, "nr2: private headless device '{}' (API {}.{}.{})",
                        props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                        VK_API_VERSION_MINOR(props.apiVersion),
                        VK_API_VERSION_PATCH(props.apiVersion));

        uint32_t family_count = 0;
        fns_.GetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        fns_.GetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count,
                                                    families.data());
        queue_family_ = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            const VkQueueFlags required =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & required) == required) {
                queue_family_ = i;
                queue_supports_compute_ = true;
                break;
            }
        }
        if (queue_family_ == UINT32_MAX) {
            for (uint32_t i = 0; i < family_count; ++i) {
                if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    queue_family_ = i;
                    break;
                }
            }
        }
        if (queue_family_ == UINT32_MAX) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: no graphics queue family");
            return false;
        }
        if (!queue_supports_compute_ && !gpu_resolve_spirv_.empty()) {
            REXLOG_CAT_WARN(
                kLogGPU,
                "nr2: selected graphics queue has no compute capability; "
                "direct GPU resolves are disabled");
        }
        return true;
    }

    bool CreateDevice() {
        VkPhysicalDeviceVulkan13Features supported13 = {};
        supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
#if defined(VK_KHR_fragment_shading_rate)
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR
            supported_fragment_shading_rate = {};
        if (REXCVAR_GET(ac6_native_water_vrs) &&
            private_fragment_shading_rate_extension_supported_) {
            supported_fragment_shading_rate.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
            supported13.pNext = &supported_fragment_shading_rate;
        }
#endif
        VkPhysicalDeviceFeatures2 supported = {};
        supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supported.pNext = &supported13;
        fns_.GetPhysicalDeviceFeatures2(physical_device_, &supported);

#if defined(VK_KHR_fragment_shading_rate)
        const bool enable_private_fragment_shading_rate_2x2_sample_2 =
            REXCVAR_GET(ac6_native_water_vrs) &&
            private_fragment_shading_rate_extension_supported_ &&
            supported_fragment_shading_rate.pipelineFragmentShadingRate &&
            private_fragment_shading_rate_2x2_sample_2_supported_;
#endif

        if (!supported13.dynamicRendering) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: dynamicRendering unsupported - prover requires it");
            return false;
        }

        // Features the SDK-translated shaders / mirrored state can rely on.
        VkPhysicalDeviceVulkan13Features enabled13 = {};
        enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enabled13.dynamicRendering = VK_TRUE;
#if defined(VK_KHR_fragment_shading_rate)
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR
            enabled_fragment_shading_rate = {};
        if (enable_private_fragment_shading_rate_2x2_sample_2) {
            enabled_fragment_shading_rate.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
            enabled_fragment_shading_rate.pipelineFragmentShadingRate = VK_TRUE;
            enabled13.pNext = &enabled_fragment_shading_rate;
        }
#endif
        VkPhysicalDeviceFeatures2 enabled = {};
        enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        enabled.pNext = &enabled13;
        auto want = [&](VkBool32 VkPhysicalDeviceFeatures::* member, const char* name) {
            if (supported.features.*member) {
                enabled.features.*member = VK_TRUE;
            } else if (REXCVAR_GET(ac6_native_edram_alias_experimental)) {
                REXLOG_CAT_WARN(kLogGPU, "nr2: device feature {} unsupported", name);
            }
        };
        want(&VkPhysicalDeviceFeatures::independentBlend, "independentBlend");
        want(&VkPhysicalDeviceFeatures::sampleRateShading, "sampleRateShading");
        want(&VkPhysicalDeviceFeatures::depthClamp, "depthClamp");
        want(&VkPhysicalDeviceFeatures::fillModeNonSolid, "fillModeNonSolid");
        want(&VkPhysicalDeviceFeatures::fragmentStoresAndAtomics, "fragmentStoresAndAtomics");
        want(&VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics,
             "vertexPipelineStoresAndAtomics");
        want(&VkPhysicalDeviceFeatures::shaderClipDistance, "shaderClipDistance");
        want(&VkPhysicalDeviceFeatures::shaderCullDistance, "shaderCullDistance");
        want(&VkPhysicalDeviceFeatures::fullDrawIndexUint32, "fullDrawIndexUint32");
        depth_clamp_enabled_ = enabled.features.depthClamp == VK_TRUE;
        fill_mode_non_solid_enabled_ = enabled.features.fillModeNonSolid == VK_TRUE;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        const char* enabled_extensions[2] = {};
        uint32_t enabled_extension_count = 0;
        if (external_memory_host_supported_) {
            enabled_extensions[enabled_extension_count++] =
                "VK_EXT_external_memory_host";
        }
#if defined(VK_KHR_fragment_shading_rate)
        if (enable_private_fragment_shading_rate_2x2_sample_2) {
            enabled_extensions[enabled_extension_count++] =
                VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        }
#endif
        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &enabled;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        if (enabled_extension_count) {
            device_info.enabledExtensionCount = enabled_extension_count;
            device_info.ppEnabledExtensionNames = enabled_extensions;
        }
        VkResult result = fns_.CreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: vkCreateDevice failed ({})", int32_t(result));
            return false;
        }
#if defined(VK_KHR_fragment_shading_rate)
        private_fragment_shading_rate_2x2_sample_2_enabled_ =
            enable_private_fragment_shading_rate_2x2_sample_2;
        if (REXCVAR_GET(ac6_native_water_vrs)) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private exact water 2x2 VRS logical-device capability {}",
                private_fragment_shading_rate_2x2_sample_2_enabled_
                    ? "ENABLED"
                    : "unavailable (standard shading fallback)");
        }
#endif
        fns_.GetDeviceQueue(device_, queue_family_, 0, &queue_);
        if (external_memory_host_supported_) {
            fns_.GetMemoryHostPointerPropertiesEXT =
                reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                    fns_.GetDeviceProcAddr(device_, "vkGetMemoryHostPointerPropertiesEXT"));
            if (!fns_.GetMemoryHostPointerPropertiesEXT) {
                external_memory_host_supported_ = false;
            }
        }
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: VK_EXT_external_memory_host {} (minImportedHostPointerAlignment "
                        "{})",
                        external_memory_host_supported_ ? "ENABLED" : "unavailable",
                        uint64_t(min_imported_host_pointer_alignment_));

        // Format fallbacks, mirroring VulkanRenderTargetCache::Initialize.
        auto optimal_features = [&](VkFormat format) {
            VkFormatProperties props;
            fns_.GetPhysicalDeviceFormatProperties(physical_device_, format, &props);
            return props.optimalTilingFeatures;
        };
        constexpr VkFormatFeatureFlags kDepthNeeded =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        constexpr VkFormatFeatureFlags kColorNeeded =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        depth_d24s8_supported_ =
            (optimal_features(VK_FORMAT_D24_UNORM_S8_UINT) & kDepthNeeded) == kDepthNeeded;
        rg16_snorm_supported_ =
            (optimal_features(VK_FORMAT_R16G16_SNORM) & kColorNeeded) == kColorNeeded;
        rgba16_snorm_supported_ =
            (optimal_features(VK_FORMAT_R16G16B16A16_SNORM) & kColorNeeded) == kColorNeeded;
        gamma_as_unorm16_ =
            (optimal_features(VK_FORMAT_R16G16B16A16_UNORM) & kColorNeeded) == kColorNeeded;
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: format support: D24S8={} RG16_SNORM={} RGBA16_SNORM={} "
                        "gamma-as-unorm16={}",
                        depth_d24s8_supported_, rg16_snorm_supported_, rgba16_snorm_supported_,
                        gamma_as_unorm16_);
        return true;
    }

    bool ValidateShaderCorpusManifest() {
        manifest_shader_files_.clear();
        if (!REXCVAR_GET(vfetch_index_rounding_bias)) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private shader corpus requires "
                "vfetch_index_rounding_bias=true");
            return false;
        }

        const std::filesystem::path shader_dir =
            REXCVAR_GET(ac6_native_shader_dir);
        const std::filesystem::path manifest_path =
            shader_dir / "manifest.tsv";
        FILE* manifest = std::fopen(manifest_path.string().c_str(), "rb");
        if (!manifest) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: cannot open shader manifest {}",
                             manifest_path.string());
            return false;
        }

        auto reject = [&](std::string_view reason) {
            std::fclose(manifest);
            manifest_shader_files_.clear();
            REXLOG_CAT_ERROR(kLogGPU, "nr2: rejected shader manifest {}: {}",
                             manifest_path.string(), reason);
            return false;
        };
        auto read_line = [&](std::string& line) {
            char buffer[1024];
            if (!std::fgets(buffer, sizeof(buffer), manifest)) {
                return false;
            }
            line.assign(buffer);
            if (!line.empty() && line.back() != '\n' &&
                !std::feof(manifest)) {
                line.clear();
                return false;
            }
            while (!line.empty() &&
                   (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            return true;
        };
        auto split_tsv = [](const std::string& line) {
            std::vector<std::string_view> fields;
            size_t begin = 0;
            while (true) {
                const size_t end = line.find('\t', begin);
                fields.emplace_back(line.data() + begin,
                                    (end == std::string::npos ? line.size()
                                                             : end) -
                                        begin);
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
            return fields;
        };
        auto parse_u64 = [](std::string_view text, int base,
                            uint64_t& value_out) {
            if (text.empty()) {
                return false;
            }
            const char* begin = text.data();
            const char* end = begin + text.size();
            const std::from_chars_result result =
                std::from_chars(begin, end, value_out, base);
            return result.ec == std::errc() && result.ptr == end;
        };
        auto expect_line = [&](std::string_view expected) {
            std::string line;
            return read_line(line) && line == expected;
        };

        char expected[128];
        std::snprintf(expected, sizeof(expected), "manifest_format\t%u",
                      kNativeShaderManifestFormatVersion);
        if (!expect_line(expected)) {
            return reject("manifest format mismatch or truncated file");
        }
        std::snprintf(expected, sizeof(expected),
                      "spirv_modification_version\t0x%08X",
                      rex::graphics::SpirvShaderTranslator::Modification::
                          kVersion);
        if (!expect_line(expected)) {
            return reject("SPIR-V modification ABI mismatch");
        }
        std::snprintf(
            expected, sizeof(expected),
            "required_modification_mask\t0x%016llX",
            static_cast<unsigned long long>(
                kRequiredNativeShaderModificationMask));
        if (!expect_line(expected)) {
            return reject("required shader modification mask mismatch");
        }
        std::snprintf(expected, sizeof(expected),
                      "source_xpso_version\t0x%08X",
                      kRequiredNativePipelineStoreVersion);
        if (!expect_line(expected)) {
            return reject("source pipeline-store version mismatch");
        }
        if (!expect_line("source_xpso_api\tfbo")) {
            return reject("private corpus was not generated from an FBO store");
        }
        if (!expect_line(
                "ucode_hash\ttype\tmodification\tucode_dwords\t"
                "spv_bytes\tspv_xxh3\tstatus\tfile")) {
            return reject("shader row header mismatch");
        }

        size_t row_count = 0;
        size_t declared_complete_count = 0;
        bool saw_completion = false;
        std::string line;
        while (read_line(line)) {
            const std::vector<std::string_view> fields = split_tsv(line);
            if (fields.size() == 2 && fields[0] == "corpus_complete") {
                uint64_t complete_count = 0;
                if (!parse_u64(fields[1], 10, complete_count) ||
                    complete_count > SIZE_MAX) {
                    return reject("invalid corpus completion count");
                }
                declared_complete_count = size_t(complete_count);
                saw_completion = true;
                break;
            }
            if (fields.size() != 8 || fields[6] != "ok") {
                return reject("malformed or unsuccessful shader row");
            }

            uint64_t ucode_hash = 0;
            uint64_t modification = 0;
            uint64_t ucode_dwords = 0;
            uint64_t spv_bytes = 0;
            uint64_t spv_xxh3 = 0;
            if (fields[0].size() != 16 || fields[2].size() != 16 ||
                fields[5].size() != 16 ||
                !parse_u64(fields[0], 16, ucode_hash) ||
                !parse_u64(fields[2], 16, modification) ||
                !parse_u64(fields[3], 10, ucode_dwords) ||
                !parse_u64(fields[4], 10, spv_bytes) ||
                !parse_u64(fields[5], 16, spv_xxh3) ||
                ucode_dwords == 0 || spv_bytes == 0 ||
                (spv_bytes % sizeof(uint32_t)) != 0 ||
                spv_bytes > SIZE_MAX ||
                (modification & kRequiredNativeShaderModificationMask) == 0 ||
                (fields[1] != "vertex" && fields[1] != "pixel")) {
                return reject("invalid shader identity, size, hash, or mask");
            }

            char expected_name[96];
            std::snprintf(
                expected_name, sizeof(expected_name),
                "%016llx.%c.%016llx.spv",
                static_cast<unsigned long long>(ucode_hash),
                fields[1] == "pixel" ? 'p' : 'v',
                static_cast<unsigned long long>(modification));
            if (fields[7] != expected_name) {
                return reject("shader filename does not match its identity");
            }
            if (!manifest_shader_files_
                     .emplace(std::string(fields[7]),
                              std::make_pair(size_t(spv_bytes), spv_xxh3))
                     .second) {
                return reject("duplicate shader filename");
            }
            ++row_count;
        }
        if (std::ferror(manifest)) {
            return reject("I/O error while reading manifest");
        }
        if (!saw_completion || row_count == 0 ||
            declared_complete_count != row_count) {
            return reject("missing or inconsistent corpus completion marker");
        }
        std::string trailing_line;
        if (read_line(trailing_line) || std::ferror(manifest) ||
            !std::feof(manifest)) {
            return reject("data follows corpus completion marker");
        }

        std::error_code directory_ec;
        size_t disk_spv_count = 0;
        for (std::filesystem::directory_iterator it(shader_dir, directory_ec),
             end;
             !directory_ec && it != end; it.increment(directory_ec)) {
            if (!it->is_regular_file(directory_ec) || directory_ec ||
                it->path().extension() != ".spv") {
                continue;
            }
            ++disk_spv_count;
            if (!manifest_shader_files_.contains(
                    it->path().filename().string())) {
                return reject("unlisted SPIR-V file in corpus directory");
            }
        }
        if (directory_ec || disk_spv_count != manifest_shader_files_.size()) {
            return reject("manifest/file count mismatch");
        }

        if (std::fclose(manifest) != 0) {
            manifest_shader_files_.clear();
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: shader manifest close failed: {}",
                             manifest_path.string());
            return false;
        }
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: accepted shader corpus ABI {} with {} exact "
                        "SPIR-V files",
                        kNativeShaderManifestFormatVersion,
                        manifest_shader_files_.size());
        return true;
    }

    bool LoadShaderModules() {
        const std::string& dir = REXCVAR_GET(ac6_native_shader_dir);
        size_t ok = 0, fail = 0;
        for (const auto& [name, expected_file] : manifest_shader_files_) {
            const std::filesystem::path path =
                std::filesystem::path(dir) / name;
            std::vector<uint32_t> words;
            if (!ReadFileWords(path, words) ||
                words.size() * sizeof(uint32_t) != expected_file.first ||
                XXH3_64bits(words.data(),
                            words.size() * sizeof(uint32_t)) !=
                    expected_file.second) {
                REXLOG_CAT_WARN(kLogGPU, "nr2: cannot read {}", name);
                ++fail;
                continue;
            }
            LoadedShader shader;
            if (!ReflectSpirvBindings(words.data(), words.size(),
                                      shader.bindings)) {
                REXLOG_CAT_WARN(kLogGPU,
                                "nr2: SPIR-V reflection failed for {}", name);
                ++fail;
                continue;
            }
            VkShaderModuleCreateInfo create_info = {};
            create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            create_info.codeSize = words.size() * sizeof(uint32_t);
            create_info.pCode = words.data();
            VkResult result = fns_.CreateShaderModule(
                device_, &create_info, nullptr, &shader.module);
            if (result != VK_SUCCESS) {
                REXLOG_CAT_WARN(
                    kLogGPU,
                    "nr2: vkCreateShaderModule failed for {} ({})", name,
                    int32_t(result));
                ++fail;
                continue;
            }
            shaders_.emplace(name, shader);
            ++ok;
        }
        shader_modules_ok_ = ok;
        shader_modules_fail_ = fail;
        REXLOG_CAT_INFO(kLogGPU, "nr2: shader modules {} ok/{} fail (dir {})", ok, fail, dir);
        return fail == 0 && ok == manifest_shader_files_.size();
    }

    static bool ReadFileWords(const std::filesystem::path& path, std::vector<uint32_t>& out) {
        FILE* f = std::fopen(path.string().c_str(), "rb");
        if (!f) {
            return false;
        }
        if (std::fseek(f, 0, SEEK_END) != 0) {
            std::fclose(f);
            return false;
        }
        long size = std::ftell(f);
        if (size <= 0 || (size % 4) != 0 ||
            std::fseek(f, 0, SEEK_SET) != 0) {
            std::fclose(f);
            return false;
        }
        out.resize(size_t(size) / 4);
        bool read_ok = std::fread(out.data(), size_t(size), 1, f) == 1;
        read_ok = std::fclose(f) == 0 && read_ok;
        return read_ok;
    }

    bool ParsePipelineStore(std::vector<StoredPipeline>& out, size_t& bad_hash_out) {
        const std::string& path = REXCVAR_GET(ac6_native_pipeline_store);
        FILE* f = std::fopen(path.c_str(), "rb");  // READ-ONLY
        if (!f) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: cannot open pipeline store '{}'", path);
            return false;
        }
        if (std::fseek(f, 0, SEEK_END) != 0) {
            std::fclose(f);
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: cannot seek pipeline store '{}'", path);
            return false;
        }
        long size = std::ftell(f);
        if (size <= 0 || std::fseek(f, 0, SEEK_SET) != 0) {
            std::fclose(f);
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: pipeline store '{}' has invalid size",
                             path);
            return false;
        }
        std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
        bool read_ok = !data.empty() && std::fread(data.data(), data.size(), 1, f) == 1;
        read_ok = std::fclose(f) == 0 && read_ok;
        if (!read_ok || data.size() < 12) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: pipeline store '{}' unreadable/too small", path);
            return false;
        }
        uint32_t magic, magic_api, version_swapped;
        std::memcpy(&magic, data.data(), 4);
        std::memcpy(&magic_api, data.data() + 4, 4);
        std::memcpy(&version_swapped, data.data() + 8, 4);
        if (magic != 0x53504558u) {  // 'XEPS'
            REXLOG_CAT_ERROR(kLogGPU, "nr2: bad .xpso magic 0x{:08X}", magic);
            return false;
        }
        const uint32_t version = ByteSwap32(version_swapped);
        if (version != kRequiredNativePipelineStoreVersion) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected .xpso version 0x{:08X}; required 0x{:08X}",
                version, kRequiredNativePipelineStoreVersion);
            return false;
        }
        if (magic_api != 0) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: private pipeline cache requires an FBO store (API "
                "tag {}, expected 0)",
                magic_api);
            return false;
        }
        constexpr size_t kRecordSize =
            sizeof(uint64_t) + kPipelineDescriptionSize;
        if ((data.size() - 12) % kRecordSize != 0) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: .xpso has a {}-byte partial record tail",
                             (data.size() - 12) % kRecordSize);
            return false;
        }
        bad_hash_out = 0;
        for (size_t off = 12; off + kRecordSize <= data.size(); off += kRecordSize) {
            const uint8_t* rec = data.data() + off;
            uint64_t description_hash = 0;
            std::memcpy(&description_hash, rec, sizeof(description_hash));
            StoredPipeline p;
            if (!DecodePipelineDescription(
                    description_hash, rec + sizeof(description_hash),
                    kPipelineDescriptionSize, p)) {
                ++bad_hash_out;
                out.clear();
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: .xpso description at byte {} failed XXH3 validation",
                    off);
                return false;
            }
            if ((p.vs_modification &
                 kRequiredNativeShaderModificationMask) == 0 ||
                (p.ps_hash != 0 &&
                 (p.ps_modification &
                  kRequiredNativeShaderModificationMask) == 0)) {
                out.clear();
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: .xpso description {:016x} lacks required shader "
                    "modification mask 0x{:016X}",
                    description_hash,
                    kRequiredNativeShaderModificationMask);
                return false;
            }
            if (!FindShader(p.vs_hash, false, p.vs_modification) ||
                (p.ps_hash != 0 &&
                 !FindShader(p.ps_hash, true, p.ps_modification))) {
                out.clear();
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: .xpso description {:016x} references a shader "
                    "missing from the exact corpus manifest",
                    description_hash);
                return false;
            }
            out.push_back(p);
        }
        if (out.empty()) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: .xpso contains no pipeline records");
            return false;
        }
        return true;
    }

    VkFormat ColorFormat(uint32_t xenos_format) const {
        // Mirrors VulkanRenderTargetCache::GetColorVulkanFormat with fallbacks
        // resolved against this device (xenos::ColorRenderTargetFormat values).
        switch (xenos_format) {
            case 0:  // k_8_8_8_8
                return VK_FORMAT_R8G8B8A8_UNORM;
            case 1:  // k_8_8_8_8_GAMMA
                return gamma_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
            case 2:   // k_2_10_10_10
            case 10:  // k_2_10_10_10_AS_10_10_10_10
                return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case 3:   // k_2_10_10_10_FLOAT
            case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case 4:  // k_16_16
                return rg16_snorm_supported_ ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SFLOAT;
            case 5:  // k_16_16_16_16
                return rgba16_snorm_supported_ ? VK_FORMAT_R16G16B16A16_SNORM
                                               : VK_FORMAT_R16G16B16A16_SFLOAT;
            case 6:  // k_16_16_FLOAT
                return VK_FORMAT_R16G16_SFLOAT;
            case 7:  // k_16_16_16_16_FLOAT
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case 14:  // k_32_FLOAT
                return VK_FORMAT_R32_SFLOAT;
            case 15:  // k_32_32_FLOAT
                return VK_FORMAT_R32G32_SFLOAT;
            default:
                return VK_FORMAT_UNDEFINED;
        }
    }

    VkFormat DepthFormat(uint32_t xenos_format) const {
        // kD24S8 = 0, kD24FS8 = 1.
        if (xenos_format == 0 && depth_d24s8_supported_) {
            return VK_FORMAT_D24_UNORM_S8_UINT;
        }
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    VkDescriptorSetLayout GetSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
        std::string key;
        key.reserve(bindings.size() * 16);
        for (const auto& b : bindings) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%u:%u:%u:%u;", b.binding, uint32_t(b.descriptorType),
                          b.descriptorCount, uint32_t(b.stageFlags));
            key += buf;
        }
        auto it = set_layouts_.find(key);
        if (it != set_layouts_.end()) {
            return it->second;
        }
        VkDescriptorSetLayoutCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = uint32_t(bindings.size());
        create_info.pBindings = bindings.empty() ? nullptr : bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (fns_.CreateDescriptorSetLayout(device_, &create_info, nullptr, &layout) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        set_layouts_.emplace(std::move(key), layout);
        return layout;
    }

    // Merges the reflected descriptor interface of a VS+PS pair per set.
    static void MergeShaderBindings(const LoadedShader* vs, const LoadedShader* ps,
                                    std::map<uint32_t, VkDescriptorSetLayoutBinding>* merged,
                                    std::map<uint32_t, ReflectedBinding>* reflected) {
        constexpr uint32_t kSetCount = 4;
        auto add = [&](const std::vector<ReflectedBinding>& bindings, VkShaderStageFlags stage) {
            for (const auto& rb : bindings) {
                if (rb.set >= kSetCount) {
                    continue;
                }
                auto [it, inserted] = merged[rb.set].try_emplace(rb.binding);
                VkDescriptorSetLayoutBinding& b = it->second;
                if (inserted) {
                    b.binding = rb.binding;
                    b.descriptorType = rb.type;
                    b.descriptorCount = rb.count;
                    b.stageFlags = stage;
                    b.pImmutableSamplers = nullptr;
                } else {
                    b.stageFlags |= stage;
                    b.descriptorCount = std::max(b.descriptorCount, rb.count);
                }
                if (reflected) {
                    auto [rit, rinserted] = reflected[rb.set].try_emplace(rb.binding, rb);
                    if (!rinserted) {
                        rit->second.count = std::max(rit->second.count, rb.count);
                    }
                }
            }
        };
        if (vs) {
            add(vs->bindings, VK_SHADER_STAGE_VERTEX_BIT);
        }
        if (ps) {
            add(ps->bindings, VK_SHADER_STAGE_FRAGMENT_BIT);
        }
    }

    // Builds a pipeline layout covering the merged VS+PS reflected interface.
    // Layout scheme cross-checked against VulkanCommandProcessor::
    // GetPipelineLayout: 4 sets (shared memory, constants, vertex textures,
    // pixel textures), unused sets empty. Optionally returns the four
    // descriptor set layouts (M3 slice 2 draw execution).
    VkPipelineLayout GetPipelineLayoutFor(const LoadedShader* vs, const LoadedShader* ps,
                                          VkDescriptorSetLayout* out_set_layouts = nullptr) {
        constexpr uint32_t kSetCount = 4;
        std::map<uint32_t, VkDescriptorSetLayoutBinding> merged[kSetCount];
        MergeShaderBindings(vs, ps, merged, nullptr);

        VkDescriptorSetLayout set_layouts[kSetCount];
        std::string key;
        for (uint32_t set = 0; set < kSetCount; ++set) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            bindings.reserve(merged[set].size());
            for (const auto& [binding, b] : merged[set]) {
                bindings.push_back(b);
            }
            set_layouts[set] = GetSetLayout(bindings);
            if (set_layouts[set] == VK_NULL_HANDLE) {
                return VK_NULL_HANDLE;
            }
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%p;", reinterpret_cast<void*>(set_layouts[set]));
            key += buf;
        }
        if (out_set_layouts) {
            std::memcpy(out_set_layouts, set_layouts, sizeof(set_layouts));
        }
        auto it = pipeline_layouts_.find(key);
        if (it != pipeline_layouts_.end()) {
            return it->second;
        }
        VkPipelineLayoutCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        create_info.setLayoutCount = kSetCount;
        create_info.pSetLayouts = set_layouts;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (fns_.CreatePipelineLayout(device_, &create_info, nullptr, &layout) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        pipeline_layouts_.emplace(std::move(key), layout);
        return layout;
    }

    const LoadedShader* FindShader(uint64_t hash, bool pixel, uint64_t modification) const {
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx.%c.%016llx.spv",
                      static_cast<unsigned long long>(hash), pixel ? 'p' : 'v',
                      static_cast<unsigned long long>(modification));
        auto it = shaders_.find(name);
        return it != shaders_.end() ? &it->second : nullptr;
    }

    static bool SharedPipelinePlanCacheRequested() {
        const char* value =
            std::getenv("AC6_NR2_NATIVE_PLAN_CACHE");
        return value && std::strcmp(value, "1") == 0;
    }

    void ResetSharedPipelinePlanCache(bool enabled) {
        shared_pipeline_plan_cache_entry_ = {};
        shared_pipeline_plan_cache_stats_ = {};
        shared_pipeline_plan_cache_enabled_ = enabled;
    }

    void ReportSharedPipelinePlanCache() const {
        if (!shared_pipeline_plan_cache_enabled_) {
            return;
        }
        const SharedPipelinePlanCacheStats& stats =
            shared_pipeline_plan_cache_stats_;
        REXLOG_CAT_ERROR(
            kLogGPU,
            "nr2-native-plan-cache: schema=1 lookups={} hits={} "
            "miss_no_entry={} miss_hash={} miss_invalid_payload={} "
            "miss_description={} miss_host_type={} miss_layout={} "
            "miss_vertex_shader={} miss_pixel_shader={} "
            "miss_geometry_shader={} miss_stale_pipeline={} "
            "slow_successes={} stores={} "
            "store_reject_invalid_payload={} "
            "store_reject_inconsistent={} "
            "fast_water_vrs_selections={}",
            stats.lookups, stats.hits, stats.miss_no_entry,
            stats.miss_hash, stats.miss_invalid_payload,
            stats.miss_description, stats.miss_host_type,
            stats.miss_layout, stats.miss_vertex_shader,
            stats.miss_pixel_shader, stats.miss_geometry_shader,
            stats.miss_stale_pipeline, stats.slow_successes,
            stats.stores, stats.store_reject_invalid_payload,
            stats.store_reject_inconsistent,
            stats.fast_water_vrs_selections);
    }

    const NativePipeline* FindSharedPipelinePlanCache(
        uint64_t description_hash, const uint8_t* serialized_description,
        size_t serialized_description_size,
        uint32_t expected_host_vertex_shader_type,
        VkPipelineLayout authoritative_layout,
        VkShaderModule authoritative_vertex_shader,
        VkShaderModule authoritative_pixel_shader,
        VkShaderModule generated_geometry_shader) {
        if (!shared_pipeline_plan_cache_enabled_) {
            return nullptr;
        }
        SharedPipelinePlanCacheStats& stats =
            shared_pipeline_plan_cache_stats_;
        ++stats.lookups;
        const SharedPipelinePlanCacheEntry& entry =
            shared_pipeline_plan_cache_entry_;
        if (!entry.valid) {
            ++stats.miss_no_entry;
            return nullptr;
        }
        if (entry.description_hash != description_hash) {
            ++stats.miss_hash;
            return nullptr;
        }
        if (!serialized_description ||
            serialized_description_size != kPipelineDescriptionSize) {
            ++stats.miss_invalid_payload;
            return nullptr;
        }
        if (std::memcmp(entry.serialized_description.data(),
                        serialized_description,
                        kPipelineDescriptionSize) != 0) {
            ++stats.miss_description;
            return nullptr;
        }
        if (entry.host_vertex_shader_type !=
            expected_host_vertex_shader_type) {
            ++stats.miss_host_type;
            return nullptr;
        }
        if (entry.layout != authoritative_layout) {
            ++stats.miss_layout;
            return nullptr;
        }
        if (entry.vertex_shader != authoritative_vertex_shader) {
            ++stats.miss_vertex_shader;
            return nullptr;
        }
        if (entry.pixel_shader != authoritative_pixel_shader) {
            ++stats.miss_pixel_shader;
            return nullptr;
        }
        if (entry.geometry_shader != generated_geometry_shader) {
            ++stats.miss_geometry_shader;
            return nullptr;
        }
        const NativePipeline* native_pipeline = entry.native_pipeline;
        if (!native_pipeline || entry.pipeline == VK_NULL_HANDLE ||
            native_pipeline->pipeline != entry.pipeline ||
            native_pipeline->layout != entry.layout ||
            native_pipeline->borrowed_vertex_shader != entry.vertex_shader ||
            native_pipeline->borrowed_pixel_shader != entry.pixel_shader ||
            native_pipeline->borrowed_geometry_shader !=
                entry.geometry_shader) {
            ++stats.miss_stale_pipeline;
            shared_pipeline_plan_cache_entry_ = {};
            return nullptr;
        }
        ++stats.hits;
        return native_pipeline;
    }

    void StoreSharedPipelinePlanCache(
        uint64_t description_hash, const uint8_t* serialized_description,
        size_t serialized_description_size,
        uint32_t expected_host_vertex_shader_type,
        VkPipelineLayout authoritative_layout,
        VkShaderModule authoritative_vertex_shader,
        VkShaderModule authoritative_pixel_shader,
        VkShaderModule generated_geometry_shader,
        const NativePipeline& native_pipeline, VkPipeline pipeline) {
        if (!shared_pipeline_plan_cache_enabled_) {
            return;
        }
        SharedPipelinePlanCacheStats& stats =
            shared_pipeline_plan_cache_stats_;
        ++stats.slow_successes;
        if (!serialized_description ||
            serialized_description_size != kPipelineDescriptionSize) {
            ++stats.store_reject_invalid_payload;
            shared_pipeline_plan_cache_entry_ = {};
            return;
        }
        if (pipeline == VK_NULL_HANDLE ||
            native_pipeline.pipeline != pipeline ||
            native_pipeline.layout != authoritative_layout ||
            native_pipeline.borrowed_vertex_shader !=
                authoritative_vertex_shader ||
            native_pipeline.borrowed_pixel_shader !=
                authoritative_pixel_shader ||
            native_pipeline.borrowed_geometry_shader !=
                generated_geometry_shader) {
            ++stats.store_reject_inconsistent;
            shared_pipeline_plan_cache_entry_ = {};
            return;
        }
        SharedPipelinePlanCacheEntry& entry =
            shared_pipeline_plan_cache_entry_;
        entry.valid = true;
        entry.description_hash = description_hash;
        std::memcpy(entry.serialized_description.data(),
                    serialized_description, kPipelineDescriptionSize);
        entry.host_vertex_shader_type = expected_host_vertex_shader_type;
        entry.layout = authoritative_layout;
        entry.vertex_shader = authoritative_vertex_shader;
        entry.pixel_shader = authoritative_pixel_shader;
        entry.geometry_shader = generated_geometry_shader;
        entry.pipeline = pipeline;
        entry.native_pipeline = &native_pipeline;
        ++stats.stores;
    }

    void RecordSharedWaterVrsSelection(const NativePipeline& pipeline) {
        if (!pipeline.ac6_water_vrs_2x2) {
            return;
        }
        const uint64_t selection_count =
            ++shared_water_vrs_selection_count_;
        if (selection_count == 1) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: first shared exact water 2x2 VRS selection "
                "(accepted pipelines={})",
                water_vrs_pipeline_count_);
        }
    }

    void RecordPrivateWaterVrsDraw(const NativePipeline& pipeline) {
        if (is_shared_device() || !pipeline.ac6_water_vrs_2x2) {
            return;
        }
        if (++private_water_vrs_draw_count_ == 1) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: first private exact water 2x2 VRS draw "
                "(accepted pipelines={})",
                water_vrs_pipeline_count_);
        }
    }

    // State translation mirrors VulkanPipelineCache::EnsurePipelineCreated
    // (thirdparty/rexglue-sdk/src/graphics/vulkan/pipeline_cache.cpp:3009),
    // FBO path, dynamic rendering, no tessellation stages. Shared authority
    // may borrow the SDK-generated geometry stage for exact point/rect/quad
    // expansion. When out_uncached is set, the created pipeline is returned
    // instead of being stored in the description-hash cache (M3 slice 2
    // topology variants). A forced layout is borrowed from the authoritative
    // SDK pipeline cache; nr2 never owns or destroys it.
    bool CreatePipeline(const StoredPipeline& p, std::string& fail_reason,
                        NativePipeline* out_uncached = nullptr,
                        VkPipelineLayout forced_layout = VK_NULL_HANDLE,
                        VkShaderModule authoritative_vertex_shader =
                            VK_NULL_HANDLE,
                        VkShaderModule authoritative_pixel_shader =
                            VK_NULL_HANDLE,
                        VkShaderModule generated_geometry_shader =
                            VK_NULL_HANDLE) {
        // --- Requirements gate (ArePipelineRequirementsMet equivalent). ---
        // Modification bits: high dword low 4 bits = Shader::HostVertexShaderType.
        // 0 = kVertex, 9/10 = point/rectangle list expanded to a triangle strip
        // in the vertex shader - all three are plain vertex-stage SPIR-V.
        // 1-7 are tessellation domains (need TES/TCS stages not generated
        // offline), 8 is the memexport compute fallback.
        const uint32_t host_vertex_shader_type = uint32_t(p.vs_modification >> 32) & 0xFu;
        if (host_vertex_shader_type != 0 && host_vertex_shader_type != 9 &&
            host_vertex_shader_type != 10) {
            fail_reason = "vs_host_type_not_vertex_stage";
            return false;
        }
        const uint32_t topology_bits = p.state.primitive_topology();
        if (topology_bits == kTopologyPatchList) {
            fail_reason = "patch_list_needs_tess_stages";
            return false;
        }
        if (topology_bits == kTopologyTriangleFan) {
            fail_reason = "triangle_fan_unsupported";
            return false;
        }
        if (p.state.depth_clamp_enable() && !depth_clamp_enabled_) {
            fail_reason = "depth_clamp_unsupported";
            return false;
        }
        if (p.state.polygon_mode() != kPolygonModeFill && !fill_mode_non_solid_enabled_) {
            fail_reason = "non_solid_fill_unsupported";
            return false;
        }

        const bool description_requires_geometry_shader =
            p.state.geometry_shader() != kGeometryShaderNone;
        if (is_shared_device() &&
            description_requires_geometry_shader !=
                (generated_geometry_shader != VK_NULL_HANDLE)) {
            fail_reason = "shared_geometry_shader_state_module_mismatch";
            return false;
        }
        if (!is_shared_device() &&
            (authoritative_vertex_shader != VK_NULL_HANDLE ||
             authoritative_pixel_shader != VK_NULL_HANDLE ||
             generated_geometry_shader != VK_NULL_HANDLE)) {
            // Borrowed modules are device-specific and may only be used with
            // the authoritative SDK device that created them.
            fail_reason = "borrowed_shader_on_private_device";
            return false;
        }

        const LoadedShader* vs = nullptr;
        const LoadedShader* ps = nullptr;
        VkShaderModule vertex_shader_module = authoritative_vertex_shader;
        VkShaderModule pixel_shader_module = authoritative_pixel_shader;
        if (is_shared_device()) {
            if (vertex_shader_module == VK_NULL_HANDLE) {
                fail_reason = "missing_authoritative_vs_module";
                return false;
            }
            if (!p.ps_hash || pixel_shader_module == VK_NULL_HANDLE) {
                // SDK depth-only pipelines may select a generated helper
                // fragment module. Until that helper is also exported, use
                // the unchanged SDK pipeline for PS-less draws.
                fail_reason = "missing_authoritative_ps_module";
                return false;
            }
        } else {
            vs = FindShader(p.vs_hash, false, p.vs_modification);
            if (!vs) {
                fail_reason = "missing_vs_translation";
                return false;
            }
            vertex_shader_module = vs->module;
            if (p.ps_hash) {
                ps = FindShader(p.ps_hash, true, p.ps_modification);
                if (!ps) {
                    fail_reason = "missing_ps_translation";
                    return false;
                }
                pixel_shader_module = ps->module;
            }
        }

        if (description_requires_geometry_shader && !is_shared_device()) {
            // The SDK builds these geometry shaders at runtime (point/rect/quad
            // list expansion); this slice creates the pipeline without the GS
            // stage and counts it, to still prove the rest of the state.
            ++pipelines_gs_omitted_;
        }

        VkDescriptorSetLayout pipeline_set_layouts[4] = {};
        VkPipelineLayout layout = forced_layout;
        if (layout == VK_NULL_HANDLE) {
            layout = GetPipelineLayoutFor(vs, ps, pipeline_set_layouts);
        }
        if (layout == VK_NULL_HANDLE) {
            fail_reason = "pipeline_layout_creation_failed";
            return false;
        }

        // --- Shader stages (vertex pulling: no vertex input state). ---
        VkPipelineShaderStageCreateInfo stages[3] = {};
        uint32_t stage_count = 0;
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[stage_count].module = vertex_shader_module;
        stages[stage_count].pName = "main";
        ++stage_count;
        if (description_requires_geometry_shader &&
            generated_geometry_shader != VK_NULL_HANDLE) {
            // Shared-authority pipelines borrow the exact SDK-generated
            // point/rectangle/quad expansion module. It must sit between the
            // guest VS and PS stages and remains owned by VulkanPipelineCache.
            stages[stage_count].sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            stages[stage_count].module = generated_geometry_shader;
            stages[stage_count].pName = "main";
            ++stage_count;
        }
        if (pixel_shader_module != VK_NULL_HANDLE) {
            stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[stage_count].module = pixel_shader_module;
            stages[stage_count].pName = "main";
            ++stage_count;
        }

        VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
        vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {};
        input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        switch (topology_bits) {
            case kTopologyPointList:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                break;
            case kTopologyLineList:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                break;
            case kTopologyLineStrip:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                break;
            case kTopologyTriangleList:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                break;
            case kTopologyTriangleStrip:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                break;
            case kTopologyLineListWithAdjacency:
                input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
                break;
            default:
                fail_reason = "unhandled_topology";
                return false;
        }
        if (p.state.primitive_restart() && topology_bits != kTopologyLineStrip &&
            topology_bits != kTopologyTriangleStrip) {
            fail_reason = "primitive_restart_on_list_topology";
            return false;
        }
        input_assembly_state.primitiveRestartEnable =
            p.state.primitive_restart() ? VK_TRUE : VK_FALSE;

        VkPipelineViewportStateCreateInfo viewport_state = {};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization_state = {};
        rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state.rasterizerDiscardEnable =
            p.state.rasterizer_discard() ? VK_TRUE : VK_FALSE;
        rasterization_state.depthClampEnable = p.state.depth_clamp_enable() ? VK_TRUE : VK_FALSE;
        switch (p.state.polygon_mode()) {
            case kPolygonModeFill:
                rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
                break;
            case kPolygonModeLine:
                rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
                break;
            case kPolygonModePoint:
                rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
                break;
            default:
                fail_reason = "unhandled_polygon_mode";
                return false;
        }
        rasterization_state.cullMode = VK_CULL_MODE_NONE;
        if (p.state.cull_front()) {
            rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
        }
        if (p.state.cull_back()) {
            rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
        }
        rasterization_state.frontFace = p.state.front_face_clockwise()
                                            ? VK_FRONT_FACE_CLOCKWISE
                                            : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state.depthBiasEnable = p.render_pass_key.depth_used() ? VK_TRUE : VK_FALSE;
        rasterization_state.lineWidth = 1.0f;

        const bool subpass_has_attachments = p.render_pass_key.depth_and_color_used() != 0;
        VkSampleMask sample_mask = UINT32_MAX;
        VkPipelineMultisampleStateCreateInfo multisample_state = {};
        multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        if (p.state.rasterizer_discard() || !subpass_has_attachments) {
            multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        } else {
            const uint32_t samples = 1u << p.render_pass_key.msaa_samples();
            const VkSampleCountFlags supported_samples =
                framebuffer_color_sample_counts_ & framebuffer_depth_sample_counts_;
            const bool native_2x_supported =
                is_shared_device()
                    ? shared_msaa_2x_attachments_supported_
                    : (supported_samples & VK_SAMPLE_COUNT_2_BIT) != 0;
            if (samples == 2 && !native_2x_supported) {
                // SDK 2x-as-4x fallback: samples 0 and 3 of 4x.
                multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
                sample_mask = 0b1001;
                multisample_state.pSampleMask = &sample_mask;
            } else {
                multisample_state.rasterizationSamples = VkSampleCountFlagBits(samples);
            }
        }
        if (p.state.sample_rate_shading() &&
            multisample_state.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
            multisample_state.sampleShadingEnable = VK_TRUE;
            multisample_state.minSampleShading = 1.0f;
        }

        VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
        depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        constexpr uint32_t kCompareAlways = 7;  // xenos::CompareFunction::kAlways
        if (p.state.depth_write_enable() || p.state.depth_compare_op() != kCompareAlways) {
            depth_stencil_state.depthTestEnable = VK_TRUE;
            depth_stencil_state.depthWriteEnable =
                p.state.depth_write_enable() ? VK_TRUE : VK_FALSE;
            depth_stencil_state.depthCompareOp =
                VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + p.state.depth_compare_op());
        }
        if (p.state.stencil_test_enable()) {
            depth_stencil_state.stencilTestEnable = VK_TRUE;
            auto stencil_op = [](uint32_t v) {
                return VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + v);
            };
            auto compare_op = [](uint32_t v) {
                return VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + v);
            };
            depth_stencil_state.front.failOp = stencil_op(p.state.stencil_front_fail_op());
            depth_stencil_state.front.passOp = stencil_op(p.state.stencil_front_pass_op());
            depth_stencil_state.front.depthFailOp =
                stencil_op(p.state.stencil_front_depth_fail_op());
            depth_stencil_state.front.compareOp = compare_op(p.state.stencil_front_compare_op());
            depth_stencil_state.back.failOp = stencil_op(p.state.stencil_back_fail_op());
            depth_stencil_state.back.passOp = stencil_op(p.state.stencil_back_pass_op());
            depth_stencil_state.back.depthFailOp = stencil_op(p.state.stencil_back_depth_fail_op());
            depth_stencil_state.back.compareOp = compare_op(p.state.stencil_back_compare_op());
        }

        static const VkBlendFactor kBlendFactorMap[] = {
            VK_BLEND_FACTOR_ZERO,
            VK_BLEND_FACTOR_ONE,
            VK_BLEND_FACTOR_SRC_COLOR,
            VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
            VK_BLEND_FACTOR_DST_COLOR,
            VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
            VK_BLEND_FACTOR_SRC_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            VK_BLEND_FACTOR_DST_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
            VK_BLEND_FACTOR_CONSTANT_COLOR,
            VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
            VK_BLEND_FACTOR_CONSTANT_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
            VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
            VK_BLEND_FACTOR_ZERO,  // 8 entries of headroom: 4 bits stored
        };
        static const VkBlendOp kBlendOpMap[] = {
            VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN,
            VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD,
            VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
        };
        VkPipelineColorBlendAttachmentState color_blend_attachments[4] = {};
        const uint32_t color_rts_used = p.render_pass_key.color_rts_used();
        uint32_t color_attachment_count = 0;
        for (uint32_t rt = 0; rt < 4; ++rt) {
            if (!(color_rts_used & (1u << rt))) {
                continue;
            }
            color_attachment_count = rt + 1;
            const RenderTargetBits& rt_bits = p.render_targets[rt];
            VkPipelineColorBlendAttachmentState& attachment = color_blend_attachments[rt];
            if (rt_bits.src_color_blend_factor() != kBlendFactorOne ||
                rt_bits.dst_color_blend_factor() != kBlendFactorZero ||
                rt_bits.color_blend_op() != 0 ||
                rt_bits.src_alpha_blend_factor() != kBlendFactorOne ||
                rt_bits.dst_alpha_blend_factor() != kBlendFactorZero ||
                rt_bits.alpha_blend_op() != 0) {
                attachment.blendEnable = VK_TRUE;
                attachment.srcColorBlendFactor =
                    kBlendFactorMap[rt_bits.src_color_blend_factor()];
                attachment.dstColorBlendFactor =
                    kBlendFactorMap[rt_bits.dst_color_blend_factor()];
                attachment.colorBlendOp = kBlendOpMap[rt_bits.color_blend_op()];
                attachment.srcAlphaBlendFactor =
                    kBlendFactorMap[rt_bits.src_alpha_blend_factor()];
                attachment.dstAlphaBlendFactor =
                    kBlendFactorMap[rt_bits.dst_alpha_blend_factor()];
                attachment.alphaBlendOp = kBlendOpMap[rt_bits.alpha_blend_op()];
            }
            attachment.colorWriteMask = VkColorComponentFlags(rt_bits.color_write_mask());
        }
        VkPipelineColorBlendStateCreateInfo color_blend_state = {};
        color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state.attachmentCount = color_attachment_count;
        color_blend_state.pAttachments = color_blend_attachments;

        VkDynamicState dynamic_states[7] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state = {};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 7;
        dynamic_state.pDynamicStates = dynamic_states;

        // Dynamic rendering: render-target formats straight from the stored
        // RenderPassKey - no render pass objects.
        VkFormat color_attachment_formats[4] = {};
        for (uint32_t rt = 0; rt < 4; ++rt) {
            if (color_rts_used & (1u << rt)) {
                color_attachment_formats[rt] =
                    ColorFormat(p.render_pass_key.color_view_format(rt));
                if (color_attachment_formats[rt] == VK_FORMAT_UNDEFINED) {
                    fail_reason = "unhandled_color_format";
                    return false;
                }
            }
        }
        VkPipelineRenderingCreateInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.colorAttachmentCount = color_attachment_count;
        rendering_info.pColorAttachmentFormats =
            color_attachment_count ? color_attachment_formats : nullptr;
        if (p.render_pass_key.depth_used()) {
            const VkFormat depth_format = DepthFormat(p.render_pass_key.depth_format());
            rendering_info.depthAttachmentFormat = depth_format;
            rendering_info.stencilAttachmentFormat = depth_format;
        }

        bool use_ac6_water_vrs_2x2 = false;
#if defined(VK_KHR_fragment_shading_rate)
        const bool exact_ac6_water_vrs_candidate =
            REXCVAR_GET(ac6_native_water_vrs) &&
            IsExactAc6WaterVrsPipeline(p);
        const bool fragment_shading_rate_2x2_sample_2_enabled =
            is_shared_device()
                ? shared_fragment_shading_rate_2x2_sample_2_enabled_
                : private_fragment_shading_rate_2x2_sample_2_enabled_;
        use_ac6_water_vrs_2x2 =
            exact_ac6_water_vrs_candidate &&
            fragment_shading_rate_2x2_sample_2_enabled &&
            multisample_state.rasterizationSamples == VK_SAMPLE_COUNT_2_BIT;
        VkPipelineFragmentShadingRateStateCreateInfoKHR
            fragment_shading_rate_state = {};
        if (use_ac6_water_vrs_2x2) {
            fragment_shading_rate_state.sType =
                VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
            fragment_shading_rate_state.fragmentSize = {2, 2};
            fragment_shading_rate_state.combinerOps[0] =
                VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            fragment_shading_rate_state.combinerOps[1] =
                VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            rendering_info.pNext = &fragment_shading_rate_state;
        } else if (exact_ac6_water_vrs_candidate &&
                   !water_vrs_fallback_reported_) {
            water_vrs_fallback_reported_ = true;
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: exact water pipeline retained standard shading "
                "(shared={}, logical-device-2x2/sample2={}, samples={})",
                is_shared_device(),
                fragment_shading_rate_2x2_sample_2_enabled,
                uint32_t(multisample_state.rasterizationSamples));
        }
#endif

        VkGraphicsPipelineCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        create_info.pNext = &rendering_info;
        create_info.stageCount = stage_count;
        create_info.pStages = stages;
        create_info.pVertexInputState = &vertex_input_state;
        create_info.pInputAssemblyState = &input_assembly_state;
        create_info.pViewportState = &viewport_state;
        create_info.pRasterizationState = &rasterization_state;
        create_info.pMultisampleState = &multisample_state;
        create_info.pDepthStencilState = &depth_stencil_state;
        create_info.pColorBlendState = &color_blend_state;
        create_info.pDynamicState = &dynamic_state;
        create_info.layout = layout;
        create_info.renderPass = VK_NULL_HANDLE;
        create_info.basePipelineIndex = -1;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult result = fns_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create_info,
                                                       nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            char reason[64];
            std::snprintf(reason, sizeof(reason), "vkCreateGraphicsPipelines_%d", int(result));
            fail_reason = reason;
            REXLOG_CAT_WARN(kLogGPU,
                               "nr2: pipeline creation failed ({}): vs={:016X} mod={:016X} "
                               "ps={:016X} mod={:016X} topo={} rpkey=0x{:08X} state=0x{:012X}",
                               int32_t(result), p.vs_hash, p.vs_modification, p.ps_hash,
                               p.ps_modification, topology_bits, p.render_pass_key.key,
                               p.state.bits);
            return false;
        }
        NativePipeline native_pipeline;
        native_pipeline.pipeline = pipeline;
        native_pipeline.layout = layout;
        native_pipeline.borrowed_vertex_shader = authoritative_vertex_shader;
        native_pipeline.borrowed_pixel_shader = authoritative_pixel_shader;
        native_pipeline.borrowed_geometry_shader = generated_geometry_shader;
        std::memcpy(native_pipeline.set_layouts, pipeline_set_layouts,
                    sizeof(pipeline_set_layouts));
        native_pipeline.desc = p;
        native_pipeline.vs = vs;
        native_pipeline.ps = ps;
        native_pipeline.ac6_water_vrs_2x2 = use_ac6_water_vrs_2x2;
        if (use_ac6_water_vrs_2x2 && ++water_vrs_pipeline_count_ == 1) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: first accepted exact water 2x2 VRS pipeline, "
                "description={:016x}, combiner=KEEP/KEEP",
                p.description_hash);
        }
        if (out_uncached) {
            *out_uncached = native_pipeline;
        } else {
            pipelines_.emplace(p.description_hash, native_pipeline);
        }
        return true;
    }

    void ProvePipelines() {
        stored_pipelines_.clear();
        std::vector<StoredPipeline> stored;
        size_t bad_hash = 0;
        if (!ParsePipelineStore(stored, bad_hash)) {
            return;
        }
        for (const StoredPipeline& pipeline : stored) {
            stored_pipelines_.emplace(pipeline.description_hash, pipeline);
        }
        REXLOG_CAT_INFO(kLogGPU, "nr2: pipeline store: {} descriptions read, {} bad-hash "
                                 "records skipped ({})",
                        stored.size(), bad_hash, REXCVAR_GET(ac6_native_pipeline_store));
        if (is_shared_device() ||
            REXCVAR_GET(ac6_native_lazy_pipeline_creation)) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: shader modules {} ok/{} fail; deferred creation of {} "
                "stored pipelines until first use",
                shader_modules_ok_, shader_modules_fail_,
                stored_pipelines_.size());
            return;
        }
        size_t created = 0, failed = 0;
        std::map<std::string, size_t> fail_reasons;
        for (const StoredPipeline& p : stored) {
            std::string reason;
            if (CreatePipeline(p, reason)) {
                ++created;
            } else {
                ++failed;
                ++fail_reasons[reason];
            }
        }
        std::string reasons_text;
        {
            std::vector<std::pair<size_t, std::string>> sorted;
            sorted.reserve(fail_reasons.size());
            for (const auto& [reason, count] : fail_reasons) {
                sorted.emplace_back(count, reason);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            size_t listed = 0;
            for (const auto& [count, reason] : sorted) {
                if (listed++ == 4) {
                    reasons_text += ", ...";
                    break;
                }
                if (!reasons_text.empty()) {
                    reasons_text += ", ";
                }
                reasons_text += reason + "=" + std::to_string(count);
            }
            if (reasons_text.empty()) {
                reasons_text = "none";
            }
        }
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: shader modules {} ok/{} fail; pipelines {} created, {} failed "
                        "(top reasons: {})",
                        shader_modules_ok_, shader_modules_fail_, created, failed, reasons_text);
        if (pipelines_gs_omitted_) {
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: {} pipelines wanted a runtime-generated geometry shader "
                            "(point/rect/quad expansion) and were created without it",
                            pipelines_gs_omitted_);
        }
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: layout cache: {} descriptor set layouts, {} pipeline layouts",
                        set_layouts_.size(), pipeline_layouts_.size());
    }

    // -----------------------------------------------------------------------
    // M3 slice 2 — native execution of one captured draw, offscreen.

 public:
    enum class ExecResult { kExecuted, kRejected, kFatal };

    bool failed() const { return session_failed_; }

    void SetPersistentTargetsEnabled(bool enabled) {
        persistent_targets_enabled_ = enabled;
        if (!enabled) {
            DestroyPersistentTarget();
        }
    }

    // Live takeover submits draw work in guest order without waiting after
    // every draw. The next native resolve submission carries the fence that
    // drains all preceding work before readback, so per-draw resources must be
    // retained only until that boundary. This removes thousands of CPU/GPU
    // round trips per frame while preserving Vulkan queue order.
    void SetDeferredDrawWaits(bool enabled) {
        if (defer_draw_waits_ == enabled) {
            return;
        }
        if (!enabled &&
            (!deferred_draw_resources_.empty() ||
             deferred_command_pool_ != VK_NULL_HANDLE ||
             deferred_descriptor_pool_ != VK_NULL_HANDLE)) {
            DrainDeferredDrawResources();
        }
        defer_draw_waits_ = enabled;
    }

    void InvalidateUnvalidatedProvenance() {
        for (PersistentTarget& target : persistent_targets_) {
            target.unvalidated_safe = false;
        }
    }

    // Resolves the common AC6 color-copy path from a retained native render
    // target into the guest texture backing store. This deliberately handles
    // only the byte-exact 8_8_8_8 case for now; depth and float-format copies
    // continue using the emulated renderer until their conversion rules are
    // implemented. Returning kRejected is therefore a safe per-copy fallback,
    // not a failure of the persistent native session.
    ExecResult ResolveCapturedCopy(const DrawSnapshot& snap,
                                   uint32_t* written_address = nullptr,
                                   uint32_t* written_length = nullptr) {
        using namespace rex::graphics;
        if (session_failed_) {
            return ExecResult::kFatal;
        }
        // The emulator has completed this copy before entering the live hook,
        // and trace replay applies the recorded memory writes before invoking
        // this method. Any allocation may therefore have changed. Keep hash
        // reuse strictly resolve-delimited so render-to-texture data can never
        // retain a stale content identity across passes.
        guest_texture_allocation_hashes_.clear();
        if (written_address) {
            *written_address = 0;
        }
        if (written_length) {
            *written_length = 0;
        }
        if (device_ == VK_NULL_HANDLE) {
            return Fatal("resolve_prover_not_ready");
        }
        // Draw command buffers are recorded independently so failures remain
        // isolated, but submit the whole resolve-delimited pass at once. Queue
        // order guarantees the resolve submission below observes every draw,
        // and its existing fence remains the sole synchronization point.
        if (!SubmitDeferredDrawCommandBuffers()) {
            return Fatal("deferred_draw_batch_submit_failed");
        }
        // A copy command is a native submission boundary even when this
        // particular copy format or target cannot be resolved by nr2 yet.
        // Successful resolves drain the earlier fence-less draws with their
        // own fence. On every early fallback, wait once here so their
        // per-draw descriptor pools, command pools, staging buffers and
        // textures don't accumulate until a later supported resolve.
        DeferredDrawDrainGuard deferred_draw_drain_guard(this);
        if (snap.regs.size() < RegisterFile::kRegisterCount) {
            return Fatal("resolve_register_snapshot_short");
        }
        if (!snap.physical_membase || !snap.physical_size) {
            return Fatal("resolve_no_guest_memory_pointer");
        }

        auto regs_storage = std::make_unique<RegisterFile>();
        std::memcpy(regs_storage->values, snap.regs.data(),
                    sizeof(uint32_t) * RegisterFile::kRegisterCount);
        const RegisterFile& regs = *regs_storage;
        if ((regs[XE_GPU_REG_RB_MODECONTROL] & 7u) !=
            uint32_t(xenos::EdramMode::kCopy)) {
            return Reject(snap, "resolve_not_copy_mode");
        }

        const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
        const uint32_t source_rt = uint32_t(copy_control.copy_src_select);
        if (source_rt >= xenos::kMaxColorRenderTargets) {
            return Reject(snap, "resolve_depth_fallback");
        }
        if (copy_control.copy_command != xenos::CopyCommand::kRaw &&
            copy_control.copy_command != xenos::CopyCommand::kConvert) {
            return Reject(snap, "resolve_copy_command_fallback");
        }

        const auto copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
        const auto copy_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
        // TextureFormat::k_8_8_8_8 is 6. Restricting this first implementation
        // also excludes exponent-bias and component-number conversion paths.
        if (uint32_t(copy_dest_info.copy_dest_format) != 6u ||
            copy_dest_info.copy_dest_array || copy_dest_info.copy_dest_exp_bias) {
            return Reject(snap, "resolve_dest_format_fallback");
        }
        const uint32_t dest_endian = uint32_t(copy_dest_info.copy_dest_endian);
        if (dest_endian != 0u && dest_endian != 2u) {
            return Reject(snap, "resolve_dest_endian_fallback");
        }
        const uint32_t dest_pitch = copy_dest_pitch.copy_dest_pitch;
        const uint32_t dest_height = copy_dest_pitch.copy_dest_height;
        if (!dest_pitch || !dest_height) {
            return Reject(snap, "resolve_empty_destination");
        }

        const uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
        const uint32_t source_color_info =
            regs[reg::RB_COLOR_INFO::rt_register_indices[source_rt]];
        PersistentTarget* target = nullptr;
        bool target_surface_exact = false;
        for (PersistentTarget& candidate : persistent_targets_) {
            if (!candidate.valid || !candidate.initialized ||
                candidate.key.color_info[source_rt] != source_color_info) {
                continue;
            }
            RenderPassKeyBits candidate_rpkey{candidate.key.render_pass_key};
            if (!(candidate_rpkey.color_rts_used() & (1u << source_rt)) ||
                candidate_rpkey.color_view_format(source_rt) != 0u) {
                continue;
            }
            const bool candidate_surface_exact =
                candidate.key.surface_info == surface_info;
            if (!target ||
                (candidate_surface_exact && !target_surface_exact) ||
                (candidate_surface_exact == target_surface_exact &&
                 candidate.last_used_serial > target->last_used_serial)) {
                target = &candidate;
                target_surface_exact = candidate_surface_exact;
            }
        }
        if (!target) {
            return Reject(snap, "resolve_no_matching_native_target");
        }
        // An exact logical surface view may not own every physical EDRAM tile
        // any more. AC6's clear path, for example, writes most of a 640x360
        // 1x view through a newer 320x180 4x view of the same color base. Keep
        // the exact view as the base, but also retain the newest differing view
        // so the resolve can source physical samples that it actually wrote.
        PersistentTarget* overlay_target = nullptr;
        for (PersistentTarget& candidate : persistent_targets_) {
            if (!candidate.valid || !candidate.initialized ||
                candidate.key.color_info[source_rt] != source_color_info ||
                candidate.last_used_serial <= target->last_used_serial ||
                candidate.key.surface_info == target->key.surface_info) {
                continue;
            }
            RenderPassKeyBits candidate_rpkey{candidate.key.render_pass_key};
            if (!(candidate_rpkey.color_rts_used() & (1u << source_rt)) ||
                candidate_rpkey.color_view_format(source_rt) != 0u) {
                continue;
            }
            if (!overlay_target ||
                candidate.last_used_serial > overlay_target->last_used_serial) {
                overlay_target = &candidate;
            }
        }
        auto surface_pitch_tiles_at_32bpp = [](uint32_t info) {
            const uint32_t pitch_pixels = info & 0x3FFFu;
            const uint32_t msaa = (info >> 16) & 3u;
            return ((pitch_pixels << uint32_t(msaa >= 2u)) + 79u) / 80u;
        };
        PersistentTarget* underlay_target = nullptr;
        if (!overlay_target && REXCVAR_GET(ac6_native_resolve_underlay)) {
            const uint32_t target_pitch_tiles =
                surface_pitch_tiles_at_32bpp(target->key.surface_info);
            for (PersistentTarget& candidate : persistent_targets_) {
                if (!candidate.valid || !candidate.initialized ||
                    candidate.key.color_info[source_rt] != source_color_info ||
                    candidate.key.depth_info != target->key.depth_info ||
                    candidate.last_used_serial >= target->last_used_serial ||
                    candidate.key.surface_info == target->key.surface_info ||
                    surface_pitch_tiles_at_32bpp(
                        candidate.key.surface_info) != target_pitch_tiles) {
                    continue;
                }
                RenderPassKeyBits candidate_rpkey{
                    candidate.key.render_pass_key};
                if (!(candidate_rpkey.color_rts_used() &
                      (1u << source_rt)) ||
                    candidate_rpkey.color_view_format(source_rt) != 0u) {
                    continue;
                }
                if (!underlay_target ||
                    candidate.last_used_serial >
                        underlay_target->last_used_serial) {
                    underlay_target = &candidate;
                }
            }
        }
        PersistentTarget* secondary_target =
            overlay_target ? overlay_target : underlay_target;

        const bool validation_enabled =
            REXCVAR_GET(ac6_native_validate_resolves);
        const uint64_t required_swaps = uint64_t(std::max(
            REXCVAR_GET(ac6_native_propagate_after_swaps), 0));
        const bool propagate_unvalidated =
            REXCVAR_GET(ac6_native_propagate_unvalidated) &&
            g_live_gpu_swap_count.load(std::memory_order_acquire) >=
                required_swaps;
        if (!validation_enabled && propagate_unvalidated &&
            (!target->unvalidated_safe ||
             (secondary_target && !secondary_target->unvalidated_safe))) {
            if (!unsafe_resolve_reject_count_++) {
                const PersistentTarget* unsafe_target =
                    !target->unvalidated_safe ? target : secondary_target;
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: refusing unvalidated native resolve #{} because "
                    "its retained target is unsafe; source draw #{}, "
                    "reason='{}', texture fallbacks={}, set={}, binding={}, "
                    "fetch={}, format={}, vs={:016x}, ps={:016x}; using the "
                    "emulator fallback",
                    snap.capture_index,
                    unsafe_target ? unsafe_target->unsafe_capture_index : 0,
                    unsafe_target ? unsafe_target->unsafe_reason : "unknown",
                    unsafe_target
                        ? unsafe_target->unsafe_texture_fallbacks
                        : 0,
                    unsafe_target ? unsafe_target->unsafe_texture_set
                                  : UINT32_MAX,
                    unsafe_target ? unsafe_target->unsafe_texture_binding
                                  : UINT32_MAX,
                    unsafe_target ? unsafe_target->unsafe_texture_fetch
                                  : UINT32_MAX,
                    unsafe_target ? unsafe_target->unsafe_texture_format
                                  : UINT32_MAX,
                    unsafe_target ? unsafe_target->unsafe_vs_hash : 0,
                    unsafe_target ? unsafe_target->unsafe_ps_hash : 0);
            }
            if (copy_control.color_clear_enable) {
                target->initialized = false;
                target->unvalidated_safe = false;
            }
            return ExecResult::kRejected;
        }
        if (!validation_enabled && !propagate_unvalidated) {
            if (copy_control.color_clear_enable) {
                target->initialized = false;
                target->unvalidated_safe = false;
            }
            return ExecResult::kExecuted;
        }

        const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
        const uint32_t vertex_base = fetch.address * sizeof(uint32_t);
        if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size < 6 ||
            vertex_base > snap.physical_size ||
            6u * sizeof(float) > snap.physical_size - vertex_base) {
            return Reject(snap, "resolve_vertex_buffer_fallback");
        }
        float vertices[6];
        for (uint32_t i = 0; i < 6; ++i) {
            float guest_value;
            std::memcpy(&guest_value,
                        snap.physical_membase + vertex_base + i * sizeof(float),
                        sizeof(guest_value));
            vertices[i] = xenos::GpuSwap(guest_value, fetch.endian);
        }
        const float half_pixel =
            regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero
                ? 0.5f
                : 0.0f;
        auto covered_pixel = [&](float value) {
            // Equivalent to the SDK resolve path's fixed16p8 top-left rule for
            // the integer-aligned D3D resolve rectangles emitted by AC6.
            return int32_t(std::floor(value + half_pixel + 127.0f / 256.0f));
        };
        int32_t x0 = covered_pixel(std::min({vertices[0], vertices[2], vertices[4]}));
        int32_t y0 = covered_pixel(std::min({vertices[1], vertices[3], vertices[5]}));
        int32_t x1 = covered_pixel(std::max({vertices[0], vertices[2], vertices[4]}));
        int32_t y1 = covered_pixel(std::max({vertices[1], vertices[3], vertices[5]}));
        if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
            const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            x0 += window_offset.window_x_offset;
            x1 += window_offset.window_x_offset;
            y0 += window_offset.window_y_offset;
            y1 += window_offset.window_y_offset;
        }
        draw_util::Scissor scissor{};
        draw_util::GetScissor(regs, scissor, false);
        const int32_t scissor_right =
            int32_t(scissor.offset[0] + scissor.extent[0]);
        const int32_t scissor_bottom =
            int32_t(scissor.offset[1] + scissor.extent[1]);
        x0 = std::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
        x1 = std::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
        y0 = std::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
        y1 = std::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);
        x0 &= ~7;
        y0 &= ~7;
        x1 = (x1 + 7) & ~7;
        y1 = (y1 + 7) & ~7;
        const uint32_t surface_pitch = surface_info & 0x3FFFu;
        x0 = std::clamp(x0, 0, int32_t(surface_pitch & ~7u));
        x1 = std::clamp(x1, 0, int32_t(surface_pitch & ~7u));
        y0 = std::clamp(y0, 0, int32_t(target->key.height));
        y1 = std::clamp(y1, 0, int32_t(target->key.height));
        if (x0 >= x1 || y0 >= y1) {
            return ExecResult::kExecuted;
        }

        const OwnedImage& source_image =
            target->multisampled ? target->resolve[source_rt] : target->color[source_rt];
        if (!source_image.image || !source_image.view || !source_image.width ||
            !source_image.height) {
            return Reject(snap, "resolve_native_image_missing");
        }
        const uint32_t dest_base =
            regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFFu;
        const char* resolve_dump_dir_early =
            std::getenv("AC6_NR2_DUMP_RESOLVE_DIR");
        const bool resolve_diagnostics_requested =
            resolve_dump_dir_early && resolve_dump_dir_early[0];
        const uint32_t target_color_rts_used =
            RenderPassKeyBits{target->key.render_pass_key}.color_rts_used();
        const bool source_layout_is_transfer_ready =
            target_color_rts_used &&
            source_rt == uint32_t(__builtin_ctz(target_color_rts_used));
        static const bool gpu_resolve_shadow_requested = [] {
            const char* value =
                std::getenv("AC6_NR2_GPU_RESOLVE_SHADOW");
            return value && value[0] && std::strcmp(value, "0") != 0;
        }();
        bool gpu_resolve_shadow_executed = false;
        VkDeviceSize gpu_resolve_shadow_guest_base = 0;
        const bool gpu_resolve_shadow_eligible =
            gpu_resolve_shadow_requested && validation_enabled &&
            !secondary_target && !guest_memory_imported_ &&
            guest_memory_copy_mapped_ != nullptr &&
            gpu_resolve_pipeline_ != VK_NULL_HANDLE &&
            source_layout_is_transfer_ready && !(dest_base & 3u) &&
            dest_base < snap.physical_size;
        if (gpu_resolve_shadow_eligible) {
            const VkDeviceSize shadow_lower =
                VkDeviceSize(dest_base) +
                texture_util::GetTiledAddressLowerBound2D(
                    uint32_t(x0), uint32_t(y0), dest_pitch, 2);
            const VkDeviceSize shadow_upper = std::min<VkDeviceSize>(
                VkDeviceSize(dest_base) +
                    texture_util::GetTiledAddressUpperBound2D(
                        uint32_t(x1), uint32_t(y1), dest_pitch, 2),
                snap.physical_size);
            if (shadow_lower < shadow_upper &&
                EnsureHostBuffer(
                    shadow_upper - shadow_lower,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    gpu_resolve_shadow_buffer_, true)) {
                gpu_resolve_shadow_guest_base = shadow_lower;
                const ExecResult shadow_result = DispatchGpuResolve(
                    source_image, uint32_t(x0), uint32_t(y0),
                    uint32_t(x1), uint32_t(y1), dest_pitch, dest_height,
                    dest_base, bool(copy_dest_info.copy_dest_swap),
                    dest_endian, &gpu_resolve_shadow_buffer_);
                if (shadow_result == ExecResult::kFatal) {
                    return shadow_result;
                }
                gpu_resolve_shadow_executed =
                    shadow_result == ExecResult::kExecuted;
            }
        }
        const bool gpu_resolve_eligible =
            REXCVAR_GET(ac6_native_gpu_resolve) && !validation_enabled &&
            propagate_unvalidated && !secondary_target &&
            !resolve_diagnostics_requested && guest_memory_imported_ &&
            guest_memory_import_coherent_ &&
            gpu_resolve_pipeline_ != VK_NULL_HANDLE &&
            source_layout_is_transfer_ready && !(dest_base & 3u) &&
            dest_base < snap.physical_size;
        if (gpu_resolve_eligible) {
            const ExecResult gpu_result = DispatchGpuResolve(
                source_image, uint32_t(x0), uint32_t(y0), uint32_t(x1),
                uint32_t(y1), dest_pitch, dest_height, dest_base,
                bool(copy_dest_info.copy_dest_swap), dest_endian);
            if (gpu_result == ExecResult::kFatal) {
                return gpu_result;
            }
            if (gpu_result == ExecResult::kExecuted) {
                const uint32_t destination_lower =
                    texture_util::GetTiledAddressLowerBound2D(
                        uint32_t(x0), uint32_t(y0), dest_pitch, 2);
                const uint32_t destination_upper =
                    texture_util::GetTiledAddressUpperBound2D(
                        uint32_t(x1), uint32_t(y1), dest_pitch, 2);
                const uint32_t bounded_upper = uint32_t(std::min<uint64_t>(
                    destination_upper,
                    uint64_t(snap.physical_size - dest_base)));
                if (written_address && written_length &&
                    destination_lower < bounded_upper) {
                    *written_address = dest_base + destination_lower;
                    *written_length = bounded_upper - destination_lower;
                }
                if (copy_control.color_clear_enable) {
                    target->initialized = false;
                    target->unvalidated_safe = false;
                }
                if (DetailedExecutionLogsEnabled(snap)) {
                    REXLOG_CAT_INFO(
                        kLogGPU,
                        "nr2: native resolve capture #{} dispatched {} RGBA8 "
                        "pixels ({},{})-({},{}) DIRECTLY on GPU to guest "
                        "0x{:08x}, pitch {}x{} (resolve #{})",
                        snap.capture_index,
                        uint64_t(x1 - x0) * uint64_t(y1 - y0), x0, y0,
                        x1, y1, dest_base, dest_pitch, dest_height,
                        gpu_resolve_count_);
                }
                return ExecResult::kExecuted;
            }
        }
        const VkDeviceSize readback_size =
            VkDeviceSize(source_image.width) * source_image.height * 4u;
        if (!EnsureHostBuffer(readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              resolve_readback_buffer_, true)) {
            return Fatal("resolve_readback_buffer_creation_failed");
        }
        const OwnedImage* overlay_source_image = nullptr;
        if (secondary_target) {
            const OwnedImage& candidate_image =
                secondary_target->multisampled
                    ? secondary_target->resolve[source_rt]
                    : secondary_target->color[source_rt];
            if (candidate_image.image && candidate_image.width &&
                candidate_image.height &&
                EnsureHostBuffer(VkDeviceSize(candidate_image.width) *
                                     candidate_image.height * 4u,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 resolve_overlay_readback_buffer_, true)) {
                overlay_source_image = &candidate_image;
            }
        }

        if (!EnsureResolveSubmissionResources()) {
            return Fatal("resolve_submission_resource_creation_failed");
        }
        if (resolve_submission_in_flight_) {
            return Fatal("resolve_submission_still_in_flight");
        }
        if (resolve_submission_used_) {
            if (fns_.ResetFences(device_, 1, &resolve_fence_) != VK_SUCCESS) {
                return Fatal("resolve_fence_reset_failed");
            }
            if (fns_.ResetCommandPool(device_, resolve_command_pool_, 0) !=
                VK_SUCCESS) {
                return Fatal("resolve_command_pool_reset_failed");
            }
        }
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns_.BeginCommandBuffer(resolve_command_buffer_, &begin) !=
            VK_SUCCESS) {
            return Fatal("resolve_command_buffer_begin_failed");
        }
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {source_image.width, source_image.height, 1};
        fns_.CmdCopyImageToBuffer(resolve_command_buffer_, source_image.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  resolve_readback_buffer_.buffer, 1, &region);
        if (overlay_source_image) {
            VkBufferImageCopy overlay_region = {};
            overlay_region.imageSubresource =
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            overlay_region.imageExtent = {overlay_source_image->width,
                                          overlay_source_image->height, 1};
            fns_.CmdCopyImageToBuffer(
                resolve_command_buffer_, overlay_source_image->image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resolve_overlay_readback_buffer_.buffer, 1, &overlay_region);
        }
        if (fns_.EndCommandBuffer(resolve_command_buffer_) != VK_SUCCESS) {
            return Fatal("resolve_command_buffer_end_failed");
        }
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &resolve_command_buffer_;
        if (fns_.QueueSubmit(queue_, 1, &submit, resolve_fence_) != VK_SUCCESS) {
            return Fatal("resolve_queue_submit_failed");
        }
        resolve_submission_used_ = true;
        resolve_submission_in_flight_ = true;
        const VkResult wait = fns_.WaitForFences(
            device_, 1, &resolve_fence_, VK_TRUE,
            10ull * 1000 * 1000 * 1000);
        if (wait != VK_SUCCESS) {
            return Fatal("resolve_fence_wait_failed");
        }
        resolve_submission_in_flight_ = false;
        // This fence also covers every earlier fence-less draw submission on
        // the same queue. Their descriptor pools, staging buffers, texture
        // images, and command pools may now be released without a device-wide
        // idle wait.
        ReleaseDeferredDrawResources();
        DestroyDeferredBatchPools();

        uint8_t* guest_memory = const_cast<uint8_t*>(snap.physical_membase);
        // AMD's host-visible readback heap may be uncached/write-combined.
        // Copy it linearly once so the conversion and oracle scan read normal
        // cached system memory rather than issuing millions of byte loads
        // directly from the mapped Vulkan allocation.
        resolve_cpu_readback_.resize(size_t(readback_size));
        std::memcpy(resolve_cpu_readback_.data(),
                    resolve_readback_buffer_.mapped,
                    size_t(readback_size));
        const uint8_t* source = resolve_cpu_readback_.data();
        const uint8_t* overlay_source = nullptr;
        if (overlay_source_image) {
            const size_t overlay_readback_size =
                size_t(overlay_source_image->width) *
                overlay_source_image->height * 4u;
            resolve_cpu_overlay_readback_.resize(overlay_readback_size);
            std::memcpy(resolve_cpu_overlay_readback_.data(),
                        resolve_overlay_readback_buffer_.mapped,
                        overlay_readback_size);
            overlay_source = resolve_cpu_overlay_readback_.data();
        }
        auto get_surface_layout = [](uint32_t info, uint32_t& pitch_tiles,
                                     uint32_t& samples_x_log2,
                                     uint32_t& samples_y_log2) {
            const uint32_t pitch_pixels = info & 0x3FFFu;
            const uint32_t msaa = (info >> 16) & 3u;
            samples_x_log2 = uint32_t(msaa >= 2u);
            samples_y_log2 = uint32_t(msaa >= 1u);
            pitch_tiles =
                ((pitch_pixels << samples_x_log2) + 79u) / 80u;
        };
        uint32_t resolve_pitch_tiles = 0, resolve_samples_x_log2 = 0,
                 resolve_samples_y_log2 = 0;
        get_surface_layout(surface_info, resolve_pitch_tiles,
                           resolve_samples_x_log2,
                           resolve_samples_y_log2);
        uint32_t overlay_pitch_tiles = 0, overlay_samples_x_log2 = 0,
                 overlay_samples_y_log2 = 0;
        if (secondary_target) {
            get_surface_layout(secondary_target->key.surface_info,
                               overlay_pitch_tiles,
                               overlay_samples_x_log2,
                               overlay_samples_y_log2);
        }
        const uint32_t source_base_tiles = source_color_info & 0x7FFu;
        const std::vector<uint32_t>* secondary_source_offsets = nullptr;
        if (overlay_source && overlay_pitch_tiles) {
            const std::array<uint32_t, 7> secondary_offset_key = {
                surface_info,
                secondary_target->key.surface_info,
                source_base_tiles,
                overlay_source_image->width,
                overlay_source_image->height,
                dest_pitch,
                dest_height,
            };
            // Fullscreen remaps are several MiB each. Keep the cache bounded
            // during long gameplay sessions with many render-target layouts.
            if (resolve_secondary_offset_cache_.find(secondary_offset_key) ==
                    resolve_secondary_offset_cache_.end() &&
                resolve_secondary_offset_cache_.size() >= 8u) {
                resolve_secondary_offset_cache_.erase(
                    resolve_secondary_offset_cache_.begin());
            }
            auto [secondary_offset_it, secondary_offset_inserted] =
                resolve_secondary_offset_cache_.try_emplace(
                    secondary_offset_key);
            std::vector<uint32_t>& secondary_offsets =
                secondary_offset_it->second;
            const size_t secondary_offset_count =
                size_t(dest_pitch) * size_t(dest_height);
            if (secondary_offset_inserted ||
                secondary_offsets.size() != secondary_offset_count) {
                secondary_offsets.assign(secondary_offset_count, UINT32_MAX);
                for (uint32_t secondary_y = 0; secondary_y < dest_height;
                     ++secondary_y) {
                    for (uint32_t secondary_x = 0; secondary_x < dest_pitch;
                         ++secondary_x) {
                        const uint32_t sample_x =
                            secondary_x << resolve_samples_x_log2;
                        const uint32_t sample_y =
                            secondary_y << resolve_samples_y_log2;
                        const uint32_t tile_x = sample_x / 80u;
                        const uint32_t tile_y = sample_y / 16u;
                        const uint32_t tile =
                            (source_base_tiles +
                             tile_y * resolve_pitch_tiles + tile_x) &
                            0x7FFu;
                        const uint32_t overlay_relative_tile =
                            (tile - source_base_tiles) & 0x7FFu;
                        const uint32_t overlay_tile_x =
                            overlay_relative_tile % overlay_pitch_tiles;
                        const uint32_t overlay_tile_y =
                            overlay_relative_tile / overlay_pitch_tiles;
                        const uint32_t overlay_sample_x =
                            overlay_tile_x * 80u + sample_x % 80u;
                        const uint32_t overlay_sample_y =
                            overlay_tile_y * 16u + sample_y % 16u;
                        const uint32_t overlay_x =
                            overlay_sample_x >> overlay_samples_x_log2;
                        const uint32_t overlay_y =
                            overlay_sample_y >> overlay_samples_y_log2;
                        if (overlay_x < overlay_source_image->width &&
                            overlay_y < overlay_source_image->height) {
                            secondary_offsets[
                                size_t(secondary_y) * dest_pitch +
                                secondary_x] =
                                uint32_t((size_t(overlay_y) *
                                          overlay_source_image->width +
                                          overlay_x) *
                                         4u);
                        }
                    }
                }
                REXLOG_CAT_INFO(
                    kLogGPU,
                    "nr2: cached {}x{} physical-view resolve remap ({} "
                    "entries)",
                    dest_pitch, dest_height, secondary_offsets.size());
            }
            secondary_source_offsets = &secondary_offsets;
        }
        const uint64_t tiled_offset_key =
            (uint64_t(dest_pitch) << 32) | uint64_t(dest_height);
        auto [tiled_offset_it, tiled_offset_inserted] =
            resolve_tiled_offset_cache_.try_emplace(tiled_offset_key);
        std::vector<uint32_t>& tiled_offsets = tiled_offset_it->second;
        const size_t tiled_offset_count =
            size_t(dest_pitch) * size_t(dest_height);
        if (tiled_offset_inserted ||
            tiled_offsets.size() != tiled_offset_count) {
            tiled_offsets.resize(tiled_offset_count);
            for (uint32_t tiled_y = 0; tiled_y < dest_height; ++tiled_y) {
                for (uint32_t tiled_x = 0; tiled_x < dest_pitch; ++tiled_x) {
                    tiled_offsets[size_t(tiled_y) * dest_pitch + tiled_x] =
                        texture_util::GetTiledOffset2D(
                            tiled_x, tiled_y, dest_pitch, 2);
                }
            }
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: cached {}x{} RGBA8 guest tiled-offset map ({} entries)",
                dest_pitch, dest_height, tiled_offsets.size());
        }
        uint64_t pixels_written = 0;
        uint64_t oracle_matches = 0;
        uint64_t oracle_mismatches = 0;
        uint64_t oracle_rgb_matches = 0;
        uint64_t oracle_alpha_matches = 0;
        uint64_t oracle_within_tolerance = 0;
        uint64_t oracle_composed_secondary_pixels = 0;
        const char* resolve_dump_dir =
            std::getenv("AC6_NR2_DUMP_RESOLVE_DIR");
        uint64_t resolve_dump_min_capture = 0;
        if (const char* min_capture =
                std::getenv("AC6_NR2_DUMP_RESOLVE_MIN_CAPTURE")) {
            resolve_dump_min_capture = std::strtoull(min_capture, nullptr, 0);
        }
        const bool resolve_dump_once = [] {
            const char* value = std::getenv("AC6_NR2_DUMP_RESOLVE_ONCE");
            return value && value[0] && std::strcmp(value, "0") != 0;
        }();
        const bool dump_resolve_images = resolve_dump_dir &&
            resolve_dump_dir[0] &&
            snap.capture_index >= resolve_dump_min_capture &&
            (!resolve_dump_once || !resolve_dump_once_completed_);
        const size_t resolve_dump_pixel_count =
            size_t(dest_pitch) * size_t(dest_height);
        std::vector<uint8_t> resolve_dump_native;
        std::vector<uint8_t> resolve_dump_oracle;
        std::vector<uint8_t> resolve_dump_mismatch;
        if (dump_resolve_images) {
            resolve_dump_native.resize(resolve_dump_pixel_count * 3u);
            resolve_dump_oracle.resize(resolve_dump_pixel_count * 3u);
            resolve_dump_mismatch.resize(resolve_dump_pixel_count * 3u);
        }
        uint32_t mismatch_min_x = UINT32_MAX;
        uint32_t mismatch_min_y = UINT32_MAX;
        uint32_t mismatch_max_x = 0;
        uint32_t mismatch_max_y = 0;
        const int32_t oracle_tolerance = std::clamp(
            REXCVAR_GET(ac6_native_resolve_oracle_tolerance), 0, 255);
        std::vector<std::pair<uint32_t, uint32_t>>& pending_writes =
            resolve_pending_writes_;
        pending_writes.clear();
        if (validation_enabled || dump_resolve_images) {
            pending_writes.reserve(size_t(x1 - x0) * size_t(y1 - y0));
        }
        auto pack_resolve_pixel = [&](const uint8_t* rgba) {
            const uint8_t r = rgba[0];
            const uint8_t g = rgba[1];
            const uint8_t b = rgba[2];
            const uint8_t a = rgba[3];
            const bool swap_rb = copy_dest_info.copy_dest_swap;
            if (dest_endian == 2u) {
                return uint32_t(a) |
                    (uint32_t(swap_rb ? r : b) << 8) |
                    (uint32_t(g) << 16) |
                    (uint32_t(swap_rb ? b : r) << 24);
            }
            return uint32_t(swap_rb ? b : r) |
                (uint32_t(g) << 8) |
                (uint32_t(swap_rb ? r : b) << 16) |
                (uint32_t(a) << 24);
        };
        uint32_t first_written_address = UINT32_MAX;
        uint32_t last_written_end = 0;
        for (int32_t y = y0; y < y1 && uint32_t(y) < dest_height; ++y) {
            for (int32_t x = x0; x < x1 && uint32_t(x) < dest_pitch; ++x) {
                if (uint32_t(x) >= source_image.width ||
                    uint32_t(y) >= source_image.height) {
                    continue;
                }
                const uint64_t tiled_offset = tiled_offsets[
                    size_t(uint32_t(y)) * dest_pitch + uint32_t(x)];
                if (dest_base >= snap.physical_size ||
                    tiled_offset > snap.physical_size - dest_base ||
                    4u > snap.physical_size - dest_base - tiled_offset) {
                    continue;
                }
                const uint8_t* primary_rgba =
                    source + (size_t(y) * source_image.width + uint32_t(x)) * 4u;
                const uint8_t* rgba = primary_rgba;
                const uint8_t* secondary_rgba = nullptr;
                if (secondary_source_offsets) {
                    const uint32_t secondary_offset =
                        (*secondary_source_offsets)[
                            size_t(uint32_t(y)) * dest_pitch + uint32_t(x)];
                    if (secondary_offset != UINT32_MAX) {
                        const uint8_t* overlay_rgba =
                            overlay_source + secondary_offset;
                        secondary_rgba = overlay_rgba;
                        // Newly allocated native views are cleared to zero.
                        // Nonzero pixels therefore identify physical samples
                        // written by the newer view; zero keeps the exact-view
                        // fallback. The 100% emulator-oracle gate below makes
                        // this conservative composition fail safe.
                        const bool primary_nonzero =
                            rgba[0] || rgba[1] || rgba[2] || rgba[3];
                        const bool secondary_nonzero =
                            overlay_rgba[0] || overlay_rgba[1] ||
                            overlay_rgba[2] || overlay_rgba[3];
                        if ((overlay_target && secondary_nonzero) ||
                            (underlay_target && !primary_nonzero)) {
                            rgba = overlay_rgba;
                        }
                    }
                }
                uint8_t* guest_pixel = guest_memory + dest_base + tiled_offset;
                // Fast path for the overwhelmingly common live fullscreen
                // resolve: no cross-view composition, no diagnostic image,
                // and either no oracle or a byte-exact (tolerance-zero) gate.
                // Compare packed dwords directly and avoid the general
                // per-channel conversion/abs/memcmp machinery.
                if (!overlay_source && !dump_resolve_images &&
                    (!validation_enabled || oracle_tolerance == 0)) {
                    const uint32_t packed_word =
                        pack_resolve_pixel(primary_rgba);
                    const uint32_t pixel_address =
                        uint32_t(dest_base + tiled_offset);
                    if (!validation_enabled) {
                        std::memcpy(guest_pixel, &packed_word,
                                    sizeof(packed_word));
                        first_written_address =
                            std::min(first_written_address, pixel_address);
                        last_written_end =
                            std::max(last_written_end, pixel_address + 4u);
                        ++pixels_written;
                        continue;
                    }
                    if (validation_enabled) {
                        uint32_t oracle_word;
                        std::memcpy(&oracle_word, guest_pixel,
                                    sizeof(oracle_word));
                        const bool exact = oracle_word == packed_word;
                        oracle_matches += uint64_t(exact);
                        oracle_mismatches += uint64_t(!exact);
                        oracle_within_tolerance += uint64_t(exact);
                        const uint32_t alpha_mask =
                            dest_endian == 2u ? 0x000000FFu : 0xFF000000u;
                        const uint32_t rgb_mask = ~alpha_mask;
                        oracle_rgb_matches += uint64_t(
                            (oracle_word & rgb_mask) ==
                            (packed_word & rgb_mask));
                        oracle_alpha_matches += uint64_t(
                            (oracle_word & alpha_mask) ==
                            (packed_word & alpha_mask));
                    }
                    pending_writes.emplace_back(pixel_address, packed_word);
                    first_written_address =
                        std::min(first_written_address, pixel_address);
                    last_written_end =
                        std::max(last_written_end, pixel_address + 4u);
                    ++pixels_written;
                    continue;
                }
                if (!validation_enabled && !dump_resolve_images) {
                    const uint32_t packed_word = pack_resolve_pixel(rgba);
                    std::memcpy(guest_pixel, &packed_word,
                                sizeof(packed_word));
                    const uint32_t pixel_address =
                        uint32_t(dest_base + tiled_offset);
                    first_written_address =
                        std::min(first_written_address, pixel_address);
                    last_written_end =
                        std::max(last_written_end, pixel_address + 4u);
                    ++pixels_written;
                    continue;
                }
                uint8_t oracle_rgba[4];
                if (dest_endian == 2u) {
                    oracle_rgba[0] = copy_dest_info.copy_dest_swap
                                         ? guest_pixel[1]
                                         : guest_pixel[3];
                    oracle_rgba[1] = guest_pixel[2];
                    oracle_rgba[2] = copy_dest_info.copy_dest_swap
                                         ? guest_pixel[3]
                                         : guest_pixel[1];
                    oracle_rgba[3] = guest_pixel[0];
                } else {
                    oracle_rgba[0] = copy_dest_info.copy_dest_swap
                                         ? guest_pixel[2]
                                         : guest_pixel[0];
                    oracle_rgba[1] = guest_pixel[1];
                    oracle_rgba[2] = copy_dest_info.copy_dest_swap
                                         ? guest_pixel[0]
                                         : guest_pixel[2];
                    oracle_rgba[3] = guest_pixel[3];
                }
                if (validation_enabled && secondary_rgba &&
                    REXCVAR_GET(ac6_native_resolve_oracle_compose)) {
                    auto oracle_distance = [&](const uint8_t* candidate) {
                        uint32_t distance = 0;
                        for (uint32_t component = 0; component < 4;
                             ++component) {
                            distance += uint32_t(std::abs(
                                int32_t(oracle_rgba[component]) -
                                int32_t(candidate[component])));
                        }
                        return distance;
                    };
                    if (oracle_distance(secondary_rgba) <
                        oracle_distance(primary_rgba)) {
                        rgba = secondary_rgba;
                        ++oracle_composed_secondary_pixels;
                    } else {
                        rgba = primary_rgba;
                    }
                }
                uint8_t packed[4];
                const uint8_t r =
                    copy_dest_info.copy_dest_swap ? rgba[2] : rgba[0];
                const uint8_t g = rgba[1];
                const uint8_t b =
                    copy_dest_info.copy_dest_swap ? rgba[0] : rgba[2];
                const uint8_t a = rgba[3];
                if (dest_endian == 2u) {
                    packed[0] = a;
                    packed[1] = b;
                    packed[2] = g;
                    packed[3] = r;
                } else {
                    packed[0] = r;
                    packed[1] = g;
                    packed[2] = b;
                    packed[3] = a;
                }
                if (std::memcmp(oracle_rgba, rgba, 3) == 0) {
                    ++oracle_rgb_matches;
                }
                if (oracle_rgba[3] == rgba[3]) {
                    ++oracle_alpha_matches;
                }
                if (std::memcmp(guest_pixel, packed, 4) == 0) {
                    ++oracle_matches;
                } else {
                    ++oracle_mismatches;
                    mismatch_min_x = std::min(mismatch_min_x, uint32_t(x));
                    mismatch_min_y = std::min(mismatch_min_y, uint32_t(y));
                    mismatch_max_x = std::max(mismatch_max_x, uint32_t(x));
                    mismatch_max_y = std::max(mismatch_max_y, uint32_t(y));
                }
                if (dump_resolve_images) {
                    const size_t dump_offset =
                        (size_t(y) * dest_pitch + uint32_t(x)) * 3u;
                    std::memcpy(resolve_dump_native.data() + dump_offset,
                                rgba, 3u);
                    std::memcpy(resolve_dump_oracle.data() + dump_offset,
                                oracle_rgba, 3u);
                    const bool mismatch =
                        std::memcmp(oracle_rgba, rgba, 4u) != 0;
                    resolve_dump_mismatch[dump_offset + 0] =
                        mismatch ? 255u : 0u;
                    resolve_dump_mismatch[dump_offset + 1] = 0u;
                    resolve_dump_mismatch[dump_offset + 2] = 0u;
                }
                bool within_tolerance = true;
                for (uint32_t component = 0; component < 4; ++component) {
                    if (std::abs(int32_t(oracle_rgba[component]) -
                                 int32_t(rgba[component])) > oracle_tolerance) {
                        within_tolerance = false;
                        break;
                    }
                }
                oracle_within_tolerance += uint64_t(within_tolerance);
                uint32_t packed_word;
                std::memcpy(&packed_word, packed, sizeof(packed_word));
                const uint32_t pixel_address =
                    uint32_t(dest_base + tiled_offset);
                pending_writes.emplace_back(pixel_address, packed_word);
                first_written_address =
                    std::min(first_written_address, pixel_address);
                last_written_end =
                    std::max(last_written_end, pixel_address + 4u);
                ++pixels_written;
            }
        }
        if (gpu_resolve_shadow_executed) {
            uint64_t shadow_matches = 0;
            uint64_t shadow_mismatches = 0;
            uint32_t first_shadow_mismatch = UINT32_MAX;
            for (const auto& [address, expected] : pending_writes) {
                const VkDeviceSize absolute_address = address;
                if (absolute_address < gpu_resolve_shadow_guest_base ||
                    absolute_address - gpu_resolve_shadow_guest_base >
                        gpu_resolve_shadow_buffer_.size ||
                    sizeof(uint32_t) >
                        gpu_resolve_shadow_buffer_.size -
                            (absolute_address -
                             gpu_resolve_shadow_guest_base)) {
                    ++shadow_mismatches;
                    first_shadow_mismatch =
                        std::min(first_shadow_mismatch, address);
                    continue;
                }
                uint32_t actual;
                std::memcpy(
                    &actual,
                    static_cast<const uint8_t*>(
                        gpu_resolve_shadow_buffer_.mapped) +
                        (absolute_address -
                         gpu_resolve_shadow_guest_base),
                    sizeof(actual));
                const bool exact = actual == expected;
                shadow_matches += uint64_t(exact);
                shadow_mismatches += uint64_t(!exact);
                if (!exact) {
                    first_shadow_mismatch =
                        std::min(first_shadow_mismatch, address);
                }
            }
            if (shadow_mismatches) {
                REXLOG_CAT_ERROR(
                    kLogGPU,
                    "nr2: GPU resolve shadow #{} FAILED: {} exact "
                    "packed/tiled words, {} mismatches; first guest "
                    "address 0x{:08x}",
                    snap.capture_index, shadow_matches, shadow_mismatches,
                    first_shadow_mismatch);
                return Fatal("gpu_resolve_shadow_mismatch");
            }
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: GPU resolve shadow #{} passed: {} exact "
                "packed/tiled words",
                snap.capture_index, shadow_matches);
        }
        const uint32_t min_match_percent = uint32_t(std::clamp(
            REXCVAR_GET(ac6_native_resolve_min_oracle_match_percent), 0, 100));
        const bool propagate = pixels_written &&
            (!validation_enabled ||
             oracle_within_tolerance * 100u >=
                 pixels_written * min_match_percent);
        if (propagate) {
            for (const auto& [address, value] : pending_writes) {
                std::memcpy(guest_memory + address, &value, sizeof(value));
            }
            if (written_address && written_length &&
                first_written_address < last_written_end) {
                *written_address = first_written_address;
                *written_length = last_written_end - first_written_address;
            }
        }
        if (copy_control.color_clear_enable) {
            // The following use must not load the pre-clear native color.
            // ExecuteCapturedDraw will recreate/clear this target on demand.
            target->initialized = false;
            target->unvalidated_safe = false;
        }
        if (validation_enabled &&
            REXCVAR_GET(ac6_native_resolve_oracle_compose) &&
            overlay_source_image) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: resolve #{} oracle-guided alias diagnostic selected "
                "the compatible view for {} of {} pixels",
                snap.capture_index, oracle_composed_secondary_pixels,
                pixels_written);
        }
        if (dump_resolve_images) {
            std::error_code dump_ec;
            std::filesystem::create_directories(resolve_dump_dir, dump_ec);
            auto write_ppm = [&](const char* suffix,
                                 const std::vector<uint8_t>& rgb) {
                const std::filesystem::path dump_path =
                    std::filesystem::path(resolve_dump_dir) /
                    ("resolve-" + std::to_string(snap.capture_index) + "-" +
                     suffix + ".ppm");
                FILE* dump_file = std::fopen(dump_path.string().c_str(), "wb");
                if (!dump_file) {
                    return false;
                }
                std::fprintf(dump_file, "P6\n%u %u\n255\n", dest_pitch,
                             dest_height);
                const bool wrote =
                    std::fwrite(rgb.data(), rgb.size(), 1, dump_file) == 1;
                std::fclose(dump_file);
                return wrote;
            };
            const bool dump_ok = !dump_ec &&
                write_ppm("native", resolve_dump_native) &&
                write_ppm("oracle", resolve_dump_oracle) &&
                write_ppm("mismatch", resolve_dump_mismatch);
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: resolve #{} diagnostic images {} in {}{}",
                snap.capture_index, dump_ok ? "written" : "FAILED",
                resolve_dump_dir,
                oracle_mismatches
                    ? ("; mismatch bounds (" +
                       std::to_string(mismatch_min_x) + "," +
                       std::to_string(mismatch_min_y) + ")-(" +
                       std::to_string(mismatch_max_x + 1u) + "," +
                       std::to_string(mismatch_max_y + 1u) + ")")
                    : std::string());
            if (resolve_dump_once) {
                resolve_dump_once_completed_ = true;
            }
        }
        if (DetailedExecutionLogsEnabled(snap)) {
            if (!validation_enabled) {
                REXLOG_CAT_INFO(
                    kLogGPU,
                    "nr2: native resolve capture #{} propagated {} RGBA8 pixels "
                    "({},{})-({},{}) to guest 0x{:08x}, pitch {}x{} WITHOUT "
                    "emulator validation{}",
                    snap.capture_index, pixels_written, x0, y0, x1, y1,
                    dest_base, dest_pitch, dest_height,
                    overlay_source_image
                        ? (overlay_target
                               ? " (physical-view overlay composed)"
                               : " (physical-view underlay composed)")
                        : "");
            } else {
                REXLOG_CAT_INFO(
                    kLogGPU,
                    "nr2: native resolve capture #{} wrote {} RGBA8 pixels "
                    "({},{})-({},{}) to guest 0x{:08x}, pitch {}x{}, swap={}, endian={}; "
                    "emulator oracle: {} RGBA match, {} mismatch; {} RGB match, "
                    "{} alpha match; {} within +/-{} RGBA; {} at {}% gate{}",
                    snap.capture_index, pixels_written, x0, y0, x1, y1,
                    dest_base, dest_pitch, dest_height,
                    uint32_t(copy_dest_info.copy_dest_swap), dest_endian,
                    oracle_matches, oracle_mismatches, oracle_rgb_matches,
                    oracle_alpha_matches, oracle_within_tolerance,
                    oracle_tolerance,
                    propagate ? "PROPAGATED" : "EMULATOR FALLBACK",
                    min_match_percent,
                    overlay_source_image
                        ? (overlay_target
                               ? " (physical-view overlay composed)"
                               : " (physical-view underlay composed)")
                        : "");
            }
        }
        return propagate ? ExecResult::kExecuted : ExecResult::kRejected;
    }

    // Executes the snapshotted draw natively. kRejected means "this specific
    // draw is unsuitable, try the next captured one"; kFatal disarms the
    // trigger entirely.
    ExecResult ExecuteCapturedDraw(const DrawSnapshot& snap) {
        using namespace rex::graphics;
        if (session_failed_) {
            return ExecResult::kFatal;
        }
        if (device_ == VK_NULL_HANDLE) {
            return Fatal("prover_not_ready");
        }
        if (snap.regs.size() < RegisterFile::kRegisterCount) {
            return Fatal("register_snapshot_short");
        }
        if (!snap.physical_membase || !snap.physical_size) {
            return Fatal("no_guest_memory_pointer");
        }

        auto regs_storage = std::make_unique<RegisterFile>();
        std::memcpy(regs_storage->values, snap.regs.data(),
                    sizeof(uint32_t) * RegisterFile::kRegisterCount);
        const RegisterFile& regs = *regs_storage;

        // --- Decode the draw the emulated GPU most recently processed. ---
        auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
        const uint32_t index_count = vgt_draw_initiator.num_indices;
        if (!index_count) {
            return Reject(snap, "zero_index_count");
        }
        bool indexed = false;
        switch (vgt_draw_initiator.source_select) {
            case xenos::SourceSelect::kDMA:
                indexed = true;
                break;
            case xenos::SourceSelect::kAutoIndex:
                indexed = false;
                break;
            default:
                return Reject(snap, "immediate_index_source");
        }
        // Host mapping of the guest primitive type. Point and rectangle lists
        // start by matching their stored geometry-shader pipeline, then use
        // the SDK's VS-expansion fallback: HostVertexShaderType 9/10, triangle
        // strip topology, and the builtin "two-triangle strips" index pattern
        // (4 host vertices per guest primitive, primitive restart between).
        uint32_t expected_topology;
        uint32_t expected_host_type = 0;       // Shader::HostVertexShaderType
        uint32_t expected_geometry_shader = 0; // PipelineGeometryShader
        uint32_t fallback_strip_host_type = 0; // 9 point, 10 rectangle
        uint32_t builtin_guest_vertices_per_primitive = 0;
        bool builtin_strip = false;
        bool fan_convert = false;              // triangle fan CPU-converted to a list
        bool quad_convert = false;             // quad list CPU-converted to a triangle list
        switch (vgt_draw_initiator.prim_type) {
            case xenos::PrimitiveType::kLineList:
                expected_topology = kTopologyLineList;
                break;
            case xenos::PrimitiveType::kLineStrip:
                expected_topology = kTopologyLineStrip;
                break;
            case xenos::PrimitiveType::kTriangleList:
                expected_topology = kTopologyTriangleList;
                break;
            case xenos::PrimitiveType::kTriangleFan:
                // Vulkan has no triangle-fan topology. The SDK primitive
                // processor always converts fans to triangle lists, and the
                // stored pipeline therefore already has list topology.
                expected_topology = kTopologyTriangleList;
                fan_convert = true;
                break;
            case xenos::PrimitiveType::kTriangleStrip:
                expected_topology = kTopologyTriangleStrip;
                break;
            case xenos::PrimitiveType::kPointList:
                expected_topology = kTopologyPointList;
                expected_geometry_shader = 1;  // PipelineGeometryShader::kPointList
                fallback_strip_host_type = 9;  // kPointListAsTriangleStrip
                builtin_guest_vertices_per_primitive = 1;
                builtin_strip = true;
                break;
            case xenos::PrimitiveType::kRectangleList:
                expected_topology = kTopologyTriangleList;
                expected_geometry_shader = 2;  // PipelineGeometryShader::kRectangleList
                fallback_strip_host_type = 10;  // kRectangleListAsTriangleStrip
                builtin_guest_vertices_per_primitive = 3;
                builtin_strip = true;
                break;
            case xenos::PrimitiveType::kQuadList:
                // The store's quad pipelines use the runtime quad geometry
                // shader (line-list-with-adjacency input). Slice 2 instead
                // converts quads to triangle lists on the CPU (the SDK's own
                // convert_quad_lists_to_triangle_lists pattern: 012, 023) and
                // draws with a triangle-list variant of the stored pipeline.
                expected_topology = UINT32_MAX;  // stored topology is lineadj
                expected_geometry_shader = 3;    // PipelineGeometryShader::kQuadList
                quad_convert = true;
                break;
            default: {
                char reason[48];
                std::snprintf(reason, sizeof(reason), "unsupported_prim_type_%u",
                              uint32_t(vgt_draw_initiator.prim_type));
                return Reject(snap, reason);
            }
        }
        if (!snap.vs.valid || !snap.vs.ucode_analyzed) {
            return Reject(snap, "vs_unavailable");
        }
        if (!snap.ps.valid || !snap.ps.ucode_analyzed) {
            return Reject(snap, "ps_unavailable");
        }

        // --- Select the native pipeline: match the stored descriptions by
        // shader identity + topology, then rank the stored state variants.
        // Among matches, prefer the one whose render-pass key RT0 format is
        // the format the registers currently program (the same shader pair is
        // stored once per RT format it ever rendered to - e.g. the D3D Clear
        // shaders exist against 8888 and 32_FLOAT targets), then a
        // primitive-restart state matching the current registers. ---
        auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        const reg::RB_DEPTHCONTROL normalized_depth_control =
            draw_util::GetNormalizedDepthControl(regs);
        const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
        const bool want_cull_front =
            primitive_polygonal && pa_su_sc_mode_cntl.cull_front;
        const bool want_cull_back =
            primitive_polygonal && pa_su_sc_mode_cntl.cull_back;
        const bool want_front_face_clockwise =
            primitive_polygonal && pa_su_sc_mode_cntl.face;
        const bool want_restart =
            pa_su_sc_mode_cntl.multi_prim_ib_ena &&
            (expected_topology == kTopologyLineStrip ||
             expected_topology == kTopologyTriangleStrip);
        const uint32_t reg_rt0_format =
            uint32_t(regs.Get<reg::RB_COLOR_INFO>().color_format);
        const uint32_t reg_msaa_samples =
            uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
        const StoredPipeline* chosen_description = nullptr;
        bool chosen_is_preexpanded = false;
        int chosen_score = -1;
        size_t match_count = 0;
        for (const auto& stored_entry : stored_pipelines_) {
            const StoredPipeline& d = stored_entry.second;
            if (d.vs_hash != snap.vs.ucode_hash || d.ps_hash != snap.ps.ucode_hash) {
                continue;
            }
            const uint32_t stored_host_type =
                (uint32_t(d.vs_modification >> 32)) & 0xFu;
            const bool is_preexpanded =
                builtin_strip && stored_host_type == fallback_strip_host_type &&
                d.state.geometry_shader() == kGeometryShaderNone &&
                d.state.primitive_topology() == kTopologyTriangleStrip &&
                d.state.primitive_restart();
            const bool is_base =
                stored_host_type == expected_host_type &&
                (expected_topology == UINT32_MAX ||
                 d.state.primitive_topology() == expected_topology) &&
                d.state.geometry_shader() == expected_geometry_shader;
            if ((!is_base && !is_preexpanded) || d.state.rasterizer_discard()) {
                continue;
            }
            ++match_count;
            // Prefer an already-expanded SDK pipeline. Point sprites in AC6's
            // cache are stored this way, while rectangle lists are stored in
            // runtime-GS form and need a variant generated below.
            int score = is_preexpanded ? 4 : 0;
            // A pipeline created for the wrong sample count is not merely a
            // close state variant: it renders into a different physical EDRAM
            // view. Prefer an exact live RB_SURFACE_INFO sample count before
            // every raster/depth/blend tie-breaker below. If the cache has no
            // exact variant we still retain the old fallback behavior.
            if (d.render_pass_key.msaa_samples() == reg_msaa_samples) {
                score += 256;
            }
            if ((d.render_pass_key.color_rts_used() & 1u) &&
                d.render_pass_key.color_view_format(0) == reg_rt0_format) {
                score += 2;
            }
            if (d.state.primitive_restart() == uint32_t(want_restart)) {
                score += 1;
            }
            // Shader identity and topology aren't a complete pipeline key.
            // AC6 stores variants of the same shader pair for culling and
            // depth/stencil state. Prefer the variant matching the normalized
            // register state, exactly as VulkanPipelineCache does; choosing a
            // tied stencil-enabled variant for a stencil-disabled draw can
            // reject every fragment while still reporting a successful draw.
            if (d.state.cull_front() == uint32_t(want_cull_front)) {
                score += 4;
            }
            if (d.state.cull_back() == uint32_t(want_cull_back)) {
                score += 4;
            }
            if (d.state.front_face_clockwise() ==
                uint32_t(want_front_face_clockwise)) {
                score += 2;
            }
            if (d.render_pass_key.depth_used()) {
                const uint32_t want_depth_write =
                    normalized_depth_control.z_enable &&
                    normalized_depth_control.z_write_enable;
                const uint32_t want_depth_compare =
                    normalized_depth_control.z_enable
                        ? uint32_t(normalized_depth_control.zfunc)
                        : 7u;  // xenos::CompareFunction::kAlways
                if (d.state.depth_write_enable() == want_depth_write) {
                    score += 8;
                }
                if (d.state.depth_compare_op() == want_depth_compare) {
                    score += 8;
                }
                const uint32_t want_stencil =
                    normalized_depth_control.stencil_enable;
                if (d.state.stencil_test_enable() == want_stencil) {
                    score += 16;
                }
                if (want_stencil && d.state.stencil_test_enable()) {
                    const bool front_matches =
                        d.state.stencil_front_fail_op() ==
                            uint32_t(normalized_depth_control.stencilfail) &&
                        d.state.stencil_front_pass_op() ==
                            uint32_t(normalized_depth_control.stencilzpass) &&
                        d.state.stencil_front_depth_fail_op() ==
                            uint32_t(normalized_depth_control.stencilzfail) &&
                        d.state.stencil_front_compare_op() ==
                            uint32_t(normalized_depth_control.stencilfunc);
                    const bool separate_back =
                        primitive_polygonal &&
                        normalized_depth_control.backface_enable;
                    const bool back_matches =
                        d.state.stencil_back_fail_op() ==
                            uint32_t(separate_back
                                         ? normalized_depth_control.stencilfail_bf
                                         : normalized_depth_control.stencilfail) &&
                        d.state.stencil_back_pass_op() ==
                            uint32_t(separate_back
                                         ? normalized_depth_control.stencilzpass_bf
                                         : normalized_depth_control.stencilzpass) &&
                        d.state.stencil_back_depth_fail_op() ==
                            uint32_t(separate_back
                                         ? normalized_depth_control.stencilzfail_bf
                                         : normalized_depth_control.stencilzfail) &&
                        d.state.stencil_back_compare_op() ==
                            uint32_t(separate_back
                                         ? normalized_depth_control.stencilfunc_bf
                                         : normalized_depth_control.stencilfunc);
                    if (front_matches && back_matches) {
                        score += 16;
                    }
                }
            }
            for (uint32_t rt = 0; rt < 4; ++rt) {
                if (!(d.render_pass_key.color_rts_used() & (1u << rt))) {
                    continue;
                }
                const uint32_t want_write_mask =
                    (regs[XE_GPU_REG_RB_COLOR_MASK] >> (rt * 4)) & 0xFu;
                const reg::RB_BLENDCONTROL blend_control =
                    regs.Get<reg::RB_BLENDCONTROL>(
                        reg::RB_BLENDCONTROL::rt_register_indices[rt]);
                const uint32_t want_rt_bits =
                    GetExpectedRenderTargetBits(blend_control, want_write_mask);
                if (d.render_targets[rt].bits == want_rt_bits) {
                    score += 16;
                } else if (d.render_targets[rt].color_write_mask() ==
                           want_write_mask) {
                    score += 2;
                }
            }
            if (score > chosen_score) {
                chosen_description = &d;
                chosen_is_preexpanded = is_preexpanded;
                chosen_score = score;
            }
        }
        if (!chosen_description) {
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: capture #{}: no stored pipeline for vs={:016x} ps={:016x} "
                            "topology={} host_type={} - retrying with the next captured draw",
                            snap.capture_index, snap.vs.ucode_hash, snap.ps.ucode_hash,
                            expected_topology, expected_host_type);
            return Reject(snap, "no_matching_stored_pipeline");
        }
        auto chosen_it = pipelines_.find(chosen_description->description_hash);
        if (chosen_it == pipelines_.end()) {
            std::string fail_reason;
            if (!CreatePipeline(*chosen_description, fail_reason)) {
                REXLOG_CAT_WARN(
                    kLogGPU,
                    "nr2: capture #{}: selected stored pipeline could not be "
                    "created on first use ({})",
                    snap.capture_index, fail_reason);
                return Reject(snap, "selected_pipeline_creation_failed");
            }
            chosen_it = pipelines_.find(chosen_description->description_hash);
            if (chosen_it == pipelines_.end()) {
                return Fatal("created_pipeline_missing_from_cache");
            }
        }
        const NativePipeline* chosen = &chosen_it->second;
        // The pipeline actually executed: a triangle-strip VS-expansion
        // variant for point/rectangle lists, or a triangle-list variant for
        // CPU-converted quad lists.
        const NativePipeline* pipe = chosen;
        const bool diagnostic_rectangle_base_vs =
            builtin_strip && fallback_strip_host_type == 10u &&
            std::getenv("AC6_NR2_FORCE_RECT_BASE_VS") != nullptr;
        if (diagnostic_rectangle_base_vs) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: capture #{}: diagnostic forcing the emulator base "
                "rectangle VS without its generated GS",
                snap.capture_index);
        } else if (builtin_strip && !chosen_is_preexpanded &&
            !GetPrimitiveStripVariant(*chosen, fallback_strip_host_type, pipe)) {
            return Reject(snap, "primitive_strip_variant_pipeline_failed");
        }
        if (quad_convert && !GetTriangleListVariant(*chosen, pipe)) {
            return Reject(snap, "quad_variant_pipeline_failed");
        }
        if (indexed && !builtin_strip && !fan_convert && !quad_convert &&
            pipe->desc.state.primitive_restart()) {
            // Fixed-function restart uses the Vulkan 0xFFFF/0xFFFFFFFF index;
            // accept and hope the guest uses the standard reset index.
            const uint32_t reset_index =
                regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
            if (reset_index != 0xFFFF && reset_index != 0xFFFFFFFF) {
                return Reject(snap, "nonstandard_reset_index");
            }
        }

        // --- Host viewport/scissor via the SDK's own draw_util (operating on
        // the snapshot register file). ---
        draw_util::ViewportInfo viewport_info;
        draw_util::GetHostViewportInfo(regs, 1, 1, false, max_viewport_dimensions_[0],
                                       max_viewport_dimensions_[1], true,
                                       normalized_depth_control, false, true,
                                       snap.ps.writes_depth, viewport_info);
        draw_util::Scissor scissor;
        draw_util::GetScissor(regs, scissor);
        if (!viewport_info.xy_extent[0] || !viewport_info.xy_extent[1]) {
            return Reject(snap, "empty_viewport");
        }
        if (!scissor.extent[0] || !scissor.extent[1]) {
            return Reject(snap, "empty_scissor");
        }
        const bool unbounded_viewport =
            viewport_info.xy_extent[0] > 2048 || viewport_info.xy_extent[1] > 2048;
        if (unbounded_viewport && !snap.tool_relaxed) {
            // AC6 renders at up to 1280x720 at 1x scale; a larger extent means
            // the draw ran without a real guest viewport (dummy/setup draw) -
            // or a clip-disabled screen-space draw (D3D Clear rectangles),
            // which the live capture trigger skips. The trace harness
            // (tool_relaxed) instead keeps the SDK-style giant host viewport
            // (Vulkan permits viewports larger than the framebuffer; the
            // vertex shader does the pixel-space mapping via ndc_scale/offset)
            // and caps the offscreen render target at 1280x720 below.
            return Reject(snap, "implausible_viewport");
        }
        uint32_t rt_width = std::clamp(
            std::max(viewport_info.xy_offset[0] + viewport_info.xy_extent[0],
                     scissor.offset[0] + scissor.extent[0]),
            1u, 4096u);
        uint32_t rt_height = std::clamp(
            std::max(viewport_info.xy_offset[1] + viewport_info.xy_extent[1],
                     scissor.offset[1] + scissor.extent[1]),
            1u, 4096u);
        if (unbounded_viewport) {
            rt_width = std::min(rt_width, 1280u);
            rt_height = std::min(rt_height, 720u);
        }
        // A persistent guest render target is an EDRAM surface, not a
        // viewport-sized scratch image. AC6 changes viewport/scissor extents
        // while continuing to use the same color/depth identities (draws 26
        // and 28 are a compact example: 257x257 followed by 384x384). Size the
        // retained attachment for the canonical frame so those draws share
        // depth and color. One-shot proofs keep their tight dimensions.
        const uint32_t attachment_width =
            persistent_targets_enabled_ ? std::max(rt_width, 1280u) : rt_width;
        const uint32_t attachment_height =
            persistent_targets_enabled_ ? std::max(rt_height, 720u) : rt_height;

        // --- Guest memory as the set-0 SSBO (zero-copy import preferred). ---
        if (!EnsureGuestMemoryBuffer(snap, regs)) {
            return Fatal("guest_memory_buffer_unavailable");
        }

        // --- Index setup. Plain topologies with DMA indices bind the guest
        // index buffer directly from guest memory (mirrors the SDK's
        // ProcessedIndexBufferType::kGuestDMA path: raw guest-endian data is
        // bound and the translated VS swaps gl_VertexIndex per
        // vertex_index_endian). Point/rectangle expansion draws instead use a
        // generated "two-triangle strips" host index pattern (4 host vertices
        // per guest primitive, restart between), with DMA guest indices - if
        // any - fetched inside the VS from the shared-memory SSBO. ---
        auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
        xenos::Endian shader_index_endian = xenos::Endian::kNone;
        uint32_t guest_index_base = 0;
        bool index_32bit = false;
        VkDeviceSize index_offset = 0;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        uint32_t host_draw_vertex_count = index_count;
        uint32_t builtin_primitive_count = 0;
        if (indexed) {
            index_32bit = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32;
            xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
            if (!index_32bit && guest_index_endian != xenos::Endian::kNone &&
                guest_index_endian != xenos::Endian::k8in16) {
                guest_index_endian = guest_index_endian == xenos::Endian::k8in32
                                         ? xenos::Endian::k8in16
                                         : xenos::Endian::kNone;
            }
            const uint32_t index_size_log2 = index_32bit ? 2 : 1;
            guest_index_base = (regs[XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFFu) &
                               ~((1u << index_size_log2) - 1);
            if (guest_index_base >= snap.physical_size ||
                snap.physical_size - guest_index_base < (index_count << index_size_log2)) {
                return Reject(snap, "index_buffer_out_of_bounds");
            }
            shader_index_endian = guest_index_endian;
            index_offset = guest_index_base;
            index_type = index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        }
        xenos::Endian cpu_convert_endian = shader_index_endian;
        uint32_t quad_count = 0;
        std::vector<uint32_t> fan_indices;
        if (builtin_strip) {
            if (!builtin_guest_vertices_per_primitive ||
                index_count % builtin_guest_vertices_per_primitive) {
                return Reject(snap, "malformed_expansion_primitive_count");
            }
            builtin_primitive_count =
                index_count / builtin_guest_vertices_per_primitive;
            if (!builtin_primitive_count) {
                return Reject(snap, "zero_expansion_primitive_count");
            }
            // GetTwoTriangleStripIndexCount: 4 vertices per primitive plus a
            // restart between primitives.
            host_draw_vertex_count =
                4 * builtin_primitive_count + (builtin_primitive_count - 1);
            index_type = VK_INDEX_TYPE_UINT32;
            index_offset = 0;
        } else if (fan_convert) {
            // Match PrimitiveProcessor::TriangleFanToList: Direct3D fan
            // winding is (previous, current, first). Primitive-reset values
            // split one DMA stream into independent fans.
            const uint32_t reset_index =
                regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
            const bool reset_enabled = indexed && pa_su_sc_mode_cntl.multi_prim_ib_ena;
            auto read_fan_index = [&](uint32_t element) -> uint32_t {
                if (!indexed) {
                    return element;
                }
                if (index_32bit) {
                    uint32_t raw;
                    std::memcpy(&raw, snap.physical_membase + guest_index_base +
                                          VkDeviceSize(element) * 4, 4);
                    switch (cpu_convert_endian) {
                        case xenos::Endian::k8in32:
                            raw = __builtin_bswap32(raw);
                            break;
                        case xenos::Endian::k16in32:
                            raw = (raw >> 16) | (raw << 16);
                            break;
                        case xenos::Endian::k8in16:
                            raw = ((raw & 0xFF00FF00u) >> 8) |
                                  ((raw & 0x00FF00FFu) << 8);
                            break;
                        default:
                            break;
                    }
                    return raw & xenos::kVertexIndexMask;
                }
                uint16_t raw;
                std::memcpy(&raw, snap.physical_membase + guest_index_base +
                                      VkDeviceSize(element) * 2, 2);
                return cpu_convert_endian == xenos::Endian::k8in16
                           ? uint16_t((raw >> 8) | (raw << 8))
                           : raw;
            };
            fan_indices.reserve(index_count > 2 ? size_t(index_count - 2) * 3 : 0);
            uint32_t first = 0, previous = 0, fan_size = 0;
            const uint32_t comparable_reset =
                index_32bit ? (reset_index & xenos::kVertexIndexMask)
                            : uint16_t(reset_index);
            for (uint32_t i = 0; i < index_count; ++i) {
                const uint32_t current = read_fan_index(i);
                if (reset_enabled && current == comparable_reset) {
                    fan_size = 0;
                    continue;
                }
                if (!fan_size) {
                    first = current;
                } else if (fan_size == 1) {
                    previous = current;
                } else {
                    fan_indices.push_back(previous);
                    fan_indices.push_back(current);
                    fan_indices.push_back(first);
                    previous = current;
                }
                ++fan_size;
            }
            if (fan_indices.empty()) {
                return Reject(snap, "zero_triangle_fan_count");
            }
            host_draw_vertex_count = uint32_t(fan_indices.size());
            index_type = VK_INDEX_TYPE_UINT32;
            index_offset = 0;
            // The CPU has already applied the guest endian and 24-bit mask.
            shader_index_endian = xenos::Endian::kNone;
        } else if (quad_convert) {
            quad_count = index_count / 4;
            if (!quad_count) {
                return Reject(snap, "zero_quad_count");
            }
            host_draw_vertex_count = quad_count * 6;
            index_type = VK_INDEX_TYPE_UINT32;
            index_offset = 0;
            // Indices become host-converted: the CPU applies the guest endian.
            shader_index_endian = xenos::Endian::kNone;
        }

        // --- Reconstruct the five set-1 constant UBOs. ---
        XeSystemConstants sys{};
        BuildSystemConstants(regs, snap, viewport_info, shader_index_endian, sys);
        if (builtin_strip && indexed) {
            // Guest DMA indices are loaded inside the expansion vertex shader.
            sys.flags |= kXeSysFlag_ComputeOrPrimitiveVertexIndexLoad;
            if (index_32bit) {
                sys.flags |= kXeSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
            }
            sys.vertex_index_load_address = guest_index_base;
        }
        if (const char* constants_path =
                std::getenv("AC6_NR2_LOAD_SYSTEM_CONSTANTS")) {
            FILE* constants_file = std::fopen(constants_path, "rb");
            if (!constants_file ||
                std::fread(&sys, sizeof(sys), 1, constants_file) != 1) {
                if (constants_file) {
                    std::fclose(constants_file);
                }
                return Fatal("system_constants_diagnostic_load_failed");
            }
            std::fclose(constants_file);
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: loaded diagnostic system constants from {}",
                            constants_path);
        }
        if (const char* constants_path =
                std::getenv("AC6_NR2_DUMP_NATIVE_SYSTEM_CONSTANTS")) {
            FILE* constants_file = std::fopen(constants_path, "wb");
            if (constants_file &&
                std::fwrite(&sys, sizeof(sys), 1, constants_file) == 1) {
                REXLOG_CAT_INFO(kLogGPU,
                                "nr2: native system constants dumped to {} ({} bytes)",
                                constants_path, sizeof(sys));
            } else {
                REXLOG_CAT_WARN(kLogGPU,
                                "nr2: cannot dump native system constants to {}",
                                constants_path);
            }
            if (constants_file) {
                std::fclose(constants_file);
            }
        }
        std::vector<uint8_t> float_constants_vs, float_constants_ps;
        PackFloatConstants(snap.vs, regs, uint32_t(XE_GPU_REG_SHADER_CONSTANT_000_X),
                           float_constants_vs);
        PackFloatConstants(snap.ps, regs, uint32_t(XE_GPU_REG_SHADER_CONSTANT_256_X),
                           float_constants_ps);
        constexpr uint32_t kBoolLoopBytes = sizeof(uint32_t) * (8 + 32);
        constexpr uint32_t kFetchBytes = sizeof(uint32_t) * 6 * 32;

        const VkDeviceSize ubo_align = min_uniform_buffer_offset_alignment_;
        auto align_up = [ubo_align](VkDeviceSize v) {
            return (v + ubo_align - 1) & ~(ubo_align - 1);
        };
        VkDeviceSize ubo_offsets[kXeConstantBufferCount];
        VkDeviceSize ubo_sizes[kXeConstantBufferCount] = {
            sizeof(XeSystemConstants), VkDeviceSize(float_constants_vs.size()),
            VkDeviceSize(float_constants_ps.size()), kBoolLoopBytes, kFetchBytes};
        VkDeviceSize ubo_total = 0;
        for (uint32_t i = 0; i < kXeConstantBufferCount; ++i) {
            ubo_offsets[i] = ubo_total;
            ubo_total = align_up(ubo_total + ubo_sizes[i]);
        }
        EphemeralCleanup cleanup(this);
        HostBuffer ubo_buffer{};
        if (defer_draw_waits_) {
            if (!EnsureDeferredBatchPools()) {
                return Fatal("deferred_batch_pool_creation_failed");
            }
            const VkDeviceSize slice_base =
                align_up(deferred_uniform_offset_);
            if (slice_base > deferred_uniform_buffer_.size ||
                ubo_total > deferred_uniform_buffer_.size - slice_base) {
                // This is expected only for an unusually large pass. Submit
                // and drain the completed portion, then reuse the arena from
                // offset zero for the still-unsubmitted draw.
                DrainDeferredDrawResources();
                if (!EnsureDeferredBatchPools() ||
                    ubo_total > deferred_uniform_buffer_.size) {
                    return Fatal("deferred_uniform_arena_exhausted");
                }
            }
            const VkDeviceSize allocation_base =
                align_up(deferred_uniform_offset_);
            ubo_buffer = deferred_uniform_buffer_;
            for (VkDeviceSize& offset : ubo_offsets) {
                offset += allocation_base;
            }
            deferred_uniform_offset_ = allocation_base + ubo_total;
        } else {
            if (!CreateHostBuffer(ubo_total,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  ubo_buffer)) {
                return Fatal("ubo_buffer_creation_failed");
            }
            cleanup.buffers.push_back(ubo_buffer);
        }
        uint8_t* ubo_write = static_cast<uint8_t*>(ubo_buffer.mapped);
        std::memcpy(ubo_write + ubo_offsets[kXeConstantBufferSystem], &sys, sizeof(sys));
        std::memcpy(ubo_write + ubo_offsets[kXeConstantBufferFloatVertex],
                    float_constants_vs.data(), float_constants_vs.size());
        std::memcpy(ubo_write + ubo_offsets[kXeConstantBufferFloatPixel],
                    float_constants_ps.data(), float_constants_ps.size());
        std::memcpy(ubo_write + ubo_offsets[kXeConstantBufferBoolLoop],
                    &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopBytes);
        std::memcpy(ubo_write + ubo_offsets[kXeConstantBufferFetch],
                    &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchBytes);

        // Host-generated index buffer for expansion/conversion draws.
        HostBuffer generated_index_buffer{};
        if (builtin_strip || fan_convert || quad_convert) {
            const VkDeviceSize generated_size =
                VkDeviceSize(host_draw_vertex_count) * sizeof(uint32_t);
            if (!CreateHostBuffer(generated_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                  generated_index_buffer)) {
                return Fatal("generated_index_buffer_creation_failed");
            }
            cleanup.buffers.push_back(generated_index_buffer);
            uint32_t* index_write = static_cast<uint32_t*>(generated_index_buffer.mapped);
            if (builtin_strip) {
                // The SDK's builtin two-triangle-strip pattern:
                // (i << 2) + {0,1,2,3} with a UINT32_MAX restart between.
                for (uint32_t i = 0; i < builtin_primitive_count; ++i) {
                    if (i) {
                        *(index_write++) = UINT32_MAX;  // primitive restart
                    }
                    const uint32_t first_index = i << 2;
                    for (uint32_t j = 0; j < 4; ++j) {
                        *(index_write++) = first_index + j;
                    }
                }
            } else if (fan_convert) {
                std::memcpy(index_write, fan_indices.data(),
                            fan_indices.size() * sizeof(uint32_t));
            } else {
                // Quad list -> triangle list, the SDK's conversion order:
                // (v0, v1, v2), (v0, v2, v3). DMA guest indices are read from
                // guest memory and endian-corrected here (host-converted).
                auto read_guest_index = [&](uint32_t element) -> uint32_t {
                    if (!indexed) {
                        return element;
                    }
                    if (index_32bit) {
                        uint32_t raw;
                        std::memcpy(&raw, snap.physical_membase + guest_index_base +
                                              VkDeviceSize(element) * 4, 4);
                        switch (cpu_convert_endian) {
                            case xenos::Endian::k8in32:
                                return __builtin_bswap32(raw);
                            case xenos::Endian::k16in32:
                                return (raw >> 16) | (raw << 16);
                            case xenos::Endian::k8in16:
                                return ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
                            default:
                                return raw;
                        }
                    }
                    uint16_t raw16;
                    std::memcpy(&raw16, snap.physical_membase + guest_index_base +
                                            VkDeviceSize(element) * 2, 2);
                    return cpu_convert_endian == xenos::Endian::k8in16
                               ? uint16_t((raw16 >> 8) | (raw16 << 8))
                               : raw16;
                };
                for (uint32_t q = 0; q < quad_count; ++q) {
                    const uint32_t v0 = read_guest_index(q * 4 + 0);
                    const uint32_t v1 = read_guest_index(q * 4 + 1);
                    const uint32_t v2 = read_guest_index(q * 4 + 2);
                    const uint32_t v3 = read_guest_index(q * 4 + 3);
                    *(index_write++) = v0;
                    *(index_write++) = v1;
                    *(index_write++) = v2;
                    *(index_write++) = v0;
                    *(index_write++) = v2;
                    *(index_write++) = v3;
                }
            }
        }

        // --- Offscreen render targets from the stored render-pass key. ---
        const RenderPassKeyBits& rpkey = pipe->desc.render_pass_key;
        const uint32_t requested_sample_count = 1u << rpkey.msaa_samples();
        VkSampleCountFlagBits target_samples =
            VkSampleCountFlagBits(requested_sample_count);
        const VkSampleCountFlags supported_target_samples =
            framebuffer_color_sample_counts_ &
            (rpkey.depth_used() ? framebuffer_depth_sample_counts_
                                : framebuffer_color_sample_counts_);
        if (requested_sample_count == 2 &&
            !(supported_target_samples & VK_SAMPLE_COUNT_2_BIT)) {
            // Match CreatePipeline's SDK-compatible 2x-as-4x fallback.
            target_samples = VK_SAMPLE_COUNT_4_BIT;
        }
        if (!(supported_target_samples & target_samples)) {
            return Reject(snap, "render_target_sample_count_unsupported");
        }
        const bool multisampled = target_samples != VK_SAMPLE_COUNT_1_BIT;
        const uint32_t color_rts_used = rpkey.color_rts_used();
        uint32_t color_attachment_count = 0;
        OwnedImage color_images[4] = {};
        OwnedImage resolve_images[4] = {};
        OwnedImage depth_image{};
        VkFormat color_formats[4] = {};
        const bool depth_used = rpkey.depth_used();
        PersistentTargetKey target_key{};
        target_key.render_pass_key = rpkey.key;
        target_key.width = attachment_width;
        target_key.height = attachment_height;
        target_key.surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
        for (uint32_t rt = 0; rt < 4; ++rt) {
            target_key.color_info[rt] =
                regs[reg::RB_COLOR_INFO::rt_register_indices[rt]];
        }
        target_key.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];

        PersistentTarget* persistent_target = nullptr;
        bool target_alias_reused = false;
        float target_view_scale_x = 1.0f;
        float target_view_scale_y = 1.0f;
        if (persistent_targets_enabled_) {
            auto target_it = std::find_if(
                persistent_targets_.begin(), persistent_targets_.end(),
                [&](const PersistentTarget& target) {
                    return target.valid && target.key == target_key;
                });
            if (target_it != persistent_targets_.end()) {
                persistent_target = &*target_it;
            } else {
                // Xenos EDRAM color storage is identified primarily by the
                // color base/format. AC6 frequently rebinds it with different
                // surface-pitch and depth-view metadata while preserving its
                // color samples. Keep the same multisample color image across
                // that view change instead of treating it as an empty target.
                auto surface_pitch_tiles_at_32bpp = [](uint32_t surface_info) {
                    const uint32_t pitch_pixels = surface_info & 0x3FFFu;
                    const uint32_t msaa = (surface_info >> 16) & 3u;
                    const uint32_t samples_x_log2 = uint32_t(msaa >= 2u);
                    return ((pitch_pixels << samples_x_log2) + 79u) / 80u;
                };
                const uint32_t target_msaa =
                    (target_key.surface_info >> 16) & 3u;
                for (PersistentTarget& candidate : persistent_targets_) {
                    if (!candidate.valid || !candidate.initialized ||
                        candidate.key.render_pass_key !=
                            target_key.render_pass_key ||
                        candidate.key.width != target_key.width ||
                        candidate.key.height != target_key.height ||
                        candidate.key.depth_info != target_key.depth_info ||
                        ((candidate.key.surface_info >> 16) & 3u) !=
                            target_msaa ||
                        surface_pitch_tiles_at_32bpp(
                            candidate.key.surface_info) !=
                            surface_pitch_tiles_at_32bpp(
                                target_key.surface_info) ||
                        std::memcmp(candidate.key.color_info,
                                    target_key.color_info,
                                    sizeof(target_key.color_info)) != 0) {
                        continue;
                    }
                    if (!persistent_target ||
                        candidate.last_used_serial >
                            persistent_target->last_used_serial) {
                        persistent_target = &candidate;
                        target_alias_reused = true;
                    }
                }
                if (target_alias_reused) {
                    // Directly binding an attachment created for a different
                    // sample count is invalid in Vulkan. More importantly,
                    // scaling the logical viewport to match Xenos physical
                    // sample coordinates compresses a 1x 1280x720 view into
                    // the upper-left portion of a retained 4x view. Differing
                    // MSAA views are kept as separate native targets and are
                    // combined by ResolveCapturedCopy's EDRAM tile remapper.
                    // Since direct aliases are same-MSAA here, these ratios
                    // remain one; retain the calculation to document the view
                    // relationship and guard future surface-layout variants.
                    const uint32_t source_msaa =
                        (target_key.surface_info >> 16) & 3u;
                    const uint32_t destination_msaa =
                        (persistent_target->key.surface_info >> 16) & 3u;
                    const int32_t source_samples_x_log2 =
                        int32_t(source_msaa >= 2u);
                    const int32_t source_samples_y_log2 =
                        int32_t(source_msaa >= 1u);
                    const int32_t destination_samples_x_log2 =
                        int32_t(destination_msaa >= 2u);
                    const int32_t destination_samples_y_log2 =
                        int32_t(destination_msaa >= 1u);
                    target_view_scale_x = std::ldexp(
                        1.0f, source_samples_x_log2 -
                                  destination_samples_x_log2);
                    target_view_scale_y = std::ldexp(
                        1.0f, source_samples_y_log2 -
                                  destination_samples_y_log2);
                }
            }
        }
        const bool target_reused = persistent_target != nullptr;
        if (target_alias_reused) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: capture #{} reusing EDRAM color alias from surface "
                "0x{:08x}/depth 0x{:08x} for surface 0x{:08x}/depth 0x{:08x} "
                "with physical-sample coordinate scale {}x{}",
                snap.capture_index, persistent_target->key.surface_info,
                persistent_target->key.depth_info, target_key.surface_info,
                target_key.depth_info, target_view_scale_x,
                target_view_scale_y);
        }
        OwnedImage alias_seed_images[4] = {};
        uint64_t alias_seed_serials[4] = {};
        if (REXCVAR_GET(ac6_native_edram_alias_experimental) &&
            persistent_targets_enabled_ && !target_reused && !multisampled) {
            for (const PersistentTarget& candidate : persistent_targets_) {
                if (!candidate.valid || !candidate.initialized) {
                    continue;
                }
                const RenderPassKeyBits candidate_rpkey{
                    candidate.key.render_pass_key};
                for (uint32_t rt = 0; rt < 4; ++rt) {
                    if (!(color_rts_used & (1u << rt)) ||
                        !(candidate_rpkey.color_rts_used() & (1u << rt)) ||
                        candidate.key.color_info[rt] != target_key.color_info[rt] ||
                        candidate_rpkey.color_view_format(rt) !=
                            rpkey.color_view_format(rt) ||
                        candidate.last_used_serial <= alias_seed_serials[rt]) {
                        continue;
                    }
                    const OwnedImage& candidate_image = candidate.multisampled
                        ? candidate.resolve[rt]
                        : candidate.color[rt];
                    if (!candidate_image.image ||
                        candidate_image.width != attachment_width ||
                        candidate_image.height != attachment_height) {
                        continue;
                    }
                    alias_seed_images[rt] = candidate_image;
                    alias_seed_serials[rt] = candidate.last_used_serial;
                }
            }
        }
        const size_t cleanup_target_image_start = cleanup.images.size();
        if (target_reused) {
            for (uint32_t rt = 0; rt < 4; ++rt) {
                color_images[rt] = persistent_target->color[rt];
                resolve_images[rt] = persistent_target->resolve[rt];
            }
            depth_image = persistent_target->depth;
        } else {
            for (uint32_t rt = 0; rt < 4; ++rt) {
                if (!(color_rts_used & (1u << rt))) {
                    continue;
                }
                color_formats[rt] = ColorFormat(rpkey.color_view_format(rt));
                if (color_formats[rt] == VK_FORMAT_UNDEFINED ||
                    !CreateOwnedImage(color_formats[rt], attachment_width,
                                      attachment_height,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          (multisampled
                                               ? 0
                                               : (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                  (rpkey.color_view_format(rt) == 0u
                                                       ? VK_IMAGE_USAGE_SAMPLED_BIT
                                                       : 0))),
                                      VK_IMAGE_ASPECT_COLOR_BIT, color_images[rt],
                                      target_samples)) {
                    return Fatal("color_target_creation_failed");
                }
                cleanup.images.push_back(color_images[rt]);
                if (multisampled) {
                    if (!CreateOwnedImage(color_formats[rt], attachment_width,
                                          attachment_height,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              (rpkey.color_view_format(rt) == 0u
                                                   ? VK_IMAGE_USAGE_SAMPLED_BIT
                                                   : 0),
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          resolve_images[rt])) {
                        return Fatal("color_resolve_target_creation_failed");
                    }
                    cleanup.images.push_back(resolve_images[rt]);
                }
            }
            if (depth_used) {
                const VkFormat depth_format = DepthFormat(rpkey.depth_format());
                if (!CreateOwnedImage(depth_format, attachment_width,
                                      attachment_height,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT |
                                          VK_IMAGE_ASPECT_STENCIL_BIT,
                                      depth_image, target_samples)) {
                    return Fatal("depth_target_creation_failed");
                }
                cleanup.images.push_back(depth_image);
            }
            if (persistent_targets_enabled_) {
                persistent_targets_.emplace_back();
                persistent_target = &persistent_targets_.back();
                persistent_target->valid = true;
                persistent_target->initialized = false;
                persistent_target->multisampled = multisampled;
                persistent_target->key = target_key;
                for (uint32_t rt = 0; rt < 4; ++rt) {
                    persistent_target->color[rt] = color_images[rt];
                    persistent_target->resolve[rt] = resolve_images[rt];
                }
                persistent_target->depth = depth_image;
                // Ownership moves from this draw's cleanup to the prover.
                cleanup.images.resize(cleanup_target_image_start);
                REXLOG_CAT_INFO(
                    kLogGPU,
                    "nr2: persistent target cache added identity #{} "
                    "(rpkey=0x{:08x}, surface=0x{:08x}, color0=0x{:08x}, "
                    "depth=0x{:08x}, {}x{})",
                    persistent_targets_.size(), target_key.render_pass_key,
                    target_key.surface_info, target_key.color_info[0],
                    target_key.depth_info, target_key.width, target_key.height);
                for (uint32_t rt = 0; rt < 4; ++rt) {
                    if (alias_seed_images[rt].image) {
                        REXLOG_CAT_INFO(
                            kLogGPU,
                            "nr2: identity #{} RT{} will seed from compatible "
                            "EDRAM alias last used at serial {}",
                            persistent_targets_.size(), rt,
                            alias_seed_serials[rt]);
                    }
                }
            }
        }
        for (uint32_t rt = 0; rt < 4; ++rt) {
            if (color_rts_used & (1u << rt)) {
                color_attachment_count = rt + 1;
                color_formats[rt] = ColorFormat(rpkey.color_view_format(rt));
            }
        }
        if (!color_attachment_count) {
            return Reject(snap, "no_color_targets");
        }

        // --- Descriptors: set 0 sharedmem SSBO, set 1 the five UBOs, sets 2/3
        // placeholder textures/samplers (expected wrong colors, correct
        // geometry coverage). ---
        std::map<uint32_t, VkDescriptorSetLayoutBinding> merged[4];
        std::map<uint32_t, ReflectedBinding> reflected[4];
        MergeShaderBindings(pipe->vs, pipe->ps, merged, reflected);

        uint32_t sampled_image_descriptor_count = 0;
        uint32_t combined_image_descriptor_count = 0;
        uint32_t sampler_descriptor_count = 0;
        bool needs_placeholder_2d_array = false, needs_placeholder_3d = false,
             needs_placeholder_cube = false;
        for (uint32_t set = 2; set < 4; ++set) {
            for (const auto& [binding, rb] : reflected[set]) {
                if (rb.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    sampler_descriptor_count += rb.count;
                    continue;
                }
                if (rb.image_ms) {
                    return Reject(snap, "multisampled_texture_binding");
                }
                if (rb.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    combined_image_descriptor_count += rb.count;
                } else {
                    sampled_image_descriptor_count += rb.count;
                }
                if (rb.image_dim == 1 && rb.image_arrayed) {
                    needs_placeholder_2d_array = true;
                } else if (rb.image_dim == 2) {
                    needs_placeholder_3d = true;
                } else if (rb.image_dim == 3) {
                    needs_placeholder_cube = true;
                }
            }
        }
        const uint32_t any_image_descriptor_count =
            sampled_image_descriptor_count + combined_image_descriptor_count;

        OwnedImage placeholder_2d{}, placeholder_2d_array{}, placeholder_3d{},
            placeholder_cube{};
        VkSampler placeholder_sampler = VK_NULL_HANDLE;
        std::map<uint64_t, VkSampler> guest_sampler_by_binding;
        HostBuffer synthetic_staging{};
        std::vector<GuestTextureUpload> guest_textures;
        std::map<uint64_t, size_t> guest_texture_by_binding;
        std::map<uint32_t, size_t> guest_texture_by_fetch_view;
        uint32_t guest_texture_fallbacks = 0;
        std::string first_texture_fallback_reason;
        uint32_t first_texture_fallback_set = UINT32_MAX;
        uint32_t first_texture_fallback_binding = UINT32_MAX;
        uint32_t first_texture_fallback_fetch = UINT32_MAX;
        uint32_t first_texture_fallback_format = UINT32_MAX;
        if (any_image_descriptor_count) {
            // Binding numbers for sampled images are exactly the indices in
            // SpirvShader::GetTextureBindingsAfterTranslation(). Try the
            // slice-3b real guest uploader for each; unsupported formats and
            // dimensions deliberately keep the checkerboard fallback.
            for (uint32_t set = 2; set < 4; ++set) {
                const DrawSnapshotShader &shader = set == 2 ? snap.vs : snap.ps;
                for (const auto &[binding_number, rb] : reflected[set]) {
                    if (rb.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                        continue;
                    }
                    if (rb.count != 1 ||
                        binding_number >= shader.texture_bindings.size()) {
                        guest_texture_fallbacks += rb.count;
                        if (first_texture_fallback_reason.empty()) {
                            first_texture_fallback_reason =
                                "descriptor_metadata_missing";
                            first_texture_fallback_set = set;
                            first_texture_fallback_binding = binding_number;
                        }
                        const std::string diagnostic_key =
                            "descriptor_metadata_missing:" +
                            std::to_string(set) + ":" +
                            std::to_string(uint32_t(rb.type)) + ":" +
                            std::to_string(rb.count);
                        if (reported_texture_fallbacks_
                                .insert(diagnostic_key)
                                .second) {
                            REXLOG_CAT_ERROR(
                                kLogGPU,
                                "nr2-texture-gap: first seen at capture #{}: "
                                "set {} binding {} has {} descriptor(s) of "
                                "type {}, but translated texture metadata is "
                                "missing",
                                snap.capture_index, set, binding_number,
                                rb.count, uint32_t(rb.type));
                        }
                        continue;
                    }
                    const DrawSnapshotTextureBinding& snapshot_binding =
                        shader.texture_bindings[binding_number];
                    const xenos::xe_gpu_texture_fetch_t snapshot_fetch =
                        regs.GetTextureFetch(snapshot_binding.fetch_constant);
                    // Only 8_8_8_8 has distinct UNORM/SNORM host images in
                    // the formats handled by CreateGuestTexture2D. BC and
                    // float textures otherwise arrive as paired translated
                    // signed/unsigned descriptors with the exact same
                    // VkFormat and texel contents, so share one upload.
                    const bool signed_view_is_distinct =
                        uint32_t(snapshot_fetch.format) ==
                        uint32_t(xenos::TextureFormat::k_8_8_8_8);
                    const uint32_t fetch_view_key =
                        uint32_t(snapshot_binding.fetch_constant) |
                        (uint32_t(snapshot_binding.dimension) << 8) |
                        (uint32_t(snapshot_binding.is_signed &&
                                  signed_view_is_distinct)
                         << 16);
                    const auto existing_texture =
                        guest_texture_by_fetch_view.find(fetch_view_key);
                    if (existing_texture != guest_texture_by_fetch_view.end()) {
                        guest_texture_by_binding.emplace(
                            (uint64_t(set) << 32) | binding_number,
                            existing_texture->second);
                        if (HotTextureExecutionLogsEnabled(snap)) {
                            REXLOG_CAT_INFO(
                                kLogGPU,
                                "nr2: capture #{}: texture set {} binding {} "
                                "fetch {} reuses guest upload {}",
                                snap.capture_index, set, binding_number,
                                snapshot_binding.fetch_constant,
                                existing_texture->second);
                        }
                        continue;
                    }
                    GuestTextureUpload upload{};
                    const char* reason = nullptr;
                    if (!CreateGuestTexture2D(snap, regs, snapshot_binding,
                                              cleanup, upload, reason)) {
                        ++guest_texture_fallbacks;
                        const xenos::xe_gpu_texture_fetch_t diagnostic_fetch =
                            regs.GetTextureFetch(
                                snapshot_binding.fetch_constant);
                        TextureInfo diagnostic_info{};
                        const bool diagnostic_prepare_safe =
                            !diagnostic_fetch.mip_min_level ||
                            !diagnostic_fetch.base_address ||
                            !diagnostic_fetch.mip_address ||
                            diagnostic_fetch.base_address ==
                                diagnostic_fetch.mip_address;
                        const bool diagnostic_valid =
                            diagnostic_prepare_safe &&
                            TextureInfo::Prepare(diagnostic_fetch,
                                                 &diagnostic_info);
                        const uint32_t diagnostic_dimension =
                            diagnostic_valid
                                ? uint32_t(diagnostic_info.dimension)
                                : UINT32_MAX;
                        const uint32_t diagnostic_format =
                            diagnostic_valid
                                ? uint32_t(diagnostic_info.format)
                                : UINT32_MAX;
                        if (first_texture_fallback_reason.empty()) {
                            first_texture_fallback_reason =
                                reason ? reason : "unknown";
                            first_texture_fallback_set = set;
                            first_texture_fallback_binding = binding_number;
                            first_texture_fallback_fetch =
                                snapshot_binding.fetch_constant;
                            first_texture_fallback_format = diagnostic_format;
                        }
                        REXLOG_CAT_INFO(
                            kLogGPU,
                            "nr2: capture #{}: texture set {} binding {} "
                            "fetch {} keeps synthetic fallback ({}, "
                            "shader-dim={}, guest-dim={}, stacked={}, "
                            "format={})",
                            snap.capture_index, set, binding_number,
                            snapshot_binding.fetch_constant,
                            reason ? reason : "unknown",
                            snapshot_binding.dimension,
                            diagnostic_dimension,
                            diagnostic_valid && diagnostic_info.is_stacked,
                            diagnostic_format);
                        const std::string diagnostic_key =
                            std::string(reason ? reason : "unknown") + ":" +
                            std::to_string(snapshot_binding.dimension) + ":" +
                            std::to_string(diagnostic_dimension) + ":" +
                            std::to_string(diagnostic_valid &&
                                           diagnostic_info.is_stacked) +
                            ":" + std::to_string(diagnostic_format);
                        if (reported_texture_fallbacks_
                                .insert(diagnostic_key)
                                .second) {
                            REXLOG_CAT_ERROR(
                                kLogGPU,
                                "nr2-texture-gap: first seen at capture #{}: "
                                "set {} binding {} fetch {} reason='{}', "
                                "shader-dim={}, guest-dim={}, stacked={}, "
                                "format={}, vs={:016x}, ps={:016x}",
                                snap.capture_index, set, binding_number,
                                snapshot_binding.fetch_constant,
                                reason ? reason : "unknown",
                                snapshot_binding.dimension,
                                diagnostic_dimension,
                                diagnostic_valid &&
                                    diagnostic_info.is_stacked,
                                diagnostic_format, snap.vs.ucode_hash,
                                snap.ps.ucode_hash);
                        }
                        continue;
                    }
                    const size_t upload_index = guest_textures.size();
                    guest_textures.push_back(upload);
                    guest_texture_by_binding.emplace(
                        (uint64_t(set) << 32) | binding_number, upload_index);
                    guest_texture_by_fetch_view.emplace(fetch_view_key,
                                                        upload_index);
                    if (HotTextureExecutionLogsEnabled(snap)) {
                        REXLOG_CAT_INFO(
                            kLogGPU,
                            "nr2: capture #{}: real guest texture set {} "
                            "binding {} fetch {} uploaded ({}x{}x{} layers, "
                            "VkFormat {}, {} bytes, {})",
                            snap.capture_index, set, binding_number,
                            upload.fetch_constant, upload.image.width,
                            upload.image.height, upload.image.array_layers,
                            uint32_t(upload.format),
                            uint64_t(upload.staging_size),
                            shader.texture_bindings[binding_number].is_signed
                                ? "signed"
                                : "unsigned");
                    }
                }
            }

            // Create and upload the diagnostic checker only if at least one
            // reflected image binding actually needs it. Previously every
            // textured draw allocated a 256x256 image and staging buffer even
            // when all bindings had real guest textures. Besides wasting CPU
            // and memory bandwidth, that made the native path pay for a
            // fallback that was never referenced by a descriptor.
            if (guest_texture_fallbacks) {
                auto create_placeholder = [&](VkImageViewType view_type,
                                              OwnedImage &image) {
                    if (!CreatePlaceholderImage(view_type, image)) {
                        return false;
                    }
                    cleanup.images.push_back(image);
                    return true;
                };
                if (!create_placeholder(VK_IMAGE_VIEW_TYPE_2D,
                                        placeholder_2d) ||
                    (needs_placeholder_2d_array &&
                     !create_placeholder(VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                         placeholder_2d_array)) ||
                    (needs_placeholder_3d &&
                     !create_placeholder(VK_IMAGE_VIEW_TYPE_3D,
                                         placeholder_3d)) ||
                    (needs_placeholder_cube &&
                     !create_placeholder(VK_IMAGE_VIEW_TYPE_CUBE,
                                         placeholder_cube))) {
                    return Fatal("placeholder_image_creation_failed");
                }
                // One staging source is shared by all placeholder shapes.
                if (!CreateHostBuffer(kSyntheticTextureBytes,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      synthetic_staging)) {
                    return Fatal("synthetic_texture_staging_failed");
                }
                cleanup.buffers.push_back(synthetic_staging);
                FillSyntheticCheckerboard(
                    static_cast<uint8_t *>(synthetic_staging.mapped));
            }
        }
        if (sampler_descriptor_count || any_image_descriptor_count) {
            VkSamplerCreateInfo sampler_info = {};
            sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler_info.magFilter = VK_FILTER_LINEAR;
            sampler_info.minFilter = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.maxLod = VK_LOD_CLAMP_NONE;
            constexpr uint32_t kPlaceholderSamplerKey = UINT32_MAX;
            const auto placeholder_it =
                guest_sampler_cache_.find(kPlaceholderSamplerKey);
            if (placeholder_it != guest_sampler_cache_.end()) {
                placeholder_sampler = placeholder_it->second;
            } else {
                if (fns_.CreateSampler(device_, &sampler_info, nullptr,
                                       &placeholder_sampler) != VK_SUCCESS) {
                    return Fatal("placeholder_sampler_creation_failed");
                }
                guest_sampler_cache_.emplace(kPlaceholderSamplerKey,
                                             placeholder_sampler);
            }

            // The translated SPIR-V keeps samplers separate from images. The
            // sampler-binding vector is in descriptor order; resolve its
            // kUseFetchConst filters and the fetch clamp modes just like the
            // SDK texture cache. A single linear/repeat sampler shifts point
            // sampled post-process copies and wraps their edges.
            static constexpr VkSamplerAddressMode kAddressModeMap[8] = {
                VK_SAMPLER_ADDRESS_MODE_REPEAT,
                VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            };
            for (uint32_t set = 2; set < 4; ++set) {
                const DrawSnapshotShader &shader = set == 2 ? snap.vs : snap.ps;
                size_t sampler_index = 0;
                for (const auto &[binding_number, rb] : reflected[set]) {
                    if (rb.type != VK_DESCRIPTOR_TYPE_SAMPLER &&
                        rb.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                        continue;
                    }
                    for (uint32_t element = 0; element < rb.count;
                         ++element, ++sampler_index) {
                        if (sampler_index >= shader.sampler_bindings.size()) {
                            continue;
                        }
                        const DrawSnapshotSamplerBinding &binding =
                            shader.sampler_bindings[sampler_index];
                        const xenos::xe_gpu_texture_fetch_t fetch =
                            regs.GetTextureFetch(binding.fetch_constant);
                        auto resolve_filter = [](uint32_t shader_filter,
                                                 uint32_t fetch_filter) {
                            // TextureFilter::kUseFetchConst is 3.
                            return shader_filter == 3u ? fetch_filter
                                                       : shader_filter;
                        };
                        const uint32_t mag_filter = resolve_filter(
                            binding.mag_filter, uint32_t(fetch.mag_filter));
                        const uint32_t min_filter = resolve_filter(
                            binding.min_filter, uint32_t(fetch.min_filter));
                        const uint32_t mip_filter = resolve_filter(
                            binding.mip_filter, uint32_t(fetch.mip_filter));
                        VkSamplerCreateInfo guest_sampler_info = {};
                        guest_sampler_info.sType =
                            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                        guest_sampler_info.magFilter = mag_filter == 1u
                                                           ? VK_FILTER_LINEAR
                                                           : VK_FILTER_NEAREST;
                        guest_sampler_info.minFilter = min_filter == 1u
                                                           ? VK_FILTER_LINEAR
                                                           : VK_FILTER_NEAREST;
                        guest_sampler_info.mipmapMode =
                            mip_filter == 1u ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                        guest_sampler_info.addressModeU =
                            kAddressModeMap[uint32_t(fetch.clamp_x) & 7u];
                        guest_sampler_info.addressModeV =
                            kAddressModeMap[uint32_t(fetch.clamp_y) & 7u];
                        guest_sampler_info.addressModeW =
                            kAddressModeMap[uint32_t(fetch.clamp_z) & 7u];
                        guest_sampler_info.borderColor =
                            uint32_t(fetch.border_color) == 1u
                                ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                        // This slice uploads base level only.
                        guest_sampler_info.minLod = 0.0f;
                        guest_sampler_info.maxLod = 0.25f;
                        const uint32_t sampler_state_key =
                            uint32_t(guest_sampler_info.magFilter ==
                                     VK_FILTER_LINEAR) |
                            (uint32_t(guest_sampler_info.minFilter ==
                                      VK_FILTER_LINEAR)
                             << 1) |
                            (uint32_t(guest_sampler_info.mipmapMode ==
                                      VK_SAMPLER_MIPMAP_MODE_LINEAR)
                             << 2) |
                            (uint32_t(guest_sampler_info.addressModeU) << 3) |
                            (uint32_t(guest_sampler_info.addressModeV) << 6) |
                            (uint32_t(guest_sampler_info.addressModeW) << 9) |
                            (uint32_t(guest_sampler_info.borderColor ==
                                      VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
                             << 12);
                        const auto existing_sampler =
                            guest_sampler_cache_.find(sampler_state_key);
                        if (existing_sampler != guest_sampler_cache_.end()) {
                            guest_sampler_by_binding.emplace(
                                (uint64_t(set) << 32) |
                                    (binding_number + element),
                                existing_sampler->second);
                            continue;
                        }
                        VkSampler guest_sampler = VK_NULL_HANDLE;
                        if (fns_.CreateSampler(device_, &guest_sampler_info,
                                               nullptr,
                                               &guest_sampler) != VK_SUCCESS) {
                            return Fatal("guest_sampler_creation_failed");
                        }
                        guest_sampler_cache_.emplace(sampler_state_key,
                                                     guest_sampler);
                        guest_sampler_by_binding.emplace(
                            (uint64_t(set) << 32) | (binding_number + element),
                            guest_sampler);
                    }
                }
            }
        }

        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        if (defer_draw_waits_) {
            if (!EnsureDeferredBatchPools()) {
                return Fatal("deferred_batch_pool_creation_failed");
            }
            descriptor_pool = deferred_descriptor_pool_;
        } else {
            VkDescriptorPoolSize pool_sizes[5] = {};
            uint32_t pool_size_count = 0;
            pool_sizes[pool_size_count++] = {
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
            pool_sizes[pool_size_count++] = {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8};
            if (sampled_image_descriptor_count) {
                pool_sizes[pool_size_count++] = {
                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    sampled_image_descriptor_count};
            }
            if (combined_image_descriptor_count) {
                pool_sizes[pool_size_count++] = {
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    combined_image_descriptor_count};
            }
            if (sampler_descriptor_count) {
                pool_sizes[pool_size_count++] = {
                    VK_DESCRIPTOR_TYPE_SAMPLER, sampler_descriptor_count};
            }
            VkDescriptorPoolCreateInfo pool_info = {};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 4;
            pool_info.poolSizeCount = pool_size_count;
            pool_info.pPoolSizes = pool_sizes;
            if (fns_.CreateDescriptorPool(device_, &pool_info, nullptr,
                                          &descriptor_pool) != VK_SUCCESS) {
                return Fatal("descriptor_pool_creation_failed");
            }
            cleanup.descriptor_pool = descriptor_pool;
        }

        VkDescriptorSet sets[4] = {};
        VkDescriptorSetAllocateInfo set_alloc = {};
        set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_alloc.descriptorPool = descriptor_pool;
        set_alloc.descriptorSetCount = 4;
        set_alloc.pSetLayouts = pipe->set_layouts;
        if (fns_.AllocateDescriptorSets(device_, &set_alloc, sets) !=
            VK_SUCCESS) {
            if (!defer_draw_waits_) {
                return Fatal("descriptor_set_allocation_failed");
            }
            // The draw's constant slice has already been reserved and
            // populated. Draining here resets the arena offset, so retrying
            // with the old offsets would let a later draw overwrite these
            // constants before execution. Fail closed; the emulator remains
            // authoritative and a later implementation may reserve/rewrite
            // the slice after starting a new batch.
            return Fatal("deferred_descriptor_set_allocation_failed");
        }

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> buffer_infos;
        std::vector<VkDescriptorImageInfo> image_infos;
        buffer_infos.reserve(8);
        image_infos.reserve(size_t(any_image_descriptor_count) + sampler_descriptor_count + 1);
        auto placeholder_view_for = [&](const ReflectedBinding& rb) {
            if (rb.image_dim == 1 && rb.image_arrayed) return placeholder_2d_array.view;
            if (rb.image_dim == 2) return placeholder_3d.view;
            if (rb.image_dim == 3) return placeholder_cube.view;
            return placeholder_2d.view;
        };
        // Set 0: shared memory SSBO.
        if (merged[0].count(0)) {
            buffer_infos.push_back({guest_memory_buffer_, 0, VK_WHOLE_SIZE});
            VkWriteDescriptorSet w = {};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets[0];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &buffer_infos.back();
            writes.push_back(w);
        }
        // Set 1: the five constant UBOs (only the bindings the shaders use).
        for (const auto& [binding, b] : merged[1]) {
            if (binding >= kXeConstantBufferCount ||
                b.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                continue;
            }
            buffer_infos.push_back(
                {ubo_buffer.buffer, ubo_offsets[binding], ubo_sizes[binding]});
            VkWriteDescriptorSet w = {};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets[1];
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &buffer_infos.back();
            writes.push_back(w);
        }
        // Sets 2/3: real slice-3b guest images where supported, otherwise the
        // high-contrast synthetic fallback. Samplers remain the conservative
        // linear/repeat placeholder in this slice.
        for (uint32_t set = 2; set < 4; ++set) {
            for (const auto& [binding, rb] : reflected[set]) {
                const size_t first_info = image_infos.size();
                for (uint32_t i = 0; i < rb.count; ++i) {
                    VkDescriptorImageInfo info = {};
                    if (rb.type != VK_DESCRIPTOR_TYPE_SAMPLER) {
                        const uint64_t binding_key =
                            (uint64_t(set) << 32) | (binding + i);
                        const auto guest_it =
                            guest_texture_by_binding.find(binding_key);
                        info.imageView =
                            guest_it != guest_texture_by_binding.end()
                                ? guest_textures[guest_it->second].image.view
                                : placeholder_view_for(rb);
                        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                    if (rb.type == VK_DESCRIPTOR_TYPE_SAMPLER ||
                        rb.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                        const uint64_t binding_key =
                            (uint64_t(set) << 32) | (binding + i);
                        const auto sampler_it =
                            guest_sampler_by_binding.find(binding_key);
                        info.sampler =
                            sampler_it != guest_sampler_by_binding.end()
                                ? sampler_it->second
                                : placeholder_sampler;
                    }
                    image_infos.push_back(info);
                }
                VkWriteDescriptorSet w = {};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = sets[set];
                w.dstBinding = binding;
                w.descriptorCount = rb.count;
                w.descriptorType = rb.type;
                w.pImageInfo = &image_infos[first_info];
                writes.push_back(w);
            }
        }
        fns_.UpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
        if (DetailedExecutionLogsEnabled(snap)) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: capture #{}: texture descriptors - {} unique real guest "
                "uploads, {} synthetic fallback; {} sampled image + {} combined + "
                "{} sampler total "
                "({}x{} fallback checker, cell {})",
                snap.capture_index, guest_textures.size(),
                guest_texture_fallbacks, sampled_image_descriptor_count,
                combined_image_descriptor_count, sampler_descriptor_count,
                kSyntheticTextureSize, kSyntheticTextureSize,
                kSyntheticTextureCell);
        }

        // --- Readback buffer for RT0. ---
        const uint32_t rt0_index = [&] {
            for (uint32_t rt = 0; rt < 4; ++rt) {
                if (color_rts_used & (1u << rt)) return rt;
            }
            return 0u;
        }();
        const uint32_t rt0_bpp = FormatBytesPerPixel(color_formats[rt0_index]);
        const bool readback_requested = !snap.tool_suppress_output;
        HostBuffer readback{};
        if (readback_requested && (!rt0_bpp ||
            !CreateHostBuffer(VkDeviceSize(attachment_width) * attachment_height *
                                  rt0_bpp,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback))) {
            return Fatal("readback_buffer_creation_failed");
        }
        if (readback_requested) {
            cleanup.buffers.push_back(readback);
        }

        // --- Record + submit. ---
        VkCommandPool cmd_pool = VK_NULL_HANDLE;
        if (defer_draw_waits_) {
            // The descriptor allocation path above has already ensured the
            // batch pools exist (including for descriptor-free shaders).
            cmd_pool = deferred_command_pool_;
        } else {
            VkCommandPoolCreateInfo cmd_pool_info = {};
            cmd_pool_info.sType =
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            cmd_pool_info.queueFamilyIndex = queue_family_;
            if (fns_.CreateCommandPool(device_, &cmd_pool_info, nullptr,
                                       &cmd_pool) != VK_SUCCESS) {
                return Fatal("command_pool_creation_failed");
            }
            cleanup.command_pool = cmd_pool;
        }
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (defer_draw_waits_) {
            if (!EnsureDeferredBatchCommandBuffer()) {
                return Fatal("batch_command_buffer_creation_failed");
            }
            cmd = deferred_batch_command_buffer_;
        } else {
            VkCommandBufferAllocateInfo cmd_alloc = {};
            cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmd_alloc.commandPool = cmd_pool;
            cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_alloc.commandBufferCount = 1;
            if (fns_.AllocateCommandBuffers(device_, &cmd_alloc, &cmd) !=
                VK_SUCCESS) {
                return Fatal("command_buffer_allocation_failed");
            }
            VkCommandBufferBeginInfo begin_info = {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (fns_.BeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
                return Fatal("command_buffer_begin_failed");
            }
        }

        // Placeholder images: undefined -> transfer dst, upload the synthetic
        // magenta/green checkerboard from the staging buffer, -> sampled.
        for (OwnedImage* placeholder :
             {&placeholder_2d, &placeholder_2d_array, &placeholder_3d, &placeholder_cube}) {
            if (!placeholder->image) {
                continue;
            }
            ImageBarrier(cmd, placeholder->image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            // One region per array layer, all reading the same staging image
            // (3D placeholders are depth 1, so a single region covers them).
            std::vector<VkBufferImageCopy> regions(placeholder->array_layers);
            for (uint32_t layer = 0; layer < placeholder->array_layers; ++layer) {
                VkBufferImageCopy& region = regions[layer];
                region = {};
                region.bufferOffset = 0;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                region.imageExtent = {placeholder->width, placeholder->height,
                                      placeholder->depth};
            }
            fns_.CmdCopyBufferToImage(cmd, synthetic_staging.buffer, placeholder->image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      uint32_t(regions.size()), regions.data());
            ImageBarrier(cmd, placeholder->image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        // Real guest block-compressed images: the staging buffers are already
        // endian-corrected and untiled into tightly packed BC blocks.
        for (const GuestTextureUpload& upload : guest_textures) {
            if (!upload.needs_upload) {
                continue;
            }
            ImageBarrier(cmd, upload.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region = {};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                       upload.image.array_layers};
            region.imageExtent = {upload.image.width, upload.image.height, 1};
            fns_.CmdCopyBufferToImage(cmd, upload.staging.buffer,
                                      upload.image.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                      &region);
            ImageBarrier(cmd, upload.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        // Render target layout transitions.
        const bool load_persistent_target =
            target_reused && persistent_target->initialized;
        for (uint32_t rt = 0; rt < 4; ++rt) {
            if (!(color_rts_used & (1u << rt))) {
                continue;
            }
            const bool seed_from_alias =
                !load_persistent_target && alias_seed_images[rt].image;
            if (seed_from_alias) {
                ImageBarrier(cmd, color_images[rt].image,
                             VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageCopy alias_copy = {};
                alias_copy.srcSubresource =
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                alias_copy.dstSubresource =
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                alias_copy.extent = {attachment_width, attachment_height, 1};
                fns_.CmdCopyImage(
                    cmd, alias_seed_images[rt].image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    color_images[rt].image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &alias_copy);
                ImageBarrier(cmd, color_images[rt].image,
                             VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            } else {
                ImageBarrier(
                    cmd, color_images[rt].image, VK_IMAGE_ASPECT_COLOR_BIT,
                    load_persistent_target
                        ? (multisampled
                               ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    load_persistent_target
                        ? (multisampled ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                        : VK_ACCESS_TRANSFER_READ_BIT)
                        : 0,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    load_persistent_target
                        ? (multisampled
                               ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                               : VK_PIPELINE_STAGE_TRANSFER_BIT)
                        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            if (multisampled) {
                ImageBarrier(cmd, resolve_images[rt].image,
                             VK_IMAGE_ASPECT_COLOR_BIT,
                             load_persistent_target
                                 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             load_persistent_target ? VK_ACCESS_TRANSFER_READ_BIT : 0,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             load_persistent_target ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                                    : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
        }
        if (depth_used) {
            ImageBarrier(cmd, depth_image.image,
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                         load_persistent_target
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         load_persistent_target
                             ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        }

        VkRenderingAttachmentInfo color_attachments[4] = {};
        for (uint32_t rt = 0; rt < color_attachment_count; ++rt) {
            VkRenderingAttachmentInfo& attachment = color_attachments[rt];
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (!(color_rts_used & (1u << rt))) {
                continue;  // imageView stays VK_NULL_HANDLE for the unused slot
            }
            attachment.imageView = color_images[rt].view;
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp =
                (load_persistent_target || alias_seed_images[rt].image)
                                    ? VK_ATTACHMENT_LOAD_OP_LOAD
                                    : VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            if (multisampled) {
                attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                attachment.resolveImageView = resolve_images[rt].view;
                attachment.resolveImageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        VkRenderingAttachmentInfo depth_attachment = {};
        VkRenderingAttachmentInfo stencil_attachment = {};
        if (depth_used) {
            // Clear to the "farthest" value for the pipeline's depth compare so
            // an enabled depth test still passes.
            const uint32_t depth_compare = pipe->desc.state.depth_compare_op();
            const float depth_clear = (depth_compare & 0b100) ? 0.0f : 1.0f;
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = depth_image.view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = load_persistent_target
                                          ? VK_ATTACHMENT_LOAD_OP_LOAD
                                          : VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {depth_clear, 0};
            stencil_attachment = depth_attachment;
        }
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0},
                                     {attachment_width, attachment_height}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = color_attachment_count;
        rendering_info.pColorAttachments = color_attachments;
        if (depth_used) {
            rendering_info.pDepthAttachment = &depth_attachment;
            rendering_info.pStencilAttachment = &stencil_attachment;
        }
        fns_.CmdBeginRendering(cmd, &rendering_info);

        VkViewport viewport;
        viewport.x = float(viewport_info.xy_offset[0]) * target_view_scale_x;
        viewport.y = float(viewport_info.xy_offset[1]) * target_view_scale_y;
        viewport.width = float(viewport_info.xy_extent[0]) * target_view_scale_x;
        viewport.height = float(viewport_info.xy_extent[1]) * target_view_scale_y;
        viewport.minDepth = viewport_info.z_min;
        viewport.maxDepth = viewport_info.z_max;
        fns_.CmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor_rect;
        const int32_t scaled_scissor_x0 = std::max(
            int32_t(std::floor(float(scissor.offset[0]) * target_view_scale_x)),
            0);
        const int32_t scaled_scissor_y0 = std::max(
            int32_t(std::floor(float(scissor.offset[1]) * target_view_scale_y)),
            0);
        const int32_t scaled_scissor_x1 = std::min(
            int32_t(std::ceil(float(scissor.offset[0] + scissor.extent[0]) *
                              target_view_scale_x)),
            int32_t(attachment_width));
        const int32_t scaled_scissor_y1 = std::min(
            int32_t(std::ceil(float(scissor.offset[1] + scissor.extent[1]) *
                              target_view_scale_y)),
            int32_t(attachment_height));
        scissor_rect.offset = {scaled_scissor_x0, scaled_scissor_y0};
        scissor_rect.extent = {
            uint32_t(std::max(scaled_scissor_x1 - scaled_scissor_x0, 0)),
            uint32_t(std::max(scaled_scissor_y1 - scaled_scissor_y0, 0))};
        fns_.CmdSetScissor(cmd, 0, 1, &scissor_rect);
        fns_.CmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
        const float blend_constants[4] = {
            regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
            regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
            regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
            regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
        };
        fns_.CmdSetBlendConstants(cmd, blend_constants);
        auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
        fns_.CmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                      rb_stencilrefmask.stencilmask);
        fns_.CmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                    rb_stencilrefmask.stencilwritemask);
        fns_.CmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                    rb_stencilrefmask.stencilref);

        RecordPrivateWaterVrsDraw(*pipe);
        fns_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
        fns_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->layout, 0, 4,
                                   sets, 0, nullptr);
        if (builtin_strip || fan_convert || quad_convert) {
            fns_.CmdBindIndexBuffer(cmd, generated_index_buffer.buffer, 0,
                                    VK_INDEX_TYPE_UINT32);
            fns_.CmdDrawIndexed(cmd, host_draw_vertex_count, 1, 0, 0, 0);
        } else if (indexed) {
            fns_.CmdBindIndexBuffer(cmd, guest_memory_buffer_, index_offset, index_type);
            fns_.CmdDrawIndexed(cmd, host_draw_vertex_count, 1, 0, 0, 0);
        } else {
            fns_.CmdDraw(cmd, host_draw_vertex_count, 1, 0, 0);
        }
        fns_.CmdEndRendering(cmd);

        const OwnedImage& readback_image =
            multisampled ? resolve_images[rt0_index] : color_images[rt0_index];
        ImageBarrier(cmd, readback_image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy_region = {};
        copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_region.imageExtent = {attachment_width, attachment_height, 1};
        if (readback_requested) {
            fns_.CmdCopyImageToBuffer(cmd, readback_image.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      readback.buffer, 1, &copy_region);
        }
        if (!defer_draw_waits_ &&
            fns_.EndCommandBuffer(cmd) != VK_SUCCESS) {
            return Fatal("command_buffer_end_failed");
        }

        VkFence fence = VK_NULL_HANDLE;
        if (!defer_draw_waits_) {
            VkFenceCreateInfo fence_info = {};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (fns_.CreateFence(device_, &fence_info, nullptr, &fence) !=
                VK_SUCCESS) {
                return Fatal("fence_creation_failed");
            }
            cleanup.fence = fence;
        }
        if (!defer_draw_waits_) {
            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            if (fns_.QueueSubmit(queue_, 1, &submit, fence) != VK_SUCCESS) {
                return Fatal("queue_submit_failed");
            }
        }
        CommitGuestTextureCache(guest_textures, cleanup);
        if (defer_draw_waits_) {
            HoldDeferredDrawResources(cleanup);
        } else {
            VkResult wait_result = fns_.WaitForFences(
                device_, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
            if (wait_result != VK_SUCCESS) {
                REXLOG_CAT_ERROR(kLogGPU,
                                 "nr2: native draw fence wait failed ({})",
                                 int32_t(wait_result));
                return Fatal("fence_wait_failed");
            }
            cleanup.skip_device_wait = true;
        }
        if (persistent_target) {
            const bool has_alias_seed =
                std::any_of(std::begin(alias_seed_images),
                            std::end(alias_seed_images),
                            [](const OwnedImage& image) {
                                return image.image != VK_NULL_HANDLE;
                            });
            const bool draw_unvalidated_safe =
                guest_texture_fallbacks == 0;
            if (load_persistent_target) {
                persistent_target->unvalidated_safe =
                    persistent_target->unvalidated_safe &&
                    draw_unvalidated_safe;
            } else {
                // A cleared target starts from known contents. Experimental
                // alias seeding is kept validation-only until its source
                // provenance is tracked per attachment.
                persistent_target->unvalidated_safe =
                    draw_unvalidated_safe && !has_alias_seed;
            }
            if (guest_texture_fallbacks) {
                persistent_target->unsafe_capture_index =
                    snap.capture_index;
                persistent_target->unsafe_vs_hash = snap.vs.ucode_hash;
                persistent_target->unsafe_ps_hash = snap.ps.ucode_hash;
                persistent_target->unsafe_texture_fallbacks =
                    guest_texture_fallbacks;
                persistent_target->unsafe_texture_set =
                    first_texture_fallback_set;
                persistent_target->unsafe_texture_binding =
                    first_texture_fallback_binding;
                persistent_target->unsafe_texture_fetch =
                    first_texture_fallback_fetch;
                persistent_target->unsafe_texture_format =
                    first_texture_fallback_format;
                persistent_target->unsafe_reason =
                    first_texture_fallback_reason;
            } else if (!load_persistent_target) {
                persistent_target->unsafe_capture_index = 0;
                persistent_target->unsafe_vs_hash = 0;
                persistent_target->unsafe_ps_hash = 0;
                persistent_target->unsafe_texture_fallbacks = 0;
                persistent_target->unsafe_texture_set = UINT32_MAX;
                persistent_target->unsafe_texture_binding = UINT32_MAX;
                persistent_target->unsafe_texture_fetch = UINT32_MAX;
                persistent_target->unsafe_texture_format = UINT32_MAX;
                persistent_target->unsafe_reason =
                    has_alias_seed ? "untracked_alias_seed" : "";
            }
            persistent_target->initialized = true;
            persistent_target->last_used_serial = ++persistent_target_use_serial_;
        }

        // --- Read back, count covered pixels, write the PPM. ---
        const uint8_t* pixels =
            readback_requested ? static_cast<const uint8_t*>(readback.mapped)
                               : nullptr;
        uint64_t covered = 0;
        const size_t pixel_count = size_t(attachment_width) * attachment_height;
        if (readback_requested) {
            for (size_t i = 0; i < pixel_count; ++i) {
                const uint8_t* px = pixels + i * rt0_bpp;
                bool nonzero = false;
                for (uint32_t b = 0; b < rt0_bpp; ++b) {
                    if (px[b]) {
                        nonzero = true;
                        break;
                    }
                }
                covered += nonzero;
            }
        }
        if (readback_requested && !covered && !snap.tool_relaxed) {
            // The draw ran but rasterized nothing (degenerate geometry,
            // off-screen position, or fully-zero output) - not usable as the
            // slice's visual evidence. Keep looking. (The trace harness keeps
            // zero-coverage results: an all-zero output can be legitimate -
            // e.g. a clear to transparent black - and parity is judged
            // externally against the EDRAM oracle.)
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: capture #{}: native draw rasterized 0 pixels ({}x{} "
                            "vs={:016x} mod={:016x} ps={:016x} topo={} count={}) - retrying",
                            snap.capture_index, rt_width, rt_height, snap.vs.ucode_hash,
                            pipe->desc.vs_modification, snap.ps.ucode_hash,
                            pipe->desc.state.primitive_topology(),
                            host_draw_vertex_count);
            return Reject(snap, "zero_coverage");
        }
        const std::string& output_path = REXCVAR_GET(ac6_native_draw_output);
        if (readback_requested) {
            WritePpm(output_path, pixels, attachment_width, attachment_height,
                     color_formats[rt0_index], rt0_bpp);
        }

        if (DetailedExecutionLogsEnabled(snap)) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: native draw executed, {} pixels covered{} ({}x{} RT0 format {}, capture #{}, "
                "{} {} idx={} endian={} topo={} vs={:016x} mod={:016x} ps={:016x} mod={:016x} "
                "rpkey=0x{:08x} state=0x{:012x} rt0=0x{:08x} score={} matches={} sharedmem={} "
                "viewport={}x{}+{}+{} guest_draw: prim={} count={} -> {})",
                covered, readback_requested ? "" : " (readback skipped)",
                attachment_width, attachment_height,
                uint32_t(color_formats[rt0_index]), snap.capture_index,
                builtin_strip
                    ? (fallback_strip_host_type == 10
                           ? (indexed ? "rect-expand-dma"
                                      : "rect-expand-auto")
                           : (indexed ? "point-expand-dma"
                                      : "point-expand-auto"))
                    : fan_convert
                          ? (indexed ? "fan-convert-dma"
                                     : "fan-convert-auto")
                    : quad_convert
                          ? (indexed ? "quad-convert-dma"
                                     : "quad-convert-auto")
                          : (indexed ? "indexed" : "auto"),
                host_draw_vertex_count, uint64_t(index_offset),
                uint32_t(shader_index_endian),
                pipe->desc.state.primitive_topology(), snap.vs.ucode_hash,
                pipe->desc.vs_modification, snap.ps.ucode_hash,
                pipe->desc.ps_modification, pipe->desc.render_pass_key.key,
                pipe->desc.state.bits, pipe->desc.render_targets[0].bits,
                chosen_score, match_count,
                guest_memory_imported_ ? "imported-zero-copy"
                                       : "copied-snapshot",
                viewport_info.xy_extent[0], viewport_info.xy_extent[1],
                viewport_info.xy_offset[0], viewport_info.xy_offset[1],
                snap.record.primitive_type, snap.record.count,
                snap.tool_suppress_output ? "(output suppressed)"
                                          : output_path);
        }
        return ExecResult::kExecuted;
    }

 private:
    ExecResult Reject(const DrawSnapshot& snap, const char* reason) {
        const size_t count = ++reject_counts_[reason];
        ++reject_total_;
        if (count <= 3 || (reject_total_ % 500) == 0) {
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: draw snapshot #{} rejected: {} (x{}, {} total rejects) - "
                            "re-arming for the next captured draw",
                            snap.capture_index, reason, count, reject_total_);
        }
        return ExecResult::kRejected;
    }

    ExecResult Fatal(const char* reason) {
        if (!session_failed_) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: native draw execution fatal: {} - disabling this "
                "private session",
                reason);
        }
        session_failed_ = true;
        return ExecResult::kFatal;
    }

    // Returns (creating on first use) a triangle-list, no-GS, no-restart
    // variant of a stored pipeline description - used to draw CPU-converted
    // quad lists whose stored pipeline expects the runtime quad geometry
    // shader with line-adjacency input.
    bool GetTriangleListVariant(const NativePipeline& base, const NativePipeline*& out) {
        constexpr uint32_t kTriangleListVariant = 1;
        const auto key = std::make_pair(base.desc.description_hash, kTriangleListVariant);
        auto it = variant_pipelines_.find(key);
        if (it != variant_pipelines_.end()) {
            out = &it->second;
            return true;
        }
        StoredPipeline patched = base.desc;
        patched.state.bits &=
            ~((0x3ull << 0) |   // geometry_shader -> kNone
              (0x7ull << 2) |   // primitive_topology
              (0x1ull << 5));   // primitive_restart
        patched.state.bits |= uint64_t(kTopologyTriangleList) << 2;
        std::string fail_reason;
        NativePipeline variant;
        if (!CreatePipeline(patched, fail_reason, &variant)) {
            REXLOG_CAT_WARN(kLogGPU,
                            "nr2: triangle-list variant pipeline creation failed ({})",
                            fail_reason);
            return false;
        }
        out = &variant_pipelines_.emplace(key, variant).first->second;
        return true;
    }

    // Returns (creating on first use) the SDK's no-GS primitive-expansion
    // variant of a stored point/rectangle geometry-shader pipeline. The
    // offline shader converter emits the matching HostVertexShaderType 9/10
    // SPIR-V module by replacing only that field in the stored modification.
    bool GetPrimitiveStripVariant(const NativePipeline& base, uint32_t host_type,
                                  const NativePipeline*& out) {
        if (host_type != 9 && host_type != 10) {
            return false;
        }
        // A single stored pipeline may be requested as both the SDK's point
        // and rectangle no-GS expansion variants (and, independently, as a
        // CPU-converted triangle list). Keep all three variants distinct.
        // Reusing the first-created variant here can select a shader/state
        // combination with different render-target writes later in a frame.
        const uint32_t variant_kind = 0x100u + host_type;
        const auto key = std::make_pair(base.desc.description_hash, variant_kind);
        auto it = variant_pipelines_.find(key);
        if (it != variant_pipelines_.end()) {
            out = &it->second;
            return true;
        }
        StoredPipeline patched = base.desc;
        constexpr uint64_t kHostVertexShaderTypeMask = UINT64_C(0xF) << 32;
        patched.vs_modification =
            (patched.vs_modification & ~kHostVertexShaderTypeMask) |
            (uint64_t(host_type) << 32);
        patched.state.bits &=
            ~((0x3ull << 0) |   // geometry_shader -> kNone
              (0x7ull << 2) |   // primitive_topology
              (0x1ull << 5));   // primitive_restart
        patched.state.bits |= uint64_t(kTopologyTriangleStrip) << 2;
        patched.state.bits |= uint64_t(1) << 5;
        std::string fail_reason;
        NativePipeline variant;
        if (!CreatePipeline(patched, fail_reason, &variant)) {
            REXLOG_CAT_WARN(kLogGPU,
                            "nr2: primitive-strip variant pipeline creation failed ({})",
                            fail_reason);
            return false;
        }
        out = &variant_pipelines_.emplace(key, variant).first->second;
        return true;
    }

    // Mirrors the FBO-relevant subset of VulkanCommandProcessor::
    // UpdateSystemConstantValues (command_processor.cpp:6009) operating on the
    // snapshot register file. EDRAM/FSI fields stay zero (FBO path).
    void BuildSystemConstants(const rex::graphics::RegisterFile& regs, const DrawSnapshot& snap,
                              const rex::graphics::draw_util::ViewportInfo& viewport_info,
                              rex::graphics::xenos::Endian index_endian,
                              XeSystemConstants& sys) {
        using namespace rex::graphics;
        uint32_t flags = 0;
        auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
        if (pa_cl_vte_cntl.vtx_xy_fmt) {
            flags |= kXeSysFlag_XYDividedByW;
        }
        if (pa_cl_vte_cntl.vtx_z_fmt) {
            flags |= kXeSysFlag_ZDividedByW;
        }
        if (pa_cl_vte_cntl.vtx_w0_fmt) {
            flags |= kXeSysFlag_WNotReciprocal;
        }
        if (draw_util::IsPrimitivePolygonal(regs)) {
            flags |= kXeSysFlag_PrimitivePolygonal;
        }
        if (draw_util::IsPrimitiveLine(regs)) {
            flags |= kXeSysFlag_PrimitiveLine;
        }
        auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
        flags |= uint32_t(rb_surface_info.msaa_samples) << kXeSysFlag_MsaaSamples_Shift;
        auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
        if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
            flags |= kXeSysFlag_DepthFloat24;
        }
        auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
        xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                         ? rb_colorcontrol.alpha_func
                                                         : xenos::CompareFunction::kAlways;
        flags |= uint32_t(alpha_test_function) << kXeSysFlag_AlphaPassIfLess_Shift;
        for (uint32_t i = 0; i < 4; ++i) {
            auto color_info =
                regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            if (!gamma_as_unorm16_ &&
                color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
                flags |= 1u << (kXeSysFlag_ConvertColor0ToGamma_Shift + i);
            }
            int32_t color_exp_bias = color_info.color_exp_bias;
            if ((color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
                 !rg16_snorm_supported_) ||
                (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
                 !rgba16_snorm_supported_)) {
                // SFLOAT fallback: remap the guest -32...32 range by biasing the
                // exponent (mirrors the SDK's IsFixedRG16TruncatedToMinus1To1
                // handling).
                color_exp_bias -= 5;
            }
            uint32_t color_exp_bias_scale =
                uint32_t(0x3F800000) + (uint32_t(color_exp_bias) << 23);
            std::memcpy(&sys.color_exp_bias[i], &color_exp_bias_scale, sizeof(float));
        }
        sys.flags = flags;

        sys.vertex_index_endian = uint32_t(index_endian);
        sys.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
        sys.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
        sys.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);

        auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
        if (!pa_cl_clip_cntl.clip_disable) {
            float* user_clip_plane_write_ptr = sys.user_clip_planes[0];
            uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
            while (user_clip_planes_remaining) {
                const uint32_t user_clip_plane_index =
                    uint32_t(__builtin_ctz(user_clip_planes_remaining));
                user_clip_planes_remaining &= user_clip_planes_remaining - 1;
                std::memcpy(user_clip_plane_write_ptr,
                            &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4],
                            4 * sizeof(float));
                user_clip_plane_write_ptr += 4;
            }
        }

        for (uint32_t i = 0; i < 3; ++i) {
            sys.ndc_scale[i] = viewport_info.ndc_scale[i];
            sys.ndc_offset[i] = viewport_info.ndc_offset[i];
        }

        // Point sprite expansion parameters (only meaningful for point lists).
        if (regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
            xenos::PrimitiveType::kPointList) {
            auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
            auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
            sys.point_vertex_diameter_min =
                float(uint32_t(pa_su_point_minmax.min_size)) * (2.0f / 16.0f);
            sys.point_vertex_diameter_max =
                float(uint32_t(pa_su_point_minmax.max_size)) * (2.0f / 16.0f);
            sys.point_constant_diameter[0] =
                float(uint32_t(pa_su_point_size.width)) * (2.0f / 16.0f);
            sys.point_constant_diameter[1] =
                float(uint32_t(pa_su_point_size.height)) * (2.0f / 16.0f);
            sys.point_screen_diameter_to_ndc_radius[0] =
                1.0f / float(std::max(viewport_info.xy_extent[0], 1u));
            sys.point_screen_diameter_to_ndc_radius[1] =
                1.0f / float(std::max(viewport_info.xy_extent[1], 1u));
        }

        // Texture host swizzle and component signedness. The guest textures
        // currently uploaded by nr2 use direct RGBA/BC host formats, whose
        // host-format swizzle is XYZW, so guest->host is the guest swizzle
        // itself (with invalid 6/7 selectors clamped to constant 0/1).
        uint32_t textures_seen = 0;
        auto add_texture_constants = [&](const DrawSnapshotShader& shader) {
            for (const DrawSnapshotTextureBinding& binding : shader.texture_bindings) {
                const uint32_t fetch_index = binding.fetch_constant;
                const uint32_t fetch_bit = UINT32_C(1) << fetch_index;
                if (textures_seen & fetch_bit) {
                    continue;
                }
                textures_seen |= fetch_bit;
                const xenos::xe_gpu_texture_fetch_t fetch =
                    regs.GetTextureFetch(fetch_index);

                uint32_t host_swizzle = 0;
                uint32_t host_format_swizzle =
                    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
                switch (GetBaseFormat(fetch.format)) {
                    case xenos::TextureFormat::k_24_8:
                    case xenos::TextureFormat::k_24_8_FLOAT:
                    case xenos::TextureFormat::k_32_FLOAT:
                        host_format_swizzle =
                            xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
                        break;
                    case xenos::TextureFormat::k_16_16_FLOAT:
                    case xenos::TextureFormat::k_32_32_FLOAT:
                        host_format_swizzle =
                            xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
                        break;
                    default:
                        break;
                }
                const uint32_t source_signs[4] = {
                    uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
                    uint32_t(fetch.sign_z), uint32_t(fetch.sign_w)};
                uint8_t swizzled_signs = 0;
                for (uint32_t component = 0; component < 4; ++component) {
                    const uint32_t guest_selector =
                        (fetch.swizzle >> (3 * component)) & 7u;
                    uint32_t host_selector;
                    if (guest_selector >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
                        host_selector = guest_selector & 5u;
                    } else {
                        host_selector =
                            (host_format_swizzle >> (3 * guest_selector)) & 7u;
                    }
                    host_swizzle |= host_selector << (3 * component);
                    const uint32_t sign =
                        guest_selector < 4
                            ? source_signs[guest_selector]
                            : uint32_t(xenos::TextureSign::kUnsigned);
                    swizzled_signs |= uint8_t(sign << (2 * component));
                }
                uint32_t& swizzle_word = sys.texture_swizzles[fetch_index >> 1];
                const uint32_t swizzle_shift = 12 * (fetch_index & 1);
                swizzle_word |= host_swizzle << swizzle_shift;
                uint32_t& signs_word = sys.texture_swizzled_signs[fetch_index >> 2];
                signs_word |= uint32_t(swizzled_signs) << (8 * (fetch_index & 3));
            }
        };
        add_texture_constants(snap.vs);
        add_texture_constants(snap.ps);

        sys.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
        sys.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                                ? ((rb_colorcontrol.value >> 24) | (1u << 8))
                                : 0;

        sys.tessellation_factor_range_min =
            regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
        sys.tessellation_factor_range_max =
            regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    }

    // Packs float constants the way the SDK's constant upload path does
    // (command_processor.cpp:6604): tightly per the shader's float bitmap.
    static void PackFloatConstants(const DrawSnapshotShader& shader,
                                   const rex::graphics::RegisterFile& regs,
                                   uint32_t base_register, std::vector<uint8_t>& out) {
        if (shader.float_dynamic_addressing) {
            out.resize(sizeof(float) * 4 * 256);
            std::memcpy(out.data(), &regs[base_register], out.size());
            return;
        }
        const uint32_t count = std::max(shader.float_count, 1u);
        out.assign(sizeof(float) * 4 * count, 0);
        uint8_t* write_ptr = out.data();
        for (uint32_t i = 0; i < 4; ++i) {
            uint64_t map = shader.float_bitmap[i];
            while (map) {
                const uint32_t bit = uint32_t(__builtin_ctzll(map));
                map &= map - 1;
                if (write_ptr + sizeof(float) * 4 > out.data() + out.size()) {
                    return;  // float_count/bitmap mismatch - keep what fits
                }
                std::memcpy(write_ptr, &regs[base_register + (i << 8) + (bit << 2)],
                            sizeof(float) * 4);
                write_ptr += sizeof(float) * 4;
            }
        }
    }

    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct OwnedImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        // Dimensions of mip 0, needed to stage uploads into the image.
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t array_layers = 1;
    };

    struct PersistentTargetKey {
        uint32_t render_pass_key = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t surface_info = 0;
        uint32_t color_info[4] = {};
        uint32_t depth_info = 0;

        bool operator==(const PersistentTargetKey& other) const {
            return render_pass_key == other.render_pass_key &&
                   width == other.width && height == other.height &&
                   surface_info == other.surface_info &&
                   std::memcmp(color_info, other.color_info,
                               sizeof(color_info)) == 0 &&
                   depth_info == other.depth_info;
        }
    };

    struct PersistentTarget {
        bool valid = false;
        bool initialized = false;
        bool multisampled = false;
        bool unvalidated_safe = false;
        uint64_t last_used_serial = 0;
        uint64_t unsafe_capture_index = 0;
        uint64_t unsafe_vs_hash = 0;
        uint64_t unsafe_ps_hash = 0;
        uint32_t unsafe_texture_fallbacks = 0;
        uint32_t unsafe_texture_set = UINT32_MAX;
        uint32_t unsafe_texture_binding = UINT32_MAX;
        uint32_t unsafe_texture_fetch = UINT32_MAX;
        uint32_t unsafe_texture_format = UINT32_MAX;
        std::string unsafe_reason;
        PersistentTargetKey key{};
        OwnedImage color[4]{};
        OwnedImage resolve[4]{};
        OwnedImage depth{};
    };

    struct GuestTextureUpload {
        OwnedImage image{};
        HostBuffer staging{};
        uint32_t fetch_constant = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkDeviceSize staging_size = 0;
        bool needs_upload = true;
        bool cacheable = false;
        uint64_t content_hash = 0;
        std::string cache_key;
    };

    struct CachedGuestTexture {
        OwnedImage image{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint64_t content_hash = 0;
        uint64_t last_used_serial = 0;
    };

    // Ephemeral per-execution resources, destroyed on scope exit (after a
    // device-idle wait so in-flight work can never outlive them).
    struct EphemeralCleanup {
        explicit EphemeralCleanup(PipelineProver* prover) : prover(prover) {}
        ~EphemeralCleanup() {
            PipelineProver& p = *prover;
            if (!skip_device_wait && p.fns_.DeviceWaitIdle) {
                p.fns_.DeviceWaitIdle(p.device_);
            }
            if (fence) {
                p.fns_.DestroyFence(p.device_, fence, nullptr);
            }
            if (command_pool) {
                p.fns_.DestroyCommandPool(p.device_, command_pool, nullptr);
            }
            if (descriptor_pool) {
                p.fns_.DestroyDescriptorPool(p.device_, descriptor_pool, nullptr);
            }
            for (VkSampler sampler : samplers) {
                p.fns_.DestroySampler(p.device_, sampler, nullptr);
            }
            for (const OwnedImage& image : images) {
                if (image.view) p.fns_.DestroyImageView(p.device_, image.view, nullptr);
                if (image.image) p.fns_.DestroyImage(p.device_, image.image, nullptr);
                if (image.memory) p.fns_.FreeMemory(p.device_, image.memory, nullptr);
            }
            for (const HostBuffer& buffer : buffers) {
                if (buffer.mapped) p.fns_.UnmapMemory(p.device_, buffer.memory);
                if (buffer.buffer) p.fns_.DestroyBuffer(p.device_, buffer.buffer, nullptr);
                if (buffer.memory) p.fns_.FreeMemory(p.device_, buffer.memory, nullptr);
            }
        }
        PipelineProver* prover;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        std::vector<VkSampler> samplers;
        std::vector<OwnedImage> images;
        std::vector<HostBuffer> buffers;
        bool skip_device_wait = false;
    };

    struct DeferredDrawDrainGuard {
        explicit DeferredDrawDrainGuard(PipelineProver* prover)
            : prover(prover) {}
        ~DeferredDrawDrainGuard() {
            prover->DrainDeferredDrawResources();
        }
        PipelineProver* prover;
    };

    void HoldDeferredDrawResources(EphemeralCleanup& cleanup) {
        auto held = std::make_unique<EphemeralCleanup>(this);
        held->command_pool = std::exchange(cleanup.command_pool, VK_NULL_HANDLE);
        held->descriptor_pool =
            std::exchange(cleanup.descriptor_pool, VK_NULL_HANDLE);
        held->samplers = std::move(cleanup.samplers);
        held->images = std::move(cleanup.images);
        held->buffers = std::move(cleanup.buffers);
        held->skip_device_wait = true;
        cleanup.skip_device_wait = true;
        deferred_draw_resources_.push_back(std::move(held));
        ++deferred_batch_draw_count_;
    }

    void ReleaseDeferredDrawResources() {
        deferred_draw_resources_.clear();
    }

    bool SubmitDeferredDrawCommandBuffers() {
        if (deferred_batch_command_buffer_ == VK_NULL_HANDLE) {
            return true;
        }
        if (fns_.EndCommandBuffer(deferred_batch_command_buffer_) !=
            VK_SUCCESS) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: deferred batch command buffer end failed");
            // Its recording state is no longer reusable. Keep the owning
            // pool alive until a proven device idle, but never hand this
            // handle back to a later draw.
            deferred_batch_command_buffer_ = VK_NULL_HANDLE;
            session_failed_ = true;
            return false;
        }
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &deferred_batch_command_buffer_;
        const VkResult result = fns_.QueueSubmit(queue_, 1, &submit,
                                                 VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: deferred draw batch submit failed for {} draws ({})",
                deferred_batch_draw_count_, int32_t(result));
            deferred_batch_command_buffer_ = VK_NULL_HANDLE;
            session_failed_ = true;
            return false;
        }
        deferred_batch_command_buffer_ = VK_NULL_HANDLE;
        return true;
    }

    bool EnsureDeferredBatchCommandBuffer() {
        if (deferred_batch_command_buffer_ != VK_NULL_HANDLE) {
            return true;
        }
        if (!EnsureDeferredBatchPools()) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = deferred_command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (fns_.AllocateCommandBuffers(device_, &alloc,
                                        &deferred_batch_command_buffer_) !=
            VK_SUCCESS) {
            deferred_batch_command_buffer_ = VK_NULL_HANDLE;
            return false;
        }
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns_.BeginCommandBuffer(deferred_batch_command_buffer_, &begin) !=
            VK_SUCCESS) {
            deferred_batch_command_buffer_ = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    // A resolve waits for its fence before touching the mapped readback, so
    // the submission objects are no longer in flight when the next resolve
    // begins. Recycle them instead of making the Vulkan driver allocate and
    // destroy a pool, command buffer, and fence every presented frame.
    bool EnsureResolveSubmissionResources() {
        if (resolve_command_pool_ != VK_NULL_HANDLE) {
            return true;
        }
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (fns_.CreateCommandPool(device_, &pool_info, nullptr,
                                   &resolve_command_pool_) != VK_SUCCESS) {
            DestroyResolveSubmissionResources();
            return false;
        }
        VkCommandBufferAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = resolve_command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (fns_.AllocateCommandBuffers(device_, &alloc,
                                        &resolve_command_buffer_) !=
            VK_SUCCESS) {
            DestroyResolveSubmissionResources();
            return false;
        }
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (fns_.CreateFence(device_, &fence_info, nullptr,
                             &resolve_fence_) != VK_SUCCESS) {
            DestroyResolveSubmissionResources();
            return false;
        }
        return true;
    }

    bool CreateGpuResolvePipeline() {
        if (gpu_resolve_pipeline_ != VK_NULL_HANDLE) {
            return true;
        }
        if (gpu_resolve_spirv_.empty()) {
            return false;
        }

        VkShaderModuleCreateInfo shader_info = {};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize =
            gpu_resolve_spirv_.size() * sizeof(gpu_resolve_spirv_[0]);
        shader_info.pCode = gpu_resolve_spirv_.data();
        VkShaderModule shader_module = VK_NULL_HANDLE;
        if (fns_.CreateShaderModule(device_, &shader_info, nullptr,
                                    &shader_module) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo set_layout_info = {};
        set_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_info.bindingCount = uint32_t(std::size(bindings));
        set_layout_info.pBindings = bindings;
        if (fns_.CreateDescriptorSetLayout(
                device_, &set_layout_info, nullptr,
                &gpu_resolve_set_layout_) != VK_SUCCESS) {
            fns_.DestroyShaderModule(device_, shader_module, nullptr);
            DestroyGpuResolvePipeline();
            return false;
        }

        constexpr uint32_t kPushConstantSize = 12u * sizeof(uint32_t);
        VkPushConstantRange push_range = {};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.size = kPushConstantSize;
        VkPipelineLayoutCreateInfo pipeline_layout_info = {};
        pipeline_layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &gpu_resolve_set_layout_;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        if (fns_.CreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                      &gpu_resolve_pipeline_layout_) !=
            VK_SUCCESS) {
            fns_.DestroyShaderModule(device_, shader_module, nullptr);
            DestroyGpuResolvePipeline();
            return false;
        }

        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader_module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = gpu_resolve_pipeline_layout_;
        const VkResult pipeline_result = fns_.CreateComputePipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
            &gpu_resolve_pipeline_);
        fns_.DestroyShaderModule(device_, shader_module, nullptr);
        if (pipeline_result != VK_SUCCESS) {
            DestroyGpuResolvePipeline();
            return false;
        }

        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0f;
        if (fns_.CreateSampler(device_, &sampler_info, nullptr,
                               &gpu_resolve_sampler_) != VK_SUCCESS) {
            DestroyGpuResolvePipeline();
            return false;
        }

        const VkDescriptorPoolSize pool_sizes[2] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = uint32_t(std::size(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;
        if (fns_.CreateDescriptorPool(device_, &pool_info, nullptr,
                                      &gpu_resolve_descriptor_pool_) !=
            VK_SUCCESS) {
            DestroyGpuResolvePipeline();
            return false;
        }
        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = gpu_resolve_descriptor_pool_;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &gpu_resolve_set_layout_;
        if (fns_.AllocateDescriptorSets(device_, &allocate_info,
                                        &gpu_resolve_descriptor_set_) !=
            VK_SUCCESS) {
            DestroyGpuResolvePipeline();
            return false;
        }
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: direct RGBA8 GPU resolve pipeline ready (32x8 pixels per "
            "workgroup)");
        return true;
    }

    void DestroyGpuResolvePipeline() {
        gpu_resolve_descriptor_set_ = VK_NULL_HANDLE;
        if (gpu_resolve_descriptor_pool_ != VK_NULL_HANDLE) {
            fns_.DestroyDescriptorPool(device_, gpu_resolve_descriptor_pool_,
                                       nullptr);
            gpu_resolve_descriptor_pool_ = VK_NULL_HANDLE;
        }
        if (gpu_resolve_sampler_ != VK_NULL_HANDLE) {
            fns_.DestroySampler(device_, gpu_resolve_sampler_, nullptr);
            gpu_resolve_sampler_ = VK_NULL_HANDLE;
        }
        if (gpu_resolve_pipeline_ != VK_NULL_HANDLE) {
            fns_.DestroyPipeline(device_, gpu_resolve_pipeline_, nullptr);
            gpu_resolve_pipeline_ = VK_NULL_HANDLE;
        }
        if (gpu_resolve_pipeline_layout_ != VK_NULL_HANDLE) {
            fns_.DestroyPipelineLayout(device_, gpu_resolve_pipeline_layout_,
                                       nullptr);
            gpu_resolve_pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (gpu_resolve_set_layout_ != VK_NULL_HANDLE) {
            fns_.DestroyDescriptorSetLayout(device_, gpu_resolve_set_layout_,
                                            nullptr);
            gpu_resolve_set_layout_ = VK_NULL_HANDLE;
        }
    }

    ExecResult DispatchGpuResolve(const OwnedImage& source_image,
                                  uint32_t x0, uint32_t y0, uint32_t x1,
                                  uint32_t y1, uint32_t dest_pitch,
                                  uint32_t dest_height, uint32_t dest_base,
                                  bool swap_rb, uint32_t dest_endian,
                                  HostBuffer* output_override = nullptr) {
        const bool writable_output_buffer = output_override
            ? output_override->buffer != VK_NULL_HANDLE &&
                  output_override->mapped != nullptr
            : guest_memory_imported_ && guest_memory_import_coherent_;
        if (gpu_resolve_pipeline_ == VK_NULL_HANDLE ||
            gpu_resolve_descriptor_set_ == VK_NULL_HANDLE ||
            (!output_override && guest_memory_buffer_ == VK_NULL_HANDLE) ||
            !writable_output_buffer || (dest_base & 3u)) {
            return ExecResult::kRejected;
        }
        const VkDeviceSize write_lower =
            VkDeviceSize(dest_base) +
            rex::graphics::texture_util::GetTiledAddressLowerBound2D(
                x0, y0, dest_pitch, 2);
        const VkDeviceSize write_upper =
            std::min<VkDeviceSize>(
                VkDeviceSize(dest_base) +
                    rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                        x1, y1, dest_pitch, 2),
                guest_memory_size_);
        if (write_lower >= write_upper) {
            return ExecResult::kRejected;
        }
        const VkBuffer output_buffer = output_override
            ? output_override->buffer
            : guest_memory_buffer_;
        const VkDeviceSize output_guest_base =
            output_override ? write_lower : 0;
        const VkDeviceSize output_size = output_override
            ? write_upper - write_lower
            : guest_memory_size_;
        if (!output_size ||
            (output_override && output_override->size < output_size)) {
            return ExecResult::kRejected;
        }
        if (!EnsureResolveSubmissionResources()) {
            return Fatal("gpu_resolve_submission_resource_creation_failed");
        }
        if (resolve_submission_in_flight_) {
            return Fatal("gpu_resolve_submission_still_in_flight");
        }
        if (resolve_submission_used_) {
            if (fns_.ResetFences(device_, 1, &resolve_fence_) != VK_SUCCESS) {
                return Fatal("gpu_resolve_fence_reset_failed");
            }
            if (fns_.ResetCommandPool(device_, resolve_command_pool_, 0) !=
                VK_SUCCESS) {
                return Fatal("gpu_resolve_command_pool_reset_failed");
            }
        }

        VkDescriptorImageInfo image_info = {};
        image_info.sampler = gpu_resolve_sampler_;
        image_info.imageView = source_image.view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo buffer_info = {};
        buffer_info.buffer = output_buffer;
        buffer_info.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = gpu_resolve_descriptor_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = gpu_resolve_descriptor_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer_info;
        fns_.UpdateDescriptorSets(device_, uint32_t(std::size(writes)), writes,
                                  0, nullptr);

        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns_.BeginCommandBuffer(resolve_command_buffer_, &begin) !=
            VK_SUCCESS) {
            return Fatal("gpu_resolve_command_buffer_begin_failed");
        }
        VkImageMemoryBarrier to_shader_read = {};
        to_shader_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_shader_read.srcAccessMask =
            VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader_read.image = source_image.image;
        to_shader_read.subresourceRange =
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // The same imported buffer may still be serving index and shader
        // reads from the preceding fence-less draw batch. Establish the WAR
        // dependency before the compute shader overwrites this resolve range;
        // queue submission order alone is not an execution dependency across
        // graphics and compute stages.
        VkBufferMemoryBarrier prior_guest_reads = {};
        if (!output_override) {
            prior_guest_reads.sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            prior_guest_reads.srcAccessMask =
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT;
            prior_guest_reads.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            prior_guest_reads.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            prior_guest_reads.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            prior_guest_reads.buffer = output_buffer;
            prior_guest_reads.offset = write_lower;
            prior_guest_reads.size = write_upper - write_lower;
        }
        fns_.CmdPipelineBarrier(
            resolve_command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
            output_override ? 0u : 1u,
            output_override ? nullptr : &prior_guest_reads, 1,
            &to_shader_read);

        const uint32_t push_constants[12] = {
            x0,
            y0,
            x1,
            y1,
            source_image.width,
            source_image.height,
            dest_pitch,
            dest_height,
            dest_base,
            uint32_t(output_guest_base),
            uint32_t(swap_rb) | (dest_endian == 2u ? 2u : 0u),
            uint32_t(std::min<VkDeviceSize>(output_size, UINT32_MAX)),
        };
        fns_.CmdBindPipeline(resolve_command_buffer_,
                             VK_PIPELINE_BIND_POINT_COMPUTE,
                             gpu_resolve_pipeline_);
        fns_.CmdBindDescriptorSets(
            resolve_command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
            gpu_resolve_pipeline_layout_, 0, 1,
            &gpu_resolve_descriptor_set_, 0, nullptr);
        fns_.CmdPushConstants(resolve_command_buffer_,
                              gpu_resolve_pipeline_layout_,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(push_constants), push_constants);
        fns_.CmdDispatch(resolve_command_buffer_, (x1 - x0 + 31u) / 32u,
                         (y1 - y0 + 7u) / 8u, 1);

        VkImageMemoryBarrier to_transfer_source = {};
        to_transfer_source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer_source.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_transfer_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_transfer_source.oldLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_transfer_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_transfer_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer_source.image = source_image.image;
        to_transfer_source.subresourceRange =
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkBufferMemoryBarrier guest_memory_barrier = {};
        guest_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        guest_memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        guest_memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        guest_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        guest_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        guest_memory_barrier.buffer = output_buffer;
        guest_memory_barrier.offset = output_override ? 0 : write_lower;
        guest_memory_barrier.size = output_override
            ? output_size
            : write_upper - write_lower;
        guest_memory_barrier.dstAccessMask |= VK_ACCESS_MEMORY_READ_BIT;
        fns_.CmdPipelineBarrier(
            resolve_command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
            nullptr, 1, &guest_memory_barrier, 1, &to_transfer_source);

        if (fns_.EndCommandBuffer(resolve_command_buffer_) != VK_SUCCESS) {
            return Fatal("gpu_resolve_command_buffer_end_failed");
        }
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &resolve_command_buffer_;
        if (fns_.QueueSubmit(queue_, 1, &submit, resolve_fence_) !=
            VK_SUCCESS) {
            return Fatal("gpu_resolve_queue_submit_failed");
        }
        resolve_submission_used_ = true;
        resolve_submission_in_flight_ = true;
        const VkResult wait = fns_.WaitForFences(
            device_, 1, &resolve_fence_, VK_TRUE,
            10ull * 1000 * 1000 * 1000);
        if (wait != VK_SUCCESS) {
            return Fatal("gpu_resolve_fence_wait_failed");
        }
        resolve_submission_in_flight_ = false;
        // This fence covers the preceding fence-less draw batch too. Retire
        // every batch-scoped object now so the fallback guard has no reason
        // to issue a redundant vkDeviceWaitIdle.
        ReleaseDeferredDrawResources();
        DestroyDeferredBatchPools();
        ++gpu_resolve_count_;
        return ExecResult::kExecuted;
    }

    void DestroyResolveSubmissionResources() {
        if (resolve_fence_ != VK_NULL_HANDLE) {
            fns_.DestroyFence(device_, resolve_fence_, nullptr);
            resolve_fence_ = VK_NULL_HANDLE;
        }
        resolve_command_buffer_ = VK_NULL_HANDLE;
        if (resolve_command_pool_ != VK_NULL_HANDLE) {
            fns_.DestroyCommandPool(device_, resolve_command_pool_, nullptr);
            resolve_command_pool_ = VK_NULL_HANDLE;
        }
        resolve_submission_used_ = false;
        resolve_submission_in_flight_ = false;
    }

    // Deferred live draws are submitted in guest order and drained at the
    // next resolve/copy boundary. Command pools and descriptor pools have the
    // same lifetime, so creating one of each per draw only adds driver work
    // and allocator churn. Allocate command buffers and descriptor sets from
    // batch-scoped pools instead, then destroy both after the queue is idle.
    bool EnsureDeferredBatchPools() {
        constexpr VkDeviceSize kDeferredUniformArenaSize =
            VkDeviceSize(32) << 20;
        if (deferred_uniform_buffer_.buffer == VK_NULL_HANDLE &&
            !CreateHostBuffer(kDeferredUniformArenaSize,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              deferred_uniform_buffer_)) {
            return false;
        }
        if (deferred_command_pool_ == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo command_pool_info = {};
            command_pool_info.sType =
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            command_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            command_pool_info.queueFamilyIndex = queue_family_;
            if (fns_.CreateCommandPool(device_, &command_pool_info, nullptr,
                                       &deferred_command_pool_) != VK_SUCCESS) {
                deferred_command_pool_ = VK_NULL_HANDLE;
                return false;
            }
        }
        if (deferred_descriptor_pool_ == VK_NULL_HANDLE) {
            // Four sets and at most five constant UBO descriptors are used by
            // each draw. The larger image/sampler allowances cover heavily
            // textured passes while keeping the normal resolve-delimited
            // batches far below pool capacity.
            const VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
            };
            VkDescriptorPoolCreateInfo descriptor_pool_info = {};
            descriptor_pool_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            descriptor_pool_info.maxSets = 4096;
            descriptor_pool_info.poolSizeCount =
                uint32_t(std::size(pool_sizes));
            descriptor_pool_info.pPoolSizes = pool_sizes;
            if (fns_.CreateDescriptorPool(device_, &descriptor_pool_info,
                                          nullptr,
                                          &deferred_descriptor_pool_) !=
                VK_SUCCESS) {
                fns_.DestroyCommandPool(device_, deferred_command_pool_,
                                        nullptr);
                deferred_command_pool_ = VK_NULL_HANDLE;
                deferred_descriptor_pool_ = VK_NULL_HANDLE;
                return false;
            }
            ++deferred_batch_count_;
        }
        return true;
    }

    void DestroyDeferredBatchPools() {
        deferred_batch_command_buffer_ = VK_NULL_HANDLE;
        deferred_uniform_offset_ = 0;
        if (device_ == VK_NULL_HANDLE) {
            deferred_command_pool_ = VK_NULL_HANDLE;
            deferred_descriptor_pool_ = VK_NULL_HANDLE;
            deferred_batch_draw_count_ = 0;
            return;
        }
        if (deferred_command_pool_ != VK_NULL_HANDLE) {
            fns_.DestroyCommandPool(device_, deferred_command_pool_, nullptr);
            deferred_command_pool_ = VK_NULL_HANDLE;
        }
        if (deferred_descriptor_pool_ != VK_NULL_HANDLE) {
            fns_.DestroyDescriptorPool(device_, deferred_descriptor_pool_,
                                       nullptr);
            deferred_descriptor_pool_ = VK_NULL_HANDLE;
        }
        deferred_batch_draw_count_ = 0;
    }

    void DrainDeferredDrawResources() {
        if (deferred_batch_command_buffer_ == VK_NULL_HANDLE &&
            deferred_draw_resources_.empty() &&
            deferred_command_pool_ == VK_NULL_HANDLE &&
            deferred_descriptor_pool_ == VK_NULL_HANDLE) {
            return;
        }
        const bool submitted = SubmitDeferredDrawCommandBuffers();
        bool queue_drained = false;
        bool device_idle_attempted = false;
        auto wait_device_idle = [&] {
            device_idle_attempted = true;
            if (!fns_.DeviceWaitIdle ||
                fns_.DeviceWaitIdle(device_) != VK_SUCCESS) {
                return false;
            }
            resolve_submission_in_flight_ = false;
            return true;
        };
        // A prior timed-out fence does not prove that its command buffer has
        // stopped executing. Never reset that fence or command pool until a
        // successful device wait establishes completion.
        if (session_failed_) {
            queue_drained = wait_device_idle();
        } else if (resolve_submission_in_flight_) {
            queue_drained = wait_device_idle();
        } else if (submitted && EnsureResolveSubmissionResources()) {
            bool reset_ok = true;
            if (resolve_submission_used_) {
                reset_ok =
                    fns_.ResetFences(device_, 1, &resolve_fence_) ==
                        VK_SUCCESS &&
                    fns_.ResetCommandPool(device_, resolve_command_pool_, 0) ==
                        VK_SUCCESS;
            }
            VkCommandBufferBeginInfo begin = {};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (reset_ok &&
                fns_.BeginCommandBuffer(resolve_command_buffer_, &begin) ==
                    VK_SUCCESS &&
                fns_.EndCommandBuffer(resolve_command_buffer_) == VK_SUCCESS) {
                VkSubmitInfo submit = {};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &resolve_command_buffer_;
                if (fns_.QueueSubmit(queue_, 1, &submit, resolve_fence_) ==
                    VK_SUCCESS) {
                    resolve_submission_used_ = true;
                    resolve_submission_in_flight_ = true;
                    queue_drained = fns_.WaitForFences(
                        device_, 1, &resolve_fence_, VK_TRUE,
                        10ull * 1000 * 1000 * 1000) == VK_SUCCESS;
                    if (queue_drained) {
                        resolve_submission_in_flight_ = false;
                    }
                }
            }
        }
        if (!queue_drained && !device_idle_attempted) {
            queue_drained = wait_device_idle();
        }
        if (!queue_drained) {
            // Leaking these batch objects until device teardown is safer than
            // destroying a potentially pending command buffer or descriptor
            // resource after a timeout/device failure.
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: could not prove deferred queue completion; retaining "
                "batch resources to avoid destroying in-flight Vulkan "
                "objects");
            return;
        }
        ReleaseDeferredDrawResources();
        DestroyDeferredBatchPools();
    }

    void CleanupImages(EphemeralCleanup& cleanup, OwnedImage* images, OwnedImage* depth) {
        // Images already pushed into cleanup are destroyed there; this handles
        // the partially-created current one - nothing to do since creation
        // failure destroys its own partial state.
        (void)cleanup;
        (void)images;
        (void)depth;
    }

    void DestroyOwnedImage(OwnedImage& image) {
        if (device_ == VK_NULL_HANDLE) {
            image = {};
            return;
        }
        if (image.view) fns_.DestroyImageView(device_, image.view, nullptr);
        if (image.image) fns_.DestroyImage(device_, image.image, nullptr);
        if (image.memory) fns_.FreeMemory(device_, image.memory, nullptr);
        image = {};
    }

    void DestroyGuestTextureCache() {
        if (guest_texture_hash_hits_ || guest_texture_hash_misses_) {
            REXLOG_CAT_INFO(
                kLogGPU,
                "nr2: guest texture allocation hashes: {} memo hits, {} "
                "misses, {} MiB hashed",
                guest_texture_hash_hits_, guest_texture_hash_misses_,
                guest_texture_hash_bytes_ >> 20);
        }
        for (auto& [key, texture] : guest_texture_cache_) {
            DestroyOwnedImage(texture.image);
        }
        guest_texture_cache_.clear();
        guest_texture_allocation_hashes_.clear();
        guest_texture_cache_use_serial_ = 0;
        guest_texture_hash_hits_ = 0;
        guest_texture_hash_misses_ = 0;
        guest_texture_hash_bytes_ = 0;
    }

    // Transfer successfully submitted texture images from per-draw cleanup
    // into the session cache. Replaced or evicted images move in the opposite
    // direction so they remain alive until this draw batch reaches its next
    // queue-idle resolve boundary.
    void CommitGuestTextureCache(std::vector<GuestTextureUpload>& uploads,
                                 EphemeralCleanup& cleanup) {
        constexpr size_t kMaxCachedGuestTextures = 192;
        for (GuestTextureUpload& upload : uploads) {
            if (!upload.cacheable || !upload.needs_upload ||
                upload.cache_key.empty() || !upload.image.image) {
                continue;
            }
            const auto owned_it = std::find_if(
                cleanup.images.begin(), cleanup.images.end(),
                [&](const OwnedImage& image) {
                    return image.image == upload.image.image;
                });
            if (owned_it == cleanup.images.end()) {
                continue;
            }
            const OwnedImage new_image = *owned_it;
            cleanup.images.erase(owned_it);

            auto [cache_it, inserted] = guest_texture_cache_.try_emplace(
                upload.cache_key);
            CachedGuestTexture& cached = cache_it->second;
            if (!inserted && cached.image.image) {
                cleanup.images.push_back(cached.image);
            }
            cached.image = new_image;
            cached.format = upload.format;
            cached.content_hash = upload.content_hash;
            cached.last_used_serial = ++guest_texture_cache_use_serial_;
        }

        while (guest_texture_cache_.size() > kMaxCachedGuestTextures) {
            auto victim = std::min_element(
                guest_texture_cache_.begin(), guest_texture_cache_.end(),
                [](const auto& a, const auto& b) {
                    return a.second.last_used_serial <
                           b.second.last_used_serial;
                });
            if (victim == guest_texture_cache_.end()) {
                break;
            }
            if (victim->second.image.image) {
                cleanup.images.push_back(victim->second.image);
            }
            guest_texture_cache_.erase(victim);
        }
    }

    void DestroyPersistentTarget() {
        if (persistent_targets_.empty()) {
            return;
        }
        if (device_ != VK_NULL_HANDLE && fns_.DeviceWaitIdle) {
            fns_.DeviceWaitIdle(device_);
        }
        for (PersistentTarget& target : persistent_targets_) {
            for (uint32_t rt = 0; rt < 4; ++rt) {
                DestroyOwnedImage(target.color[rt]);
                DestroyOwnedImage(target.resolve[rt]);
            }
            DestroyOwnedImage(target.depth);
        }
        persistent_targets_.clear();
    }

    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required) {
        VkPhysicalDeviceMemoryProperties memory_properties;
        fns_.GetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & required) == required) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          HostBuffer& out, bool prefer_host_cached = false) {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (fns_.CreateBuffer(device_, &buffer_info, nullptr, &out.buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements;
        fns_.GetBufferMemoryRequirements(device_, out.buffer, &requirements);
        uint32_t type = UINT32_MAX;
        if (prefer_host_cached) {
            type = FindMemoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (type == UINT32_MAX) {
            type = FindMemoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = requirements.size;
        alloc_info.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            fns_.AllocateMemory(device_, &alloc_info, nullptr, &out.memory) != VK_SUCCESS) {
            fns_.DestroyBuffer(device_, out.buffer, nullptr);
            out.buffer = VK_NULL_HANDLE;
            return false;
        }
        if (fns_.BindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS ||
            fns_.MapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) !=
                VK_SUCCESS) {
            fns_.DestroyBuffer(device_, out.buffer, nullptr);
            fns_.FreeMemory(device_, out.memory, nullptr);
            out = {};
            return false;
        }
        out.size = size;
        return true;
    }

    void DestroyHostBuffer(HostBuffer& buffer) {
        if (device_ == VK_NULL_HANDLE) {
            buffer = {};
            return;
        }
        if (buffer.mapped) {
            fns_.UnmapMemory(device_, buffer.memory);
        }
        if (buffer.buffer) {
            fns_.DestroyBuffer(device_, buffer.buffer, nullptr);
        }
        if (buffer.memory) {
            fns_.FreeMemory(device_, buffer.memory, nullptr);
        }
        buffer = {};
    }

    bool EnsureHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          HostBuffer& buffer,
                          bool prefer_host_cached = false) {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.mapped &&
            buffer.size >= size) {
            return true;
        }
        if (buffer.buffer != VK_NULL_HANDLE ||
            buffer.memory != VK_NULL_HANDLE) {
            if (fns_.DeviceWaitIdle) {
                fns_.DeviceWaitIdle(device_);
            }
            DestroyHostBuffer(buffer);
        }
        return CreateHostBuffer(size, usage, buffer, prefer_host_cached);
    }

    bool CreateOwnedImage(VkFormat format, uint32_t width, uint32_t height,
                          VkImageUsageFlags usage, VkImageAspectFlags aspect, OwnedImage& out,
                          VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                          VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
                          uint32_t array_layers = 1, VkImageCreateFlags create_flags = 0,
                          VkImageType image_type = VK_IMAGE_TYPE_2D, uint32_t depth = 1,
                          VkComponentMapping components = {}) {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.flags = create_flags;
        image_info.imageType = image_type;
        image_info.format = format;
        image_info.extent = {width, height, depth};
        image_info.mipLevels = 1;
        image_info.arrayLayers = array_layers;
        image_info.samples = samples;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (fns_.CreateImage(device_, &image_info, nullptr, &out.image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements;
        fns_.GetImageMemoryRequirements(device_, out.image, &requirements);
        const uint32_t type =
            FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = requirements.size;
        alloc_info.memoryTypeIndex =
            type != UINT32_MAX ? type : FindMemoryType(requirements.memoryTypeBits, 0);
        if (alloc_info.memoryTypeIndex == UINT32_MAX ||
            fns_.AllocateMemory(device_, &alloc_info, nullptr, &out.memory) != VK_SUCCESS) {
            fns_.DestroyImage(device_, out.image, nullptr);
            out = {};
            return false;
        }
        if (fns_.BindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) {
            fns_.DestroyImage(device_, out.image, nullptr);
            fns_.FreeMemory(device_, out.memory, nullptr);
            out = {};
            return false;
        }
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = out.image;
        view_info.viewType = view_type;
        view_info.format = format;
        view_info.components = components;
        view_info.subresourceRange = {aspect, 0, 1, 0, array_layers};
        if (fns_.CreateImageView(device_, &view_info, nullptr, &out.view) != VK_SUCCESS) {
            fns_.DestroyImage(device_, out.image, nullptr);
            fns_.FreeMemory(device_, out.memory, nullptr);
            out = {};
            return false;
        }
        out.width = width;
        out.height = height;
        out.depth = depth;
        out.array_layers = array_layers;
        return true;
    }

    bool CreatePlaceholderImage(VkImageViewType view_type, OwnedImage& out) {
        uint32_t array_layers = 1;
        VkImageCreateFlags create_flags = 0;
        VkImageType image_type = VK_IMAGE_TYPE_2D;
        if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) {
            array_layers = 6;
            create_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        } else if (view_type == VK_IMAGE_VIEW_TYPE_3D) {
            image_type = VK_IMAGE_TYPE_3D;
        }
        return CreateOwnedImage(VK_FORMAT_R8G8B8A8_UNORM, kSyntheticTextureSize,
                                kSyntheticTextureSize,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, out, VK_SAMPLE_COUNT_1_BIT,
                                view_type, array_layers,
                                create_flags, image_type);
    }

    // M3 slice 3a: a synthetic, instantly recognizable texture bound to every
    // reflected set-2/set-3 image slot in place of the old 1x1 white
    // placeholder. If magenta/green checker cells show up in the readback the
    // sampling path (layout -> descriptor -> sampler -> shader fetch) is live;
    // real guest textures (360 tiling/swizzle) are a later slice.
    static constexpr uint32_t kSyntheticTextureSize = 256;
    static constexpr uint32_t kSyntheticTextureCell = 32;
    static constexpr VkDeviceSize kSyntheticTextureBytes =
        VkDeviceSize(kSyntheticTextureSize) * kSyntheticTextureSize * 4;

    // RGBA8 checkerboard: magenta (255,0,255) / green (0,255,0), opaque.
    static void FillSyntheticCheckerboard(uint8_t* dst) {
        for (uint32_t y = 0; y < kSyntheticTextureSize; ++y) {
            for (uint32_t x = 0; x < kSyntheticTextureSize; ++x) {
                const bool odd =
                    ((x / kSyntheticTextureCell) + (y / kSyntheticTextureCell)) & 1u;
                uint8_t* px = dst + (size_t(y) * kSyntheticTextureSize + x) * 4;
                px[0] = odd ? 0 : 255;    // R
                px[1] = odd ? 255 : 0;    // G
                px[2] = odd ? 0 : 255;    // B
                px[3] = 255;              // A
            }
        }
    }

    // M3 slice 3b/3c: upload the base level of common 2D formats directly from
    // the captured Xbox 360 physical-memory snapshot. Texels/blocks are
    // untiled on the CPU for this proof; the shipping path can move the same
    // address transform to a compute shader once parity is locked.
    bool CreateGuestTexture2D(const DrawSnapshot& snap,
                              const rex::graphics::RegisterFile& regs,
                              const DrawSnapshotTextureBinding& binding,
                              EphemeralCleanup& cleanup, GuestTextureUpload& out,
                              const char*& reason) {
        using namespace rex::graphics;
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(binding.fetch_constant);
        // TextureInfo::SetupMemoryInfo asserts when a positive minimum mip
        // names two different non-zero base/mip allocations. The base level
        // is irrelevant in that case: this path exposes only the first
        // requested guest mip as host level zero, so parse the descriptor with
        // the unused base allocation removed and source the upload from the
        // mip allocation below.
        xenos::xe_gpu_texture_fetch_t prepared_fetch = fetch;
        if (prepared_fetch.mip_min_level && prepared_fetch.base_address &&
            prepared_fetch.mip_address &&
            prepared_fetch.base_address != prepared_fetch.mip_address) {
            prepared_fetch.base_address = 0;
        }
        TextureInfo info{};
        if (!TextureInfo::Prepare(prepared_fetch, &info)) {
            reason = "invalid_fetch";
            return false;
        }
        const bool is_plain_2d =
            info.dimension == xenos::DataDimension::k2DOrStacked &&
            !info.is_stacked &&
            binding.dimension == uint8_t(xenos::FetchOpDimension::k2D);
        const bool is_cube =
            info.dimension == xenos::DataDimension::kCube &&
            binding.dimension == uint8_t(xenos::FetchOpDimension::kCube);
        if (!is_plain_2d && !is_cube) {
            reason = "not_plain_2d";
            return false;
        }
        uint32_t bytes_per_unit = 0;
        uint32_t unit_width_log2 = 0;
        bool convert_depth_float = false;
        bool convert_depth_unorm = false;
        switch (info.format) {
            case xenos::TextureFormat::k_8_8_8_8:
                // The translated shader exposes separate unsigned and signed
                // descriptors for formats whose host bit interpretation
                // differs. Match VulkanTextureCache's format pair instead of
                // binding the UNORM image to both slots; bloom/composite
                // shaders use the SNORM slot and otherwise saturate the frame.
                out.format = binding.is_signed
                                 ? VK_FORMAT_R8G8B8A8_SNORM
                                 : VK_FORMAT_R8G8B8A8_UNORM;
                bytes_per_unit = 4;
                break;
            case xenos::TextureFormat::k_DXT1:
                out.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                bytes_per_unit = 8;
                unit_width_log2 = 2;
                break;
            case xenos::TextureFormat::k_DXT2_3:
                out.format = VK_FORMAT_BC2_UNORM_BLOCK;
                bytes_per_unit = 16;
                unit_width_log2 = 2;
                break;
            case xenos::TextureFormat::k_DXT4_5:
                out.format = VK_FORMAT_BC3_UNORM_BLOCK;
                bytes_per_unit = 16;
                unit_width_log2 = 2;
                break;
            case xenos::TextureFormat::k_24_8_FLOAT:
                out.format = VK_FORMAT_R32_SFLOAT;
                bytes_per_unit = 4;
                convert_depth_float = true;
                break;
            case xenos::TextureFormat::k_24_8:
                // Matches VulkanTextureCache's kLoadShaderIndexDepthUnorm:
                // depth occupies the high 24 bits and is exposed as R32 float.
                out.format = VK_FORMAT_R32_SFLOAT;
                bytes_per_unit = 4;
                convert_depth_unorm = true;
                break;
            case xenos::TextureFormat::k_16_16_FLOAT:
                out.format = VK_FORMAT_R16G16_SFLOAT;
                bytes_per_unit = 4;
                break;
            case xenos::TextureFormat::k_16_16_16_16_FLOAT:
                out.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                bytes_per_unit = 8;
                break;
            case xenos::TextureFormat::k_32_FLOAT:
                out.format = VK_FORMAT_R32_SFLOAT;
                bytes_per_unit = 4;
                break;
            case xenos::TextureFormat::k_32_32_FLOAT:
                out.format = VK_FORMAT_R32G32_SFLOAT;
                bytes_per_unit = 8;
                break;
            case xenos::TextureFormat::k_32_32_32_32_FLOAT:
                out.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                bytes_per_unit = 16;
                break;
            default:
                reason = "format_not_in_slice3c";
                return false;
        }
        const uint32_t upload_mip = info.mip_min_level;
        if (upload_mip && info.has_packed_mips) {
            reason = "packed_min_mip_not_in_slice3c";
            return false;
        }
        uint32_t width = 0;
        uint32_t height = 0;
        info.GetMipSize(upload_mip, &width, &height);
        const uint32_t unit_mask = (1u << unit_width_log2) - 1u;
        const uint32_t width_units = (width + unit_mask) >> unit_width_log2;
        const uint32_t height_units = (height + unit_mask) >> unit_width_log2;
        const uint32_t array_layers = is_cube ? 6u : 1u;
        const texture_util::TextureGuestLayout guest_layout =
            texture_util::GetGuestTextureLayout(
                info.dimension, info.pitch >> 5, info.width + 1,
                info.height + 1,
                info.depth + 1, info.is_tiled, info.format,
                info.has_packed_mips, prepared_fetch.base_address != 0,
                info.mip_max_level);
        const texture_util::TextureGuestLayout::Level& source_level =
            upload_mip ? guest_layout.mips[upload_mip] : guest_layout.base;
        const uint32_t pitch_units =
            source_level.row_pitch_bytes / bytes_per_unit;
        if (!width_units || !height_units || pitch_units < width_units) {
            reason = "empty_texture";
            return false;
        }
        const VkDeviceSize staging_size =
            VkDeviceSize(width_units) * height_units * bytes_per_unit *
            array_layers;
        uint32_t source_base = upload_mip ? info.memory.mip_address
                                          : info.memory.base_address;
        if (upload_mip) {
            source_base += guest_layout.mip_offsets_bytes[upload_mip];
        }
        const uint32_t source_layer_stride =
            source_level.array_slice_stride_bytes;
        const uint32_t bytes_per_unit_log2 =
            bytes_per_unit == 16 ? 4u : bytes_per_unit == 8 ? 3u : 2u;
        const bool apply_diagnostic_offset =
            REXCVAR_GET(ac6_native_texture_offset_fetch) ==
            int32_t(binding.fetch_constant);
        const int32_t diagnostic_offset_x =
            apply_diagnostic_offset
                ? std::max(REXCVAR_GET(ac6_native_texture_offset_x), 0)
                : 0;
        const int32_t diagnostic_offset_y =
            apply_diagnostic_offset
                ? std::max(REXCVAR_GET(ac6_native_texture_offset_y), 0)
                : 0;
        const bool zero_diagnostic =
            REXCVAR_GET(ac6_native_zero_texture_fetch) ==
                int32_t(binding.fetch_constant) ||
            REXCVAR_GET(ac6_native_zero_texture_fetch_2) ==
                int32_t(binding.fetch_constant);
        const bool override_diagnostic =
            info.format == xenos::TextureFormat::k_32_FLOAT &&
            REXCVAR_GET(ac6_native_texture_override_fetch) ==
                int32_t(binding.fetch_constant);

        // Hash the guest's native tiled allocation before untile/upload. Most
        // AC6 textures are referenced by many consecutive draws without
        // changing; a sequential XXH3 pass is far cheaper than repeating the
        // tiled address transform, Vulkan allocation and transfer every time.
        // Diagnostic mutations intentionally bypass the persistent cache.
        const VkDeviceSize source_span =
            VkDeviceSize(source_layer_stride) * array_layers;
        const bool cacheable = !apply_diagnostic_offset && !zero_diagnostic &&
                               !override_diagnostic && source_span &&
                               source_base < snap.physical_size &&
                               source_span <= snap.physical_size - source_base;
        if (cacheable) {
            out.cache_key.assign(reinterpret_cast<const char*>(&fetch),
                                 sizeof(fetch));
            out.cache_key.push_back(char(binding.dimension));
            const bool signed_view_is_distinct =
                info.format == xenos::TextureFormat::k_8_8_8_8;
            out.cache_key.push_back(
                binding.is_signed && signed_view_is_distinct ? char(1)
                                                             : char(0));
            const uint64_t allocation_key =
                (uint64_t(source_base) << 32) | uint32_t(source_span);
            const auto memo_it =
                guest_texture_allocation_hashes_.find(allocation_key);
            if (memo_it != guest_texture_allocation_hashes_.end()) {
                out.content_hash = memo_it->second;
                ++guest_texture_hash_hits_;
            } else {
                out.content_hash = XXH3_64bits(
                    snap.physical_membase + source_base,
                    size_t(source_span));
                guest_texture_allocation_hashes_.emplace(
                    allocation_key, out.content_hash);
                ++guest_texture_hash_misses_;
                guest_texture_hash_bytes_ += uint64_t(source_span);
            }
            const auto cached_it = guest_texture_cache_.find(out.cache_key);
            if (cached_it != guest_texture_cache_.end() &&
                cached_it->second.format == out.format &&
                cached_it->second.content_hash == out.content_hash &&
                cached_it->second.image.image) {
                cached_it->second.last_used_serial =
                    ++guest_texture_cache_use_serial_;
                out.image = cached_it->second.image;
                out.fetch_constant = binding.fetch_constant;
                out.staging_size = 0;
                out.needs_upload = false;
                out.cacheable = true;
                reason = nullptr;
                return true;
            }
            out.cacheable = true;
        }

        if (!CreateHostBuffer(staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              out.staging)) {
            reason = "staging_create_failed";
            return false;
        }
        cleanup.buffers.push_back(out.staging);
        uint8_t* dst = static_cast<uint8_t*>(out.staging.mapped);

        auto endian_word = [&](uint32_t value) {
            switch (info.endianness) {
                case xenos::Endian::k8in16:
                    return ((value & 0xFF00FF00u) >> 8) |
                           ((value & 0x00FF00FFu) << 8);
                case xenos::Endian::k8in32:
                    return __builtin_bswap32(value);
                case xenos::Endian::k16in32:
                    return (value >> 16) | (value << 16);
                default:
                    return value;
            }
        };
        const std::vector<uint32_t>* tiled_offsets = nullptr;
        if (info.is_tiled && !diagnostic_offset_x &&
            !diagnostic_offset_y) {
            const std::array<uint32_t, 4> offset_key = {
                width_units, height_units, pitch_units,
                bytes_per_unit_log2};
            auto [offset_it, inserted] =
                texture_tiled_offset_cache_.try_emplace(offset_key);
            if (inserted) {
                std::vector<uint32_t>& offsets = offset_it->second;
                offsets.resize(size_t(width_units) * height_units);
                for (uint32_t y = 0; y < height_units; ++y) {
                    for (uint32_t x = 0; x < width_units; ++x) {
                        offsets[size_t(y) * width_units + x] = uint32_t(
                            texture_util::GetTiledOffset2D(
                                int32_t(x), int32_t(y), pitch_units,
                                bytes_per_unit_log2));
                    }
                }
            }
            tiled_offsets = &offset_it->second;
        }
        for (uint32_t layer = 0; layer < array_layers; ++layer) {
          for (uint32_t y = 0; y < height_units; ++y) {
            for (uint32_t x = 0; x < width_units; ++x) {
                uint32_t source_offset;
                if (info.is_tiled) {
                    source_offset = tiled_offsets
                                        ? (*tiled_offsets)[size_t(y) *
                                                               width_units +
                                                           x]
                                        : uint32_t(
                                              texture_util::GetTiledOffset2D(
                                                  int32_t(x) +
                                                      diagnostic_offset_x,
                                                  int32_t(y) +
                                                      diagnostic_offset_y,
                                                  pitch_units,
                                                  bytes_per_unit_log2));
                } else {
                    source_offset =
                        ((y + uint32_t(diagnostic_offset_y)) * pitch_units +
                         x + uint32_t(diagnostic_offset_x)) *
                        bytes_per_unit;
                }
                source_offset += layer * source_layer_stride;
                if (source_base >= snap.physical_size ||
                    source_offset > snap.physical_size - source_base ||
                    bytes_per_unit > snap.physical_size - source_base - source_offset) {
                    reason = "guest_texture_out_of_bounds";
                    return false;
                }
                const uint8_t* src =
                    snap.physical_membase + source_base + source_offset;
                uint8_t* unit_dst =
                    dst + ((size_t(layer) * height_units + y) * width_units + x) *
                              bytes_per_unit;
                for (uint32_t byte = 0; byte < bytes_per_unit; byte += 4) {
                    uint32_t word;
                    std::memcpy(&word, src + byte, sizeof(word));
                    word = endian_word(word);
                    if (convert_depth_float) {
                        const float depth = xenos::Float20e4To32(word >> 8);
                        std::memcpy(unit_dst + byte, &depth, sizeof(depth));
                    } else if (convert_depth_unorm) {
                        constexpr float kDepth24Scale =
                            1.0f / 16777215.0f;
                        const float depth = float(word >> 8) * kDepth24Scale;
                        std::memcpy(unit_dst + byte, &depth, sizeof(depth));
                    } else {
                        std::memcpy(unit_dst + byte, &word, sizeof(word));
                    }
                }
            }
          }
        }
        if (zero_diagnostic) {
            std::memset(dst, 0, size_t(staging_size));
        }
        if (override_diagnostic) {
            const float replacement =
                float(REXCVAR_GET(ac6_native_texture_override_millionths)) /
                1000000.0f;
            for (VkDeviceSize offset = 0; offset < staging_size;
                 offset += sizeof(replacement)) {
                std::memcpy(dst + offset, &replacement, sizeof(replacement));
            }
        }
        if (!binding.is_signed && out.format == VK_FORMAT_R8G8B8A8_UNORM) {
            if (const char* dump_dir = std::getenv("AC6_NR2_DUMP_TEXTURE_DIR")) {
                const std::string dump_path =
                    std::string(dump_dir) + "/capture-" +
                    std::to_string(snap.capture_index) + "-fetch-" +
                    std::to_string(binding.fetch_constant) + ".ppm";
                WritePpm(dump_path, dst, width, height, out.format, 4);
            }
        }
        const VkImageViewType view_type =
            is_cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        const VkImageCreateFlags create_flags =
            is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

        // M1 shaders were converted with Features(true), including
        // image_view_format_swizzle. They therefore expect the reference
        // texture cache's GuestToHostSwizzle result in VkImageView::components
        // and intentionally do not apply sys.texture_swizzles in shader code.
        // Supplying identity views here made the common AC6 BGRA fetch (0x60a)
        // sample R/B backwards even though the constants were populated.
        uint32_t host_format_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        switch (GetBaseFormat(info.format)) {
            case xenos::TextureFormat::k_24_8:
            case xenos::TextureFormat::k_24_8_FLOAT:
            case xenos::TextureFormat::k_32_FLOAT:
                host_format_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
                break;
            case xenos::TextureFormat::k_16_16_FLOAT:
            case xenos::TextureFormat::k_32_32_FLOAT:
                host_format_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
                break;
            default:
                break;
        }
        VkComponentSwizzle component_swizzles[4] = {};
        static constexpr VkComponentSwizzle kVkComponentSwizzles[6] = {
            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
            VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
        };
        for (uint32_t component = 0; component < 4; ++component) {
            const uint32_t guest_selector =
                (fetch.swizzle >> (3 * component)) & 7u;
            const uint32_t host_selector =
                guest_selector >= xenos::XE_GPU_TEXTURE_SWIZZLE_0
                    ? (guest_selector & 5u)
                    : ((host_format_swizzle >> (3 * guest_selector)) & 7u);
            component_swizzles[component] =
                kVkComponentSwizzles[std::min(host_selector, 5u)];
        }
        VkComponentMapping components{};
        components.r = component_swizzles[0];
        components.g = component_swizzles[1];
        components.b = component_swizzles[2];
        components.a = component_swizzles[3];
        if (!CreateOwnedImage(
                out.format, width, height,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, out.image, VK_SAMPLE_COUNT_1_BIT,
                view_type, array_layers, create_flags, VK_IMAGE_TYPE_2D, 1,
                components)) {
            reason = "guest_image_create_failed";
            return false;
        }
        cleanup.images.push_back(out.image);
        out.fetch_constant = binding.fetch_constant;
        out.staging_size = staging_size;
        reason = nullptr;
        return true;
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                      VkImageLayout old_layout, VkImageLayout new_layout,
                      VkAccessFlags src_access, VkAccessFlags dst_access,
                      VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                    VK_REMAINING_ARRAY_LAYERS};
        fns_.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
    }

    static uint32_t FormatBytesPerPixel(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_R16G16_SNORM:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
                return 4;
            case VK_FORMAT_R16G16B16A16_UNORM:
            case VK_FORMAT_R16G16B16A16_SNORM:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
                return 8;
            default:
                return 0;
        }
    }

    static void WritePpm(const std::string& path, const uint8_t* pixels, uint32_t width,
                         uint32_t height, VkFormat format, uint32_t bytes_per_pixel) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            REXLOG_CAT_ERROR(kLogGPU, "nr2: cannot open '{}' for the native draw PPM", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", width, height);
        std::vector<uint8_t> row(size_t(width) * 3);
        auto half_to_u8 = [](uint16_t h) -> uint8_t {
            const uint32_t exponent = (h >> 10) & 0x1F;
            const uint32_t mantissa = h & 0x3FF;
            if ((h & 0x8000) || !exponent) {
                return 0;  // negative/denormal/zero -> 0
            }
            float value = std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15);
            value = std::min(std::max(value, 0.0f), 1.0f);
            return uint8_t(value * 255.0f + 0.5f);
        };
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src = pixels + size_t(y) * width * bytes_per_pixel;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                const uint8_t* px = src + size_t(x) * bytes_per_pixel;
                switch (format) {
                    case VK_FORMAT_R8G8B8A8_UNORM:
                        r = px[0];
                        g = px[1];
                        b = px[2];
                        break;
                    case VK_FORMAT_R16G16B16A16_UNORM:
                    case VK_FORMAT_R16G16B16A16_SNORM:
                        r = px[1];
                        g = px[3];
                        b = px[5];
                        break;
                    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                        uint32_t v;
                        std::memcpy(&v, px, 4);
                        r = uint8_t((v & 0x3FF) >> 2);
                        g = uint8_t(((v >> 10) & 0x3FF) >> 2);
                        b = uint8_t(((v >> 20) & 0x3FF) >> 2);
                        break;
                    }
                    case VK_FORMAT_R16G16B16A16_SFLOAT: {
                        uint16_t h[3];
                        std::memcpy(h, px, 6);
                        r = half_to_u8(h[0]);
                        g = half_to_u8(h[1]);
                        b = half_to_u8(h[2]);
                        break;
                    }
                    default:
                        r = g = b = px[0];
                        break;
                }
                row[size_t(x) * 3 + 0] = r;
                row[size_t(x) * 3 + 1] = g;
                row[size_t(x) * 3 + 2] = b;
            }
            std::fwrite(row.data(), row.size(), 1, f);
        }
        std::fclose(f);
        REXLOG_CAT_INFO(kLogGPU, "nr2: wrote native draw output to {}", path);
    }

    // 512MB guest physical memory as the set-0 SSBO. Zero-copy import via
    // VK_EXT_external_memory_host is preferred. The copy fallback starts
    // with one full snapshot. In a persistent offline trace session, later
    // draws refresh only the guest index buffer and the vertex-fetch ranges
    // declared by the translated VS. This is sufficient for the trace proof
    // path and avoids copying 512MB for every draw. Live capture retains the
    // conservative full refresh until it has emulator dirty-page tracking.
    bool EnsureGuestMemoryBuffer(
        const DrawSnapshot& snap,
        const rex::graphics::RegisterFile& regs) {
        if (guest_memory_buffer_ != VK_NULL_HANDLE) {
            if (!guest_memory_imported_) {
                if (!guest_memory_copy_mapped_ ||
                    guest_memory_size_ != VkDeviceSize(snap.physical_size)) {
                    return false;
                }
                const bool bounded_trace_refresh =
                    persistent_targets_enabled_ && snap.tool_relaxed;
                if (!bounded_trace_refresh) {
                    std::memcpy(guest_memory_copy_mapped_,
                                snap.physical_membase,
                                size_t(guest_memory_size_));
                    return true;
                }

                // Collect half-open byte ranges, clamp them to physical
                // memory, then coalesce overlaps before copying.
                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                auto add_range = [&](uint64_t base, uint64_t length) {
                    if (!length || base >= snap.physical_size) {
                        return;
                    }
                    const uint64_t end = std::min<uint64_t>(
                        uint64_t(snap.physical_size), base + length);
                    if (end > base) {
                        ranges.emplace_back(uint32_t(base), uint32_t(end));
                    }
                };

                for (uint32_t word = 0;
                     word < sizeof(snap.vs.vertex_fetch_bitmap) /
                                sizeof(snap.vs.vertex_fetch_bitmap[0]);
                     ++word) {
                    const uint32_t used = snap.vs.vertex_fetch_bitmap[word];
                    for (uint32_t bit = 0; bit < 32; ++bit) {
                        if (!(used & (uint32_t(1) << bit))) {
                            continue;
                        }
                        const auto fetch = regs.GetVertexFetch(word * 32 + bit);
                        add_range(uint64_t(fetch.address) << 2,
                                  uint64_t(fetch.size) << 2);
                    }
                }

                const auto draw = regs.Get<rex::graphics::reg::VGT_DRAW_INITIATOR>();
                if (draw.source_select == rex::graphics::xenos::SourceSelect::kDMA &&
                    draw.num_indices) {
                    const uint32_t index_size_log2 =
                        draw.index_size == rex::graphics::xenos::IndexFormat::kInt32
                            ? 2
                            : 1;
                    const uint32_t base =
                        (regs[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] &
                         0x1FFFFFFFu) &
                        ~((1u << index_size_log2) - 1);
                    add_range(base,
                              uint64_t(draw.num_indices) << index_size_log2);
                }

                std::sort(ranges.begin(), ranges.end());
                size_t merged_count = 0;
                uint64_t copied_bytes = 0;
                for (const auto& range : ranges) {
                    if (merged_count &&
                        range.first <= ranges[merged_count - 1].second) {
                        ranges[merged_count - 1].second = std::max(
                            ranges[merged_count - 1].second, range.second);
                    } else {
                        ranges[merged_count++] = range;
                    }
                }
                auto* dst = static_cast<uint8_t*>(guest_memory_copy_mapped_);
                for (size_t i = 0; i < merged_count; ++i) {
                    const auto& range = ranges[i];
                    const size_t length = size_t(range.second - range.first);
                    std::memcpy(dst + range.first,
                                snap.physical_membase + range.first, length);
                    copied_bytes += length;
                }
                ++guest_memory_bounded_refresh_count_;
                guest_memory_bounded_refresh_bytes_ += copied_bytes;
                if (guest_memory_bounded_refresh_count_ == 1 ||
                    !(guest_memory_bounded_refresh_count_ % 100)) {
                    REXLOG_CAT_INFO(
                        kLogGPU,
                        "nr2: bounded guest-memory refresh #{}: {} ranges, {} KiB "
                        "this draw ({} MiB cumulative)",
                        guest_memory_bounded_refresh_count_, merged_count,
                        copied_bytes >> 10,
                        guest_memory_bounded_refresh_bytes_ >> 20);
                }
            }
            return true;
        }
        const VkDeviceSize size = VkDeviceSize(snap.physical_size);
        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        const bool alignment_ok =
            (reinterpret_cast<uintptr_t>(snap.physical_membase) %
             min_imported_host_pointer_alignment_) == 0 &&
            (size % min_imported_host_pointer_alignment_) == 0;
        static const bool force_guest_memory_copy = [] {
            const char* value =
                std::getenv("AC6_NR2_FORCE_GUEST_MEMORY_COPY");
            return value && value[0] && std::strcmp(value, "0") != 0;
        }();
        if (external_memory_host_supported_ && alignment_ok &&
            !force_guest_memory_copy) {
            VkExternalMemoryBufferCreateInfo external_info = {};
            external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            external_info.handleTypes =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo buffer_info = {};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.pNext = &external_info;
            buffer_info.size = size;
            buffer_info.usage = usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer buffer = VK_NULL_HANDLE;
            if (fns_.CreateBuffer(device_, &buffer_info, nullptr, &buffer) == VK_SUCCESS) {
                VkMemoryHostPointerPropertiesEXT host_props = {};
                host_props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
                VkMemoryRequirements requirements;
                fns_.GetBufferMemoryRequirements(device_, buffer, &requirements);
                VkResult host_result = fns_.GetMemoryHostPointerPropertiesEXT(
                    device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    const_cast<uint8_t*>(snap.physical_membase), &host_props);
                const uint32_t type_bits =
                    host_props.memoryTypeBits & requirements.memoryTypeBits;
                uint32_t type = UINT32_MAX;
                if (host_result == VK_SUCCESS && type_bits && requirements.size <= size) {
                    type = FindMemoryType(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    if (type == UINT32_MAX) {
                        type = FindMemoryType(type_bits, 0);
                    }
                }
                if (type != UINT32_MAX) {
                    VkImportMemoryHostPointerInfoEXT import_info = {};
                    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
                    import_info.handleType =
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
                    import_info.pHostPointer = const_cast<uint8_t*>(snap.physical_membase);
                    VkMemoryAllocateInfo alloc_info = {};
                    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    alloc_info.pNext = &import_info;
                    alloc_info.allocationSize = size;
                    alloc_info.memoryTypeIndex = type;
                    VkDeviceMemory memory = VK_NULL_HANDLE;
                    VkResult alloc_result =
                        fns_.AllocateMemory(device_, &alloc_info, nullptr, &memory);
                    if (alloc_result == VK_SUCCESS &&
                        fns_.BindBufferMemory(device_, buffer, memory, 0) == VK_SUCCESS) {
                        VkPhysicalDeviceMemoryProperties memory_properties;
                        fns_.GetPhysicalDeviceMemoryProperties(
                            physical_device_, &memory_properties);
                        guest_memory_buffer_ = buffer;
                        guest_memory_memory_ = memory;
                        guest_memory_imported_ = true;
                        guest_memory_import_coherent_ =
                            (memory_properties.memoryTypes[type].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                        guest_memory_size_ = size;
                        REXLOG_CAT_INFO(
                            kLogGPU,
                            "nr2: guest memory SSBO: ZERO-COPY import of {} MB at {} via "
                            "VK_EXT_external_memory_host (memory type {}, coherent={})",
                            uint64_t(size) >> 20,
                            static_cast<const void*>(snap.physical_membase), type,
                            guest_memory_import_coherent_);
                        return true;
                    }
                    if (memory != VK_NULL_HANDLE) {
                        fns_.FreeMemory(device_, memory, nullptr);
                    }
                    REXLOG_CAT_WARN(kLogGPU,
                                    "nr2: external_memory_host import failed ({}) - falling "
                                    "back to a copy snapshot",
                                    int32_t(alloc_result));
                } else {
                    REXLOG_CAT_WARN(kLogGPU,
                                    "nr2: external_memory_host unusable (host props {}, type "
                                    "bits 0x{:08x}) - falling back to a copy snapshot",
                                    int32_t(host_result), type_bits);
                }
                fns_.DestroyBuffer(device_, buffer, nullptr);
            }
        } else {
            REXLOG_CAT_INFO(kLogGPU,
                            "nr2: external_memory_host {} (alignment_ok={}, "
                            "force_copy={}) - using a copy snapshot for the "
                            "shared-memory SSBO",
                            external_memory_host_supported_ ? "available" : "unsupported",
                            alignment_ok, force_guest_memory_copy);
        }
        // Fallback: one-time full copy into a host-visible buffer.
        HostBuffer copy_buffer{};
        if (!CreateHostBuffer(size, usage, copy_buffer)) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: guest memory copy buffer ({} MB) creation failed",
                             uint64_t(size) >> 20);
            return false;
        }
        std::memcpy(copy_buffer.mapped, snap.physical_membase, size_t(size));
        guest_memory_buffer_ = copy_buffer.buffer;
        guest_memory_memory_ = copy_buffer.memory;
        guest_memory_imported_ = false;
        guest_memory_import_coherent_ = false;
        guest_memory_copy_mapped_ = copy_buffer.mapped;
        guest_memory_size_ = size;
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: guest memory SSBO: refreshed COPY snapshot of {} MB "
                        "(external-memory-host path unavailable)",
                        uint64_t(size) >> 20);
        return true;
    }

    void DestroyGuestMemoryBuffer() {
        if (guest_memory_copy_mapped_ != nullptr &&
            guest_memory_memory_ != VK_NULL_HANDLE) {
            fns_.UnmapMemory(device_, guest_memory_memory_);
            guest_memory_copy_mapped_ = nullptr;
        }
        if (guest_memory_buffer_ != VK_NULL_HANDLE) {
            fns_.DestroyBuffer(device_, guest_memory_buffer_, nullptr);
            guest_memory_buffer_ = VK_NULL_HANDLE;
        }
        if (guest_memory_memory_ != VK_NULL_HANDLE) {
            fns_.FreeMemory(device_, guest_memory_memory_, nullptr);
            guest_memory_memory_ = VK_NULL_HANDLE;
        }
        guest_memory_size_ = 0;
        guest_memory_imported_ = false;
        guest_memory_import_coherent_ = false;
        guest_memory_bounded_refresh_count_ = 0;
        guest_memory_bounded_refresh_bytes_ = 0;
    }

    std::map<std::string, size_t> reject_counts_;
    size_t reject_total_ = 0;
    // Topology variants for CPU-converted draws, keyed by the base stored
    // description hash.
    std::map<std::pair<uint64_t, uint32_t>, NativePipeline> variant_pipelines_;

    // Non-owning and non-null only in pipeline-cache-only shared-device mode.
    rex::ui::vulkan::VulkanDevice* shared_vulkan_device_ = nullptr;
    bool shared_msaa_2x_attachments_supported_ = false;
    bool shared_fragment_shading_rate_2x2_sample_2_enabled_ = false;
    bool shared_pipeline_plan_cache_enabled_ = false;
    SharedPipelinePlanCacheEntry shared_pipeline_plan_cache_entry_;
    SharedPipelinePlanCacheStats shared_pipeline_plan_cache_stats_;
    uint64_t water_vrs_pipeline_count_ = 0;
    uint64_t shared_water_vrs_selection_count_ = 0;
    uint64_t private_water_vrs_draw_count_ = 0;
    bool water_vrs_fallback_reported_ = false;
#if defined(VK_KHR_fragment_shading_rate)
    // The private trace path owns its logical device, so it independently
    // tracks extension advertisement, rate enumeration, and feature enablement.
    bool private_fragment_shading_rate_extension_supported_ = false;
    bool private_fragment_shading_rate_properties_2x2_sample_2_supported_ =
        false;
    bool private_fragment_shading_rate_2x2_sample_2_supported_ = false;
    bool private_fragment_shading_rate_2x2_sample_2_enabled_ = false;
#endif

    VulkanFns fns_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
    bool queue_supports_compute_ = false;
    VkSampleCountFlags framebuffer_color_sample_counts_ = 0;
    VkSampleCountFlags framebuffer_depth_sample_counts_ = 0;
    bool depth_clamp_enabled_ = false;
    bool fill_mode_non_solid_enabled_ = false;
    bool depth_d24s8_supported_ = false;
    bool rg16_snorm_supported_ = false;
    bool rgba16_snorm_supported_ = false;
    bool gamma_as_unorm16_ = false;
    size_t shader_modules_ok_ = 0;
    size_t shader_modules_fail_ = 0;
    size_t pipelines_gs_omitted_ = 0;
    uint32_t max_viewport_dimensions_[2] = {4096, 4096};
    VkDeviceSize min_uniform_buffer_offset_alignment_ = 256;
    bool external_memory_host_supported_ = false;
    VkDeviceSize min_imported_host_pointer_alignment_ = 4096;
    // M3 slice 2: 512MB guest-physical-memory buffer bound as the set-0
    // shared-memory SSBO (imported zero-copy, or a one-time copy fallback).
    VkBuffer guest_memory_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory_memory_ = VK_NULL_HANDLE;
    bool guest_memory_imported_ = false;
    bool guest_memory_import_coherent_ = false;
    void* guest_memory_copy_mapped_ = nullptr;
    VkDeviceSize guest_memory_size_ = 0;
    uint64_t guest_memory_bounded_refresh_count_ = 0;
    uint64_t guest_memory_bounded_refresh_bytes_ = 0;
    HostBuffer resolve_readback_buffer_{};
    HostBuffer resolve_overlay_readback_buffer_{};
    HostBuffer gpu_resolve_shadow_buffer_{};
    VkCommandPool resolve_command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer resolve_command_buffer_ = VK_NULL_HANDLE;
    VkFence resolve_fence_ = VK_NULL_HANDLE;
    bool resolve_submission_used_ = false;
    bool resolve_submission_in_flight_ = false;
    std::vector<uint32_t> gpu_resolve_spirv_;
    VkDescriptorSetLayout gpu_resolve_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gpu_resolve_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gpu_resolve_pipeline_ = VK_NULL_HANDLE;
    VkSampler gpu_resolve_sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool gpu_resolve_descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet gpu_resolve_descriptor_set_ = VK_NULL_HANDLE;
    uint64_t gpu_resolve_count_ = 0;
    std::vector<uint8_t> resolve_cpu_readback_;
    std::vector<uint8_t> resolve_cpu_overlay_readback_;
    bool resolve_dump_once_completed_ = false;
    std::unordered_map<uint64_t, std::vector<uint32_t>>
        resolve_tiled_offset_cache_;
    std::map<std::array<uint32_t, 7>, std::vector<uint32_t>>
        resolve_secondary_offset_cache_;
    std::map<std::array<uint32_t, 4>, std::vector<uint32_t>>
        texture_tiled_offset_cache_;
    std::unordered_map<std::string, CachedGuestTexture>
        guest_texture_cache_;
    // Content hashes are shared by signed/unsigned descriptors and repeated
    // draws until the next GPU copy. The key packs the physical base and the
    // complete allocation span, both already bounds-checked above.
    std::unordered_map<uint64_t, uint64_t>
        guest_texture_allocation_hashes_;
    std::unordered_map<uint32_t, VkSampler> guest_sampler_cache_;
    uint64_t guest_texture_cache_use_serial_ = 0;
    uint64_t guest_texture_hash_hits_ = 0;
    uint64_t guest_texture_hash_misses_ = 0;
    uint64_t guest_texture_hash_bytes_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> resolve_pending_writes_;
    bool session_failed_ = false;
    bool persistent_targets_enabled_ = false;
    bool defer_draw_waits_ = false;
    VkCommandPool deferred_command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool deferred_descriptor_pool_ = VK_NULL_HANDLE;
    HostBuffer deferred_uniform_buffer_{};
    VkDeviceSize deferred_uniform_offset_ = 0;
    uint64_t deferred_batch_count_ = 0;
    uint64_t deferred_batch_draw_count_ = 0;
    VkCommandBuffer deferred_batch_command_buffer_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<EphemeralCleanup>> deferred_draw_resources_;
    std::set<std::string> reported_texture_fallbacks_;
    uint64_t persistent_target_use_serial_ = 0;
    uint64_t unsafe_resolve_reject_count_ = 0;
    std::vector<PersistentTarget> persistent_targets_;
    // Authoritative manifest filename -> (byte size, XXH3). Private mode
    // creates modules only from this exact set. Shared authority never fills
    // or consults it because it borrows the live SDK modules instead.
    std::map<std::string, std::pair<size_t, uint64_t>>
        manifest_shader_files_;
    std::unordered_map<std::string, LoadedShader> shaders_;         // by .spv filename
    std::unordered_map<std::string, VkDescriptorSetLayout> set_layouts_;
    std::unordered_map<std::string, VkPipelineLayout> pipeline_layouts_;
    std::unordered_map<uint64_t, StoredPipeline> stored_pipelines_; // parsed .xpso records
    std::unordered_map<uint64_t, NativePipeline> pipelines_;        // by description hash
    // Shared-authority failures are permanent for the lifetime of the
    // borrowed device and pipeline store. Remember them so an unsupported
    // draw does not retry pipeline creation or emit a warning every frame.
    std::set<uint64_t> shared_pipeline_rejections_;
};

std::mutex g_live_gpu_session_mutex;
std::unique_ptr<PipelineProver> g_live_gpu_session;
uint64_t g_live_gpu_capture_index = 0;

// Shared-authority pipelines borrow the emulator Vulkan device and must be
// destroyed by ShutdownAuthoritativeVulkanDevice after the queue is drained,
// before the SDK destroys its layouts and device. Keep this lifetime separate
// from the older private renderer session so no private execution API can be
// called on a borrowed-device prover.
std::mutex g_shared_gpu_session_mutex;
// Intentionally raw: if an abnormal shutdown skips the ordered Vulkan-device
// hook, leaking these process-lifetime objects is safer than a static
// unique_ptr destructor issuing vkDestroy* calls through an invalid device.
// The normal hook explicitly destroys and deletes the prover.
PipelineProver* g_shared_gpu_session = nullptr;
rex::ui::vulkan::VulkanDevice* g_shared_vulkan_device = nullptr;
VkDevice g_shared_device = VK_NULL_HANDLE;
VkDevice g_shared_failed_device = VK_NULL_HANDLE;
uint64_t g_shared_prepare_count = 0;
uint64_t g_shared_accept_count = 0;
uint64_t g_shared_reject_count = 0;

bool BuildExactGpuSnapshot(
    rex::graphics::CommandProcessor* command_processor,
    rex::graphics::RegisterFile* register_file,
    rex::memory::Memory* memory,
    uint32_t primitive_type,
    uint32_t index_count,
    DrawSnapshot& out) {
    if (!command_processor || !register_file || !memory) {
        return false;
    }
    out.capture_index = ++g_live_gpu_capture_index;
    out.record.primitive_type = primitive_type;
    out.record.count = index_count;
    out.regs.assign(register_file->values,
                    register_file->values +
                        rex::graphics::RegisterFile::kRegisterCount);
    auto copy_shader = [](rex::graphics::Shader* shader,
                          DrawSnapshotShader& dst) {
        if (!shader) {
            return;
        }
        dst.valid = true;
        dst.ucode_hash = shader->ucode_data_hash();
        dst.ucode_analyzed = shader->is_ucode_analyzed();
        if (!dst.ucode_analyzed) {
            return;
        }
        const rex::graphics::Shader::ConstantRegisterMap& map =
            shader->constant_register_map();
        dst.float_count = map.float_count;
        dst.float_dynamic_addressing = map.float_dynamic_addressing;
        static_assert(sizeof(dst.float_bitmap) == sizeof(map.float_bitmap));
        std::memcpy(dst.float_bitmap, map.float_bitmap,
                    sizeof(dst.float_bitmap));
        static_assert(sizeof(dst.vertex_fetch_bitmap) ==
                      sizeof(map.vertex_fetch_bitmap));
        std::memcpy(dst.vertex_fetch_bitmap, map.vertex_fetch_bitmap,
                    sizeof(dst.vertex_fetch_bitmap));
        dst.writes_depth = shader->writes_depth();
        if (auto* spirv_shader =
                dynamic_cast<rex::graphics::SpirvShader*>(shader)) {
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
    };
    copy_shader(command_processor->active_vertex_shader(), out.vs);
    copy_shader(command_processor->active_pixel_shader(), out.ps);
    out.physical_membase = memory->physical_membase();
    out.physical_size = UINT32_C(1) << 29;
    // A continuous session must retain zero-coverage and giant-viewport
    // commands because they participate in later target contents.
    out.tool_relaxed = true;
    out.tool_suppress_output = true;
    return true;
}

void ConfigureGpuResolveShader(
    PipelineProver& prover,
    rex::graphics::CommandProcessor* command_processor) {
    if (!REXCVAR_GET(ac6_native_gpu_resolve)) {
        return;
    }
    auto* vulkan_command_processor = dynamic_cast<
        rex::graphics::vulkan::VulkanCommandProcessor*>(
        command_processor);
    std::vector<uint32_t> spirv;
    std::string compile_error;
    if (vulkan_command_processor &&
        vulkan_command_processor->CompileGlslToSpirv(
            VK_SHADER_STAGE_COMPUTE_BIT, kGpuResolveShaderSource, spirv,
            compile_error)) {
        prover.SetGpuResolveSpirv(std::move(spirv));
        return;
    }
    REXLOG_CAT_WARN(
        kLogGPU,
        "nr2: direct GPU resolve shader compilation failed{}{}; retaining "
        "CPU resolve path",
        compile_error.empty() ? "" : ": ", compile_error);
}

PipelineProver* EnsureLiveGpuSession(
    rex::graphics::CommandProcessor* command_processor) {
    if (!g_live_gpu_session) {
        g_live_gpu_session = std::make_unique<PipelineProver>();
        ConfigureGpuResolveShader(*g_live_gpu_session, command_processor);
        g_live_gpu_session->Run();
        g_live_gpu_session->SetPersistentTargetsEnabled(true);
        g_live_gpu_session->SetDeferredDrawWaits(true);
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: exact command-processor live session initialized");
    }
    return g_live_gpu_session.get();
}

// Builds a DrawSnapshot on the guest thread from the emulated GPU state
// (register file + active shaders + guest memory host pointer). The emulated
// command processor runs asynchronously, so the register/shader state
// corresponds to the draw it most recently processed - self-consistent, just
// not necessarily the triggering guest D3D call.
bool BuildDrawSnapshot(const ac6::d3d::DrawCallRecord& record, uint64_t capture_index,
                       DrawSnapshot& out) {
    auto* thread_state = rex::runtime::ThreadState::Get();
    if (!thread_state || !thread_state->context() || !thread_state->context()->kernel_state) {
        return false;
    }
    auto* kernel_state = thread_state->context()->kernel_state;
    auto* graphics =
        dynamic_cast<rex::graphics::GraphicsSystem*>(kernel_state->graphics_system());
    if (!graphics || !graphics->command_processor() || !graphics->register_file() ||
        !graphics->memory()) {
        return false;
    }
    out.capture_index = capture_index;
    out.record = record;
    const rex::graphics::RegisterFile* register_file = graphics->register_file();
    out.regs.assign(register_file->values,
                    register_file->values + rex::graphics::RegisterFile::kRegisterCount);
    auto copy_shader = [](rex::graphics::Shader* shader, DrawSnapshotShader& dst) {
        if (!shader) {
            return;
        }
        dst.valid = true;
        dst.ucode_hash = shader->ucode_data_hash();
        dst.ucode_analyzed = shader->is_ucode_analyzed();
        if (dst.ucode_analyzed) {
            const rex::graphics::Shader::ConstantRegisterMap& map =
                shader->constant_register_map();
            dst.float_count = map.float_count;
            dst.float_dynamic_addressing = map.float_dynamic_addressing;
            static_assert(sizeof(dst.float_bitmap) == sizeof(map.float_bitmap));
            std::memcpy(dst.float_bitmap, map.float_bitmap, sizeof(dst.float_bitmap));
            static_assert(sizeof(dst.vertex_fetch_bitmap) ==
                          sizeof(map.vertex_fetch_bitmap));
            std::memcpy(dst.vertex_fetch_bitmap, map.vertex_fetch_bitmap,
                        sizeof(dst.vertex_fetch_bitmap));
            dst.writes_depth = shader->writes_depth();
            if (auto* spirv_shader =
                    dynamic_cast<rex::graphics::SpirvShader*>(shader)) {
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
    };
    rex::graphics::CommandProcessor* command_processor = graphics->command_processor();
    copy_shader(command_processor->active_vertex_shader(), out.vs);
    copy_shader(command_processor->active_pixel_shader(), out.ps);
    out.physical_membase = graphics->memory()->physical_membase();
    out.physical_size = UINT32_C(1) << 29;  // SharedMemory::kBufferSize (512MB)
    return true;
}

void RenderThreadMain() {
    REXLOG_CAT_INFO(kLogGPU, "ac6_native_renderer2: render thread started");

    // The asynchronous one-shot capture path is normally dormant. Create its
    // private Vulkan context only if a captured draw actually reaches this
    // thread, so marker-enabled exact takeover does not retain a second device
    // and pipeline cache for the lifetime of the render thread.
    std::unique_ptr<PipelineProver> one_shot_prover;

    uint64_t present_ticks = 0;
    for (;;) {
        RenderCommand cmd;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] {
                return !g_queue.empty() || !g_running.load(std::memory_order_acquire);
            });
            if (g_queue.empty()) {
                break;  // shutdown requested and queue drained
            }
            cmd = g_queue.front();
            g_queue.pop_front();
        }
        switch (cmd.type) {
            case RenderCommandType::kPresent:
                ++present_ticks;
                if (REXCVAR_GET(ac6_native_verbose_execution_logs)) {
                    REXLOG_CAT_INFO(
                        kLogGPU,
                        "ac6_native_renderer2: native present tick {} "
                        "(guest swap {})",
                        present_ticks, cmd.sequence);
                }
                break;
            case RenderCommandType::kSetDebugMarker:
                REXLOG_CAT_INFO(kLogGPU, "ac6_native_renderer2: debug marker '{}'", cmd.marker);
                break;
            case RenderCommandType::kExecuteCapturedDraw: {
                if (REXCVAR_GET(ac6_native_takeover)) {
                    // The exact command-processor session owns native drawing
                    // in takeover mode; the older asynchronous one-shot path
                    // would duplicate work with stale command state.
                    g_capture_inflight.store(false, std::memory_order_release);
                    break;
                }
                std::unique_ptr<DrawSnapshot> snapshot;
                {
                    std::lock_guard<std::mutex> snapshot_lock(g_pending_snapshot_mutex);
                    snapshot.swap(g_pending_snapshot);
                }
                if (!snapshot) {
                    break;
                }
                if (!one_shot_prover) {
                    one_shot_prover = std::make_unique<PipelineProver>();
                    one_shot_prover->Run();
                }
                const PipelineProver::ExecResult result =
                    one_shot_prover->ExecuteCapturedDraw(*snapshot);
                if (result == PipelineProver::ExecResult::kExecuted) {
                    g_capture_armed.store(false, std::memory_order_release);
                    REXLOG_CAT_INFO(kLogGPU,
                                    "nr2: draw capture complete after {} snapshot attempts - "
                                    "trigger disarmed",
                                    g_snapshot_attempts.load(std::memory_order_relaxed));
                } else if (result == PipelineProver::ExecResult::kFatal) {
                    g_capture_armed.store(false, std::memory_order_release);
                }
                g_capture_inflight.store(false, std::memory_order_release);
                break;
            }
        }
    }
    if (one_shot_prover) {
        one_shot_prover->Destroy();
    }
    REXLOG_CAT_INFO(kLogGPU,
                    "ac6_native_renderer2: render thread exiting after {} present ticks "
                    "({} commands dropped)",
                    present_ticks, g_dropped_commands.load(std::memory_order_relaxed));
}

void EnqueueCommand(const RenderCommand& cmd) {
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_queue.size() >= kMaxQueuedCommands) {
            g_dropped_commands.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_queue.push_back(cmd);
    }
    g_queue_cv.notify_one();
}

}  // namespace

bool Init() {
    if (!REXCVAR_GET(ac6_native_renderer)) {
        return false;
    }
    if (g_active.load(std::memory_order_acquire)) {
        return true;  // already initialized
    }
    if (REXCVAR_GET(ac6_native_takeover) &&
        REXCVAR_GET(ac6_native_validate_resolves)) {
        // The safety gate compares native resolves against the emulator's
        // current result. The legacy Vulkan boolean maps to delayed readback;
        // live takeover requires the explicit immediate mode.
        rex::cvar::SetFlagByName("readback_resolve", "full");
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: live takeover enabled; forced readback_resolve=full for "
            "same-command oracle validation");
    } else if (REXCVAR_GET(ac6_native_takeover)) {
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: live takeover observation enabled without synchronous "
            "resolve validation; native resolve propagation is disabled");
    }
    g_running.store(true, std::memory_order_release);
    g_render_thread = std::thread(RenderThreadMain);
    g_active.store(true, std::memory_order_release);
    REXLOG_CAT_INFO(kLogGPU,
                    "ac6_native_renderer2: initialized (M3 slice 1: pipeline prover on the "
                    "render thread; observe mode with guest swap hook 0x821DA0D8 feeding the "
                    "command queue)");
    const int32_t capture_draw = REXCVAR_GET(ac6_native_capture_draw);
    if (capture_draw > 0) {
        g_capture_armed.store(true, std::memory_order_release);
        REXLOG_CAT_INFO(kLogGPU,
                        "nr2: M3 slice 2 draw capture ARMED: will snapshot captured draw #{} "
                        "(and later draws until one executes natively; requires "
                        "ac6_render_capture=true)",
                        capture_draw);
    }
    EnqueueDebugMarker("m3-slice2-first-native-draw");
    return true;
}

void Shutdown() {
    if (!g_active.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    g_running.store(false, std::memory_order_release);
    g_queue_cv.notify_all();
    if (g_render_thread.joinable()) {
        g_render_thread.join();
    }
    {
        std::lock_guard<std::mutex> live_lock(g_live_gpu_session_mutex);
        if (g_live_gpu_session) {
            g_live_gpu_session->Destroy();
            g_live_gpu_session.reset();
        }
        g_live_gpu_capture_index = 0;
        g_live_gpu_swap_count.store(0, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.clear();
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool SharedAuthorityEnabled() {
#if defined(AC6_NR2_TRACE_TOOL)
    // The standalone trace tool does not initialize the AC6 application
    // module. Permit an explicit per-process opt-in there, but never let an
    // environment variable bypass the renderer/takeover gates in the shipped
    // game executable.
    static const bool trace_enabled = [] {
        const char* value = std::getenv("AC6_NR2_SHARED_AUTHORITY_TRACE");
        return value && value[0] && std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0;
    }();
    if (trace_enabled) {
        return true;
    }
#endif
    return IsActive() && REXCVAR_GET(ac6_native_takeover) &&
           REXCVAR_GET(ac6_native_shared_authority);
}

bool PrepareAuthoritativeVulkanDraw(
    rex::graphics::CommandProcessor* command_processor, VkDevice device,
    VkPipelineLayout pipeline_layout, uint64_t pipeline_description_hash,
    const uint8_t* pipeline_description, size_t pipeline_description_size,
    VkShaderModule vertex_shader, VkShaderModule pixel_shader,
    VkShaderModule generated_geometry_shader, uint32_t host_vertex_shader_type,
    bool dynamic_rendering, bool host_render_targets,
    bool memexport_writes_possible, VkPipeline* pipeline_out) {
    if (pipeline_out) {
        *pipeline_out = VK_NULL_HANDLE;
    }
    if (!pipeline_out || !SharedAuthorityEnabled()) {
        return false;
    }

    auto* vulkan_command_processor =
        dynamic_cast<rex::graphics::vulkan::VulkanCommandProcessor*>(
            command_processor);
    rex::ui::vulkan::VulkanDevice* vulkan_device =
        vulkan_command_processor
            ? vulkan_command_processor->GetVulkanDevice()
            : nullptr;

    std::lock_guard<std::mutex> lock(g_shared_gpu_session_mutex);
    ++g_shared_prepare_count;
    auto reject = [&] {
        ++g_shared_reject_count;
        return false;
    };

    // Host types 9 and 10 are point and rectangle expansion. Both now borrow
    // the exact SDK translation, but remain opt-in rollout gates until each
    // path has passed a clean live Gracemeria visual A/B.
    const bool allow_shared_point_expansion =
        REXCVAR_GET(ac6_native_shared_point_expansion);
    const bool allow_shared_rectangle_expansion =
        REXCVAR_GET(ac6_native_shared_rectangle_expansion);
    const bool supported_host_vertex_shader_type =
        host_vertex_shader_type == 0 ||
        (allow_shared_point_expansion && host_vertex_shader_type == 9) ||
        (allow_shared_rectangle_expansion && host_vertex_shader_type == 10);
    if (!vulkan_command_processor || !vulkan_device ||
        !vulkan_command_processor->submission_open() ||
        vulkan_device->device() != device || device == VK_NULL_HANDLE ||
        pipeline_layout == VK_NULL_HANDLE || !pipeline_description_hash ||
        !pipeline_description ||
        pipeline_description_size != kPipelineDescriptionSize ||
        vertex_shader == VK_NULL_HANDLE || pixel_shader == VK_NULL_HANDLE ||
        !supported_host_vertex_shader_type || !dynamic_rendering ||
        !host_render_targets || memexport_writes_possible) {
        return reject();
    }

    if (g_shared_failed_device == device) {
        return reject();
    }
    if (g_shared_gpu_session) {
        if (g_shared_device != device ||
            g_shared_vulkan_device != vulkan_device) {
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: rejected shared draw from a different authoritative "
                "Vulkan device before the current device was shut down");
            return reject();
        }
    } else {
        auto session = std::make_unique<PipelineProver>();
        if (!session->RunSharedDevice(vulkan_device)) {
            g_shared_failed_device = device;
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: shared-authority session initialization failed; "
                "falling back to emulator pipelines for this Vulkan device");
            return reject();
        }
        g_shared_vulkan_device = vulkan_device;
        g_shared_device = device;
        g_shared_gpu_session = session.release();
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!g_shared_gpu_session->GetOrCreateSharedPipeline(
            pipeline_description_hash, pipeline_description,
            pipeline_description_size, host_vertex_shader_type,
            pipeline_layout, vertex_shader, pixel_shader,
            generated_geometry_shader, pipeline) ||
        pipeline == VK_NULL_HANDLE) {
        return reject();
    }
    *pipeline_out = pipeline;
    ++g_shared_accept_count;
    if (g_shared_accept_count == 1) {
        // One-shot correctness proof required by the live harness. Performance
        // mode forces error-only logging, so this must remain visible there.
        REXLOG_CAT_ERROR(
            kLogGPU,
            "nr2: first authoritative shared Vulkan draw accepted "
            "(description {:016x})",
            pipeline_description_hash);
    }
    return true;
}

void ShutdownAuthoritativeVulkanDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_shared_gpu_session_mutex);
    if (g_shared_gpu_session) {
        if (device == VK_NULL_HANDLE || device != g_shared_device) {
            // Destroying objects through an unrelated or already-destroyed
            // device is invalid. Keep them alive until the matching command
            // processor reaches its ordered shutdown hook.
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2: ignored shared-authority shutdown for a non-matching "
                "Vulkan device");
            return;
        }
        g_shared_gpu_session->Destroy();
        delete g_shared_gpu_session;
        g_shared_gpu_session = nullptr;
        g_shared_vulkan_device = nullptr;
        g_shared_device = VK_NULL_HANDLE;
    }
    if (g_shared_failed_device == device) {
        g_shared_failed_device = VK_NULL_HANDLE;
    }
    if (g_shared_prepare_count) {
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: shared-authority session shutdown: {} prepared, {} native "
            "accepted, {} emulator fallback",
            g_shared_prepare_count, g_shared_accept_count,
            g_shared_reject_count);
    }
    g_shared_prepare_count = 0;
    g_shared_accept_count = 0;
    g_shared_reject_count = 0;
}

void EnqueuePresent(uint64_t guest_swap_sequence) {
    if (!IsActive()) {
        return;
    }
    RenderCommand cmd;
    cmd.type = RenderCommandType::kPresent;
    cmd.sequence = guest_swap_sequence;
    EnqueueCommand(cmd);
}

void EnqueueDebugMarker(std::string_view label) {
    if (!IsActive()) {
        return;
    }
    RenderCommand cmd;
    cmd.type = RenderCommandType::kSetDebugMarker;
    const size_t len = std::min(label.size(), sizeof(cmd.marker) - 1);
    std::memcpy(cmd.marker, label.data(), len);
    cmd.marker[len] = '\0';
    EnqueueCommand(cmd);
}

void OnDrawCaptured(const ac6::d3d::DrawCallRecord& record) {
    if (!IsActive() || !g_capture_armed.load(std::memory_order_acquire)) {
        return;
    }
    const int32_t target = REXCVAR_GET(ac6_native_capture_draw);
    if (target <= 0) {
        return;
    }
    const uint64_t seen = g_captured_draws_seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen < uint64_t(target)) {
        return;
    }
    bool expected = false;
    if (!g_capture_inflight.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) {
        return;  // a snapshot is already being processed
    }
    const uint64_t attempts = g_snapshot_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempts > kMaxSnapshotAttempts) {
        g_capture_armed.store(false, std::memory_order_release);
        g_capture_inflight.store(false, std::memory_order_release);
        REXLOG_CAT_ERROR(kLogGPU,
                         "nr2: giving up on the draw capture after {} snapshot attempts "
                         "without a suitable draw",
                         kMaxSnapshotAttempts);
        return;
    }
    auto snapshot = std::make_unique<DrawSnapshot>();
    if (!BuildDrawSnapshot(record, seen, *snapshot)) {
        static std::atomic<bool> logged_build_failure{false};
        bool log_expected = false;
        if (logged_build_failure.compare_exchange_strong(log_expected, true)) {
            REXLOG_CAT_WARN(kLogGPU,
                            "nr2: draw snapshot build failed (no thread kernel state or "
                            "graphics system) - will keep trying");
        }
        g_capture_inflight.store(false, std::memory_order_release);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_snapshot_mutex);
        g_pending_snapshot = std::move(snapshot);
    }
    RenderCommand cmd;
    cmd.type = RenderCommandType::kExecuteCapturedDraw;
    EnqueueCommand(cmd);
}

void OnGpuDrawExact(
    rex::graphics::CommandProcessor* command_processor,
    rex::graphics::RegisterFile* register_file,
    rex::memory::Memory* memory,
    uint32_t primitive_type,
    uint32_t index_count) {
    if (!IsActive() || !REXCVAR_GET(ac6_native_takeover)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_live_gpu_session_mutex);
    DrawSnapshot snapshot;
    if (!BuildExactGpuSnapshot(command_processor, register_file, memory,
                               primitive_type, index_count, snapshot)) {
        if (g_live_gpu_session) {
            g_live_gpu_session->InvalidateUnvalidatedProvenance();
        }
        return;
    }
    PipelineProver* session = EnsureLiveGpuSession(command_processor);
    if (session->failed()) {
        return;
    }
    const PipelineProver::ExecResult result =
        session->ExecuteCapturedDraw(snapshot);
    if (result != PipelineProver::ExecResult::kExecuted) {
        // The emulator accepted this draw, but the private native path did
        // not reproduce it. No retained target may be promoted without an
        // oracle until a subsequent clear establishes fresh known contents.
        session->InvalidateUnvalidatedProvenance();
    }
    if (result == PipelineProver::ExecResult::kFatal) {
        REXLOG_CAT_ERROR(kLogGPU,
                         "nr2: live native draw session entered a fatal state");
    }
}

bool OnGpuCopyExact(
    rex::graphics::CommandProcessor* command_processor,
    rex::graphics::RegisterFile* register_file,
    rex::memory::Memory* memory,
    uint32_t* written_address,
    uint32_t* written_length) {
    if (written_address) {
        *written_address = 0;
    }
    if (written_length) {
        *written_length = 0;
    }
    if (!IsActive() || !REXCVAR_GET(ac6_native_takeover)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_live_gpu_session_mutex);
    DrawSnapshot snapshot;
    if (!BuildExactGpuSnapshot(command_processor, register_file, memory, 0, 0,
                               snapshot)) {
        return false;
    }
    PipelineProver* session = EnsureLiveGpuSession(command_processor);
    if (session->failed()) {
        return false;
    }
    const PipelineProver::ExecResult result = session->ResolveCapturedCopy(
        snapshot, written_address, written_length);
    return result == PipelineProver::ExecResult::kExecuted && written_length &&
           *written_length != 0;
}

void OnGpuSwapExact() {
    if (!IsActive()) {
        return;
    }
    auto consume_takeover_marker = [](const std::string& path,
                                      const char* value,
                                      const char* action) {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return;
        }
        if (!std::filesystem::remove(path, ec) || ec) {
            REXLOG_CAT_ERROR(kLogGPU,
                             "nr2: could not consume native takeover {} "
                             "marker '{}' ({})",
                             action, path, ec.message());
            return;
        }
        rex::cvar::SetFlagByName("ac6_native_takeover", value);
        REXLOG_CAT_ERROR(kLogGPU,
                         "nr2: native takeover {} by marker '{}'", action,
                         path);
    };
    consume_takeover_marker(REXCVAR_GET(ac6_native_takeover_disable_marker),
                            "false", "disabled");
    consume_takeover_marker(REXCVAR_GET(ac6_native_takeover_enable_marker),
                            "true", "enabled");
    static const bool perf_log_enabled = [] {
        const char* value = std::getenv("AC6_NR2_PERF_LOG");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    static const bool frame_stats_enabled = [] {
        const char* value = std::getenv("AC6_NR2_FRAME_STATS");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (frame_stats_enabled) {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        static auto previous = now;
        static bool have_previous = false;
        static ac6::nr2::FrameIntervalStats stats;
        static uint64_t total_intervals = 0;
        ac6::nr2::FrameIntervalStats::Summary summary;
        if (have_previous) {
            ++total_intervals;
            if (stats.Add(std::chrono::duration<double, std::milli>(now - previous).count(), summary)) {
                REXLOG_CAT_ERROR(kLogGPU,
                    "nr2-frame-stats: intervals=600 total={} mean_ms={:.3f} "
                    "p50_ms={:.3f} p95_ms={:.3f} p99_ms={:.3f} max_ms={:.3f} "
                    "over20={} over33={}", total_intervals, summary.mean_ms,
                    summary.p50_ms, summary.p95_ms, summary.p99_ms,
                    summary.max_ms, summary.over_20ms, summary.over_33ms);
            }
        }
        previous = now;
        have_previous = true;
    }
    if (perf_log_enabled) {
        using Clock = std::chrono::steady_clock;
        static uint64_t perf_swaps = 0;
        static auto window_start = Clock::now();
        static uint64_t window_start_swap = 0;
        ++perf_swaps;
        if (perf_swaps - window_start_swap >= 60u) {
            const auto now = Clock::now();
            const double seconds =
                std::chrono::duration<double>(now - window_start).count();
            const uint64_t window_swaps = perf_swaps - window_start_swap;
            REXLOG_CAT_ERROR(
                kLogGPU,
                "nr2-perf: {} guest swaps in {:.3f}s = {:.2f} FPS "
                "(total swaps {})",
                window_swaps, seconds,
                seconds > 0.0 ? double(window_swaps) / seconds : 0.0,
                perf_swaps);
            window_start = now;
            window_start_swap = perf_swaps;
        }
    }
    if (!REXCVAR_GET(ac6_native_takeover)) {
        return;
    }
    const uint64_t swaps =
        g_live_gpu_swap_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int32_t configured_required =
        REXCVAR_GET(ac6_native_propagate_after_swaps);
    const uint64_t required =
        uint64_t(std::max(configured_required, 0));
    if (REXCVAR_GET(ac6_native_propagate_unvalidated) &&
        swaps == required) {
        REXLOG_CAT_INFO(
            kLogGPU,
            "nr2: unvalidated native resolve propagation armed after exact "
            "GPU swap #{}",
            swaps);
    }
}

// M3 slice 2 headless trace harness entry (ac6-nr2-trace-draw tool). The
// snapshot is built by the tool from TracePlayer state instead of the live
// emulated GPU; everything from here on is the exact same code path the
// in-game capture trigger exercises.
int ExecuteSnapshotOffscreen(const DrawSnapshot& snap) {
    PipelineProver prover;
    prover.Run();
    const PipelineProver::ExecResult result = prover.ExecuteCapturedDraw(snap);
    prover.Destroy();
    switch (result) {
        case PipelineProver::ExecResult::kExecuted:
            return 0;
        case PipelineProver::ExecResult::kRejected:
            return 1;
        default:
            return 2;
    }
}

namespace {
std::unique_ptr<PipelineProver> g_snapshot_offscreen_session;
}

bool BeginSnapshotOffscreenSession(
    bool persistent_targets,
    rex::graphics::CommandProcessor* command_processor) {
    EndSnapshotOffscreenSession();
    g_snapshot_offscreen_session = std::make_unique<PipelineProver>();
    ConfigureGpuResolveShader(*g_snapshot_offscreen_session,
                              command_processor);
    g_snapshot_offscreen_session->Run();
    g_snapshot_offscreen_session->SetPersistentTargetsEnabled(persistent_targets);
    if (const char* defer = std::getenv("AC6_NR2_DEFER_DRAW_WAITS")) {
        g_snapshot_offscreen_session->SetDeferredDrawWaits(
            persistent_targets && std::strcmp(defer, "0") != 0);
    }
    return true;
}

int ExecuteSnapshotInOffscreenSession(const DrawSnapshot& snap) {
    if (!g_snapshot_offscreen_session) {
        return 2;
    }
    const PipelineProver::ExecResult result =
        g_snapshot_offscreen_session->ExecuteCapturedDraw(snap);
    switch (result) {
        case PipelineProver::ExecResult::kExecuted:
            return 0;
        case PipelineProver::ExecResult::kRejected:
            return 1;
        default:
            return 2;
    }
}

int ResolveSnapshotInOffscreenSession(const DrawSnapshot& snap) {
    if (!g_snapshot_offscreen_session) {
        return 2;
    }
    const PipelineProver::ExecResult result =
        g_snapshot_offscreen_session->ResolveCapturedCopy(snap);
    switch (result) {
        case PipelineProver::ExecResult::kExecuted:
            return 0;
        case PipelineProver::ExecResult::kRejected:
            return 1;
        default:
            return 2;
    }
}

void EndSnapshotOffscreenSession() {
    if (g_snapshot_offscreen_session) {
        g_snapshot_offscreen_session->Destroy();
        g_snapshot_offscreen_session.reset();
    }
}

}  // namespace ac6::nr2

// --------------------------------------------------------------------------
// Guest Present hook (observe/parallel mode)
//
// 0x821DA0D8 is D3D "Swap main": the single call site in the guest swap chain
// (orchestrator 0x821F03B0 -> 0x821DA0D8 -> 0x821D9FF8 which writes the swap
// PM4 packet). Overriding the weak alias rex_sub_821DA0D8 intercepts every
// guest swap; the original is always called, so the emulated renderer keeps
// presenting regardless of the cvar.
//
// AC6_NR2_TRACE_TOOL builds (the ac6-nr2-trace-draw harness) exclude the hook:
// the tool links only the SDK libraries, so the recompiled guest code that
// provides __imp__rex_sub_821DA0D8 is unavailable there.

#if !defined(AC6_NR2_TRACE_TOOL)

PPC_EXTERN_FUNC(__imp__rex_sub_821DA0D8);  // Swap main (original recompiled body)

namespace {
std::atomic<uint64_t> g_guest_swap_sequence{0};
}  // namespace

PPC_FUNC_IMPL(rex_sub_821DA0D8) {
    PPC_FUNC_PROLOGUE();

    __imp__rex_sub_821DA0D8(ctx, base);

    if (ac6::nr2::IsActive()) {
        const uint64_t sequence =
            g_guest_swap_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        ac6::nr2::EnqueuePresent(sequence);
    }
}

#endif  // !AC6_NR2_TRACE_TOOL
