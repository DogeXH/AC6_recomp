#pragma once

// AC6 Native Renderer v2 (see docs/NATIVE_RENDERER_V2_PLAN.md).
//
// ONE translation module, video.cpp-style. M2 provides the RenderCommand
// queue, the render thread that consumes it, and the guest Present hook
// feeding it (observe/parallel mode: the emulated renderer still presents;
// the native thread just logs a tick per guest swap). M3 slice 1 adds the
// pipeline prover: at render-thread startup a private headless Vulkan
// context is created, the offline-converted SPIR-V cache (cvar
// ac6_native_shader_dir) is loaded into VkShaderModules, and the stored
// pipeline descriptions (cvar ac6_native_pipeline_store, read-only) are
// translated into native VkPipelines, logging an 'nr2: ...' summary.
// M3 slice 2 adds the first native draw: cvar ac6_native_capture_draw > 0
// arms a one-shot draw capture; on the Nth captured guest draw the module
// snapshots the shadow state, the emulated GPU register file, and the active
// shader identities, then executes that draw natively offscreen (zero-copy
// guest memory imported as the shared-memory SSBO where the driver allows)
// and writes the color target to /tmp/m3-test/native-draw.ppm.
// Everything is gated behind the ac6_native_renderer cvar (default false) so
// default behavior is completely unchanged.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "../d3d_state.h"

namespace rex::graphics {
class CommandProcessor;
class RegisterFile;
}  // namespace rex::graphics

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace ac6::nr2 {

enum class RenderCommandType : uint8_t {
    kPresent,
    kSetDebugMarker,
    kExecuteCapturedDraw,
};

struct RenderCommand {
    RenderCommandType type{RenderCommandType::kPresent};
    // kPresent: guest swap sequence number (1-based, producer side).
    uint64_t sequence{0};
    // kSetDebugMarker: NUL-terminated label (truncated to fit).
    char marker[56]{};
};

// Lifecycle — called from the app (Ac6recompApp). Init reads the
// ac6_native_renderer cvar and returns false without any side effects when it
// is disabled; when enabled it starts the render thread. Both are idempotent.
bool Init();
void Shutdown();

// True while the render thread is running (module enabled + initialized).
bool IsActive();

// Producer side (guest/PPC threads). No-ops unless IsActive().
void EnqueuePresent(uint64_t guest_swap_sequence);
void EnqueueDebugMarker(std::string_view label);

// ---------------------------------------------------------------------------
// M3 slice 2 — captured-draw snapshot (guest capture layer -> render thread).

// Identity + constant-map info of one active emulated shader, copied from the
// SDK's Shader object at capture time (guest thread; the emulated command
// processor updates these asynchronously, so the snapshot reflects the draw
// the emulated GPU most recently processed, not necessarily the triggering
// guest D3D call - fine for the static menu frames this slice targets).
struct DrawSnapshotTextureBinding {
    uint8_t fetch_constant{0};
    uint8_t dimension{0};
    bool is_signed{false};
};

struct DrawSnapshotSamplerBinding {
    uint8_t fetch_constant{0};
    uint8_t mag_filter{0};
    uint8_t min_filter{0};
    uint8_t mip_filter{0};
    uint8_t aniso_filter{0};
};

struct DrawSnapshotShader {
    bool valid{false};
    bool ucode_analyzed{false};
    bool writes_depth{false};
    bool float_dynamic_addressing{false};
    uint64_t ucode_hash{0};
    uint32_t float_count{0};
    uint64_t float_bitmap[4]{};
    // Guest-memory ranges read by vertex pulling. Each bit corresponds to
    // one of the 96 vertex fetch constants in the register file.
    uint32_t vertex_fetch_bitmap[3]{};
    // Descriptor order emitted by SpirvShaderTranslator. Texture descriptors
    // start at binding 0; sampler descriptors follow all texture descriptors.
    // This metadata lets the private native device bind the same fetch
    // constants as the emulated Vulkan renderer without re-parsing ucode.
    std::vector<DrawSnapshotTextureBinding> texture_bindings;
    std::vector<DrawSnapshotSamplerBinding> sampler_bindings;
};

struct DrawSnapshot {
    uint64_t capture_index{0};                 // 1-based captured-draw counter
    ac6::d3d::DrawCallRecord record{};         // guest-side shadow-state record
    DrawSnapshotShader vs{};
    DrawSnapshotShader ps{};
    std::vector<uint32_t> regs;                // full RegisterFile::values copy
    const uint8_t* physical_membase{nullptr};  // host ptr to guest physical 0
    uint32_t physical_size{0};                 // shared-memory window bytes
    // M3 slice 2 trace harness only (never set by the in-game capture layer).
    // The live capture trigger keeps retrying until a draw "looks" plausible,
    // so it rejects clip-disabled giant-viewport draws (D3D Clear rectangles)
    // and zero-coverage results. The offline harness targets ONE chosen trace
    // command and judges parity externally against the EDRAM oracle, so those
    // two rejections are relaxed: the SDK-style giant host viewport is kept
    // (offscreen target capped at 1280x720) and a zero-coverage result still
    // writes its PPM and counts as executed.
    bool tool_relaxed{false};
    // Persistent range replay only needs the endpoint image. Intermediate
    // draws skip readback, PPM encoding, and disk writes.
    bool tool_suppress_output{false};
};

// Called by the capture layer (d3d_hooks.cpp CaptureDrawCall) for every
// captured DrawCallRecord, on the guest thread. Applies the
// ac6_native_capture_draw trigger (Nth captured draw and later, while armed),
// builds the DrawSnapshot from the emulated GPU state, and enqueues it for
// the native render thread. No-op unless the module is active and armed.
void OnDrawCaptured(const ac6::d3d::DrawCallRecord& record);

// Exact backend-side live path. These are called synchronously by the Vulkan
// command processor after it has decoded a draw or completed an immediate
// resolve readback, so native execution sees the exact register/shader state
// and may safely compare a native resolve with the emulator result.
void OnGpuDrawExact(rex::graphics::CommandProcessor* command_processor,
                    rex::graphics::RegisterFile* register_file,
                    rex::memory::Memory* memory,
                    uint32_t primitive_type,
                    uint32_t index_count);
bool OnGpuCopyExact(rex::graphics::CommandProcessor* command_processor,
                    rex::graphics::RegisterFile* register_file,
                    rex::memory::Memory* memory,
                    uint32_t* written_address,
                    uint32_t* written_length);
void OnGpuSwapExact();

// Shared-authority live path. The Vulkan command processor has already made
// guest memory, constants, textures, dynamic state and render targets
// authoritative before calling this seam. A matching native pipeline using
// the SDK's exact live shader modules may therefore replace only the graphics
// pipeline while the draw is recorded
// into the emulator-owned target on the emulator-owned device and queue.
bool SharedAuthorityEnabled();
bool PrepareAuthoritativeVulkanDraw(
    rex::graphics::CommandProcessor* command_processor,
    VkDevice device,
    VkPipelineLayout pipeline_layout,
    uint64_t pipeline_description_hash,
    const uint8_t* pipeline_description,
    size_t pipeline_description_size,
    // Borrowed exact live guest translations; nr2 must never destroy them.
    VkShaderModule vertex_shader,
    VkShaderModule pixel_shader,
    // Borrowed from the SDK pipeline cache; nr2 must never destroy it.
    VkShaderModule generated_geometry_shader,
    uint32_t host_vertex_shader_type,
    bool dynamic_rendering,
    bool host_render_targets,
    bool memexport_writes_possible,
    VkPipeline* pipeline_out);
void ShutdownAuthoritativeVulkanDevice(VkDevice device);

// M3 slice 2 headless trace harness entry (the ac6-nr2-trace-draw tool; see
// trace_draw_main.cpp). Runs the pipeline prover on the calling thread
// (private Vulkan context, offline SPIR-V cache, stored pipeline
// descriptions), executes ONE externally built DrawSnapshot offscreen with
// the same code path the in-game capture trigger uses, and tears the context
// down again. The PPM lands at the ac6_native_draw_output cvar path.
// Returns 0 executed, 1 snapshot rejected as unsuitable, 2 fatal error.
int ExecuteSnapshotOffscreen(const DrawSnapshot& snap);

// Reusable trace-tool session. Unlike ExecuteSnapshotOffscreen, this keeps the
// private Vulkan device and pipeline cache alive between snapshots. With
// persistent_targets enabled, compatible guest color/depth targets are loaded
// on later draws instead of being recreated and cleared.
bool BeginSnapshotOffscreenSession(
    bool persistent_targets,
    rex::graphics::CommandProcessor* command_processor = nullptr);
int ExecuteSnapshotInOffscreenSession(const DrawSnapshot& snap);
// Handles an EDRAM kCopy command in a persistent trace session by resolving
// the matching native color target into Xenos-tiled guest memory. Returns 0
// when handled, 1 when the resolve deliberately falls back to the emulated
// copy (unsupported depth/format/state or no matching native target), and 2
// for a fatal native-device error.
int ResolveSnapshotInOffscreenSession(const DrawSnapshot& snap);
void EndSnapshotOffscreenSession();

}  // namespace ac6::nr2
