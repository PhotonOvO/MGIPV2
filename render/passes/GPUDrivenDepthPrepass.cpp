#include "GPUDrivenDepthPrepass.h"

namespace demo {

GPUDrivenDepthPrepass::GPUDrivenDepthPrepass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenDepthPrepass::getDependencies() const
{
  return {};
}

void GPUDrivenDepthPrepass::execute(const PassContext& context) const
{
  if(context.cmd == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("GPUDrivenDepthPrepass");
  context.cmd->endEvent();
}

}  // namespace demo
