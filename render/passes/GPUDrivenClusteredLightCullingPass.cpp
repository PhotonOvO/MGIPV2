#include "GPUDrivenClusteredLightCullingPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenClusteredLightCullingPass::GPUDrivenClusteredLightCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenClusteredLightCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 7> dependencies = {
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusteredLightUniformHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightCountsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightIndicesHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightStatsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenClusteredLightCullingPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("GPUDrivenClusteredLightCulling");
  m_renderer->executeClusteredLightCullingPass(*context.cmd, *context.params);
  context.cmd->endEvent();
}

}  // namespace demo
