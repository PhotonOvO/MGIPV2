#include "GPUDrivenDebugPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenDebugPass::GPUDrivenDebugPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenDebugPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenDebugPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr && context.transientAllocator != nullptr)
  {
    m_renderer->executeDebugPass(context);
  }
}

}  // namespace demo
