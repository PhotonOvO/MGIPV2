#include "GPUCullingPass.h"

#include "../Renderer.h"

#include <array>

namespace demo {

GPUCullingPass::GPUCullingPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUCullingPass::getDependencies() const
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

void GPUCullingPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUCullingPass");
  m_renderer->executeGPUCullingPass(*context.cmd, *context.params);
  context.cmd->endEvent();
}

}  // namespace demo
