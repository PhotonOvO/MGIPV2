#include "GPUDrivenLightCullingPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenLightCullingPass::GPUDrivenLightCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenLightCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 7> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenLightCullingPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr)
  {
    context.cmd->beginEvent("GPUDrivenLightCulling");
    m_renderer->executeLightCullingPass(*context.cmd, *context.params);
    context.cmd->endEvent();
  }
}

}  // namespace demo
