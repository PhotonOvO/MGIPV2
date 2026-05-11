#include "GPUDrivenPresentPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenPresentPass::GPUDrivenPresentPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenPresentPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenPresentPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr)
  {
    m_renderer->executePresentPass(context);
  }
}

}  // namespace demo
