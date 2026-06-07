#include "GPUDrivenImguiPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo {

GPUDrivenImguiPass::GPUDrivenImguiPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenImguiPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 0> dependencies = {};
  return {dependencies.data(), 0};
}

void GPUDrivenImguiPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenImgui");
  m_renderer->executeImGuiPass(*context.commandBuffer, *context.params);
  m_renderer->endPresentPass(*context.commandBuffer);
  context.commandBuffer->endEvent();
}

}  // namespace demo
