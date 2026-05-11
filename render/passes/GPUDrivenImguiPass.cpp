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
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr)
  {
    m_renderer->executeImguiPass(context);
  }
}

}  // namespace demo
