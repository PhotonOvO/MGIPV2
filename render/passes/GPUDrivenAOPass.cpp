#include "GPUDrivenAOPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenAOPass::GPUDrivenAOPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenAOPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenAOPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }
  m_renderer->executeAOPass(*context.cmd, *context.params);
}

}  // namespace demo
