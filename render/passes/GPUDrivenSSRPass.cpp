#include "GPUDrivenSSRPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenSSRPass::GPUDrivenSSRPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenSSRPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSSRPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }
  m_renderer->executeSSRPass(*context.cmd, *context.params);
}

}  // namespace demo
