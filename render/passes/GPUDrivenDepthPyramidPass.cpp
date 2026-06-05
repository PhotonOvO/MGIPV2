#include "GPUDrivenDepthPyramidPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenDepthPyramidPass::GPUDrivenDepthPyramidPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenDepthPyramidPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::write, rhi::ShaderStage::compute,
                                      rhi::ResourceState::General),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenDepthPyramidPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmdBuffer != nullptr && context.params != nullptr)
  {
    context.cmdBuffer->beginEvent("GPUDrivenDepthPyramid");
    m_renderer->executeDepthPyramidPass(*context.cmdBuffer, *context.params);
    context.cmdBuffer->endEvent();
  }
}

}  // namespace demo
