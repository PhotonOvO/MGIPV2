#include "GPUDrivenGBufferPass.h"

namespace demo {

GPUDrivenGBufferPass::GPUDrivenGBufferPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenGBufferPass::getDependencies() const
{
  return {};
}

void GPUDrivenGBufferPass::execute(const PassContext& context) const
{
  if(context.cmd == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("GPUDrivenGBufferPass");
  context.cmd->endEvent();
}

}  // namespace demo
