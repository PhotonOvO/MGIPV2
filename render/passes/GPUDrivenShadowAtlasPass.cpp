#include "GPUDrivenShadowAtlasPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenShadowAtlasPass::GPUDrivenShadowAtlasPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenShadowAtlasPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::texture(kPassGPUDrivenShadowAtlasHandle,
                                      ResourceAccess::write,
                                      rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenShadowAtlasPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr && context.transientAllocator != nullptr)
  {
    m_renderer->executeShadowAtlasPass(context);
  }
}

}  // namespace demo
