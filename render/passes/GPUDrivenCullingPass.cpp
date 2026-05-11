#include "GPUDrivenCullingPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenCullingPass::GPUDrivenCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 6> dependencies = {
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullIndirectBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullStatsBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullUniformBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenCullingPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr)
  {
    context.cmd->beginEvent("GPUDrivenCulling");
    m_renderer->executeGPUCullingPass(*context.cmd, *context.params);
    context.cmd->endEvent();
  }
}

}  // namespace demo
