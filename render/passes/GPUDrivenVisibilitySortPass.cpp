#include "GPUDrivenVisibilitySortPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenVisibilitySortPass::GPUDrivenVisibilitySortPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenVisibilitySortPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::buffer(kPassGPUDrivenSortKeyBufferHandle, ResourceAccess::readWrite, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUDrivenSortValueBufferHandle, ResourceAccess::readWrite, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenVisibilitySortPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenVisibilitySort");
  m_renderer->executeVisibilitySortPass(context);
  context.cmd->endEvent();
}

}  // namespace demo
