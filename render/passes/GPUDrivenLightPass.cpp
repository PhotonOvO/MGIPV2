#include "GPUDrivenLightPass.h"

namespace demo {

GPUDrivenLightPass::GPUDrivenLightPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenLightPass::getDependencies() const
{
  return {};
}

void GPUDrivenLightPass::execute(const PassContext& context) const
{
  if(context.cmd == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("GPUDrivenLightPass");
  context.cmd->endEvent();
}

}  // namespace demo
