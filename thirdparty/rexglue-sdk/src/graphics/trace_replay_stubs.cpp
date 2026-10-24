// Tool-only stubs for the offline trace replay executable.
//
// The deck tree's Vulkan command processor reports swap decisions to the AC6
// application's backend diagnostics (ac6::backend::ReportSwapDecision, defined
// in the app's src/ac6_backend_fixes/ac6_backend_hooks.cpp). The trace replay
// tool links only the SDK libraries, not the application, so provide a no-op
// definition with the identical mangled signature. This translation unit is
// compiled into the rexgpu-vulkan-trace-dump executable only.

#include "../../../../src/ac6_backend_fixes/ac6_backend_hooks.h"

// The AC6-specific trace tool links native_renderer.cpp, while the generic
// SDK trace replayer doesn't. Weak references let the former exercise the
// shared-authority seam and keep the latter a no-op without another target-
// specific stub library.
namespace ac6::nr2 {
__attribute__((weak)) bool SharedAuthorityEnabled();
__attribute__((weak)) bool PrepareAuthoritativeVulkanDraw(
    rex::graphics::CommandProcessor* command_processor, VkDevice device,
    VkPipelineLayout pipeline_layout, uint64_t pipeline_description_hash,
    const uint8_t* pipeline_description, size_t pipeline_description_size,
    VkShaderModule vertex_shader, VkShaderModule pixel_shader,
    VkShaderModule generated_geometry_shader, uint32_t host_vertex_shader_type,
    bool dynamic_rendering, bool host_render_targets,
    bool memexport_writes_possible, VkPipeline* pipeline_out);
__attribute__((weak)) void ShutdownAuthoritativeVulkanDevice(VkDevice device);
}  // namespace ac6::nr2

namespace ac6::backend {

void ReportSwapDecision(const rex::system::GraphicsSwapSubmission& /*submission*/,
                        uint64_t /*submission_sequence*/,
                        SwapSourceType /*swap_source*/,
                        bool /*swap_source_scaled*/,
                        uint32_t /*guest_output_width*/,
                        uint32_t /*guest_output_height*/,
                        uint32_t /*source_width*/,
                        uint32_t /*source_height*/,
                        uint64_t /*active_vertex_shader_hash*/,
                        uint64_t /*active_pixel_shader_hash*/) {}

void NativeRendererOnGpuDraw(
    rex::graphics::CommandProcessor* /*command_processor*/,
    rex::graphics::RegisterFile* /*register_file*/,
    rex::memory::Memory* /*memory*/,
    uint32_t /*primitive_type*/,
    uint32_t /*index_count*/) {}

bool NativeRendererOnGpuCopy(
    rex::graphics::CommandProcessor* /*command_processor*/,
    rex::graphics::RegisterFile* /*register_file*/,
    rex::memory::Memory* /*memory*/,
    uint32_t* /*written_address*/,
    uint32_t* /*written_length*/) {
  return false;
}

void NativeRendererOnGpuSwap() {}

bool NativeRendererSharedAuthorityEnabled() {
  return ac6::nr2::SharedAuthorityEnabled &&
         ac6::nr2::SharedAuthorityEnabled();
}

bool NativeRendererPrepareVulkanDraw(
    const NativeVulkanDrawRequest& request, NativeVulkanDrawPlan& plan) {
  plan = {};
  if (!ac6::nr2::PrepareAuthoritativeVulkanDraw) {
    return false;
  }
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
  if (ac6::nr2::ShutdownAuthoritativeVulkanDevice) {
    ac6::nr2::ShutdownAuthoritativeVulkanDevice(device);
  }
}

}  // namespace ac6::backend
