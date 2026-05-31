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
  static const std::array<PassResourceDependency, 4> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSSRPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || !context.cameraAllocValid)
  {
    return;
  }
  m_renderer->executeSSRPass(*context.cmd,
                             *context.params,
                             reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                             context.cameraAlloc.offset);
}

}  // namespace demo
