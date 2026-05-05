#include "MeshletCullingPass.h"

namespace demo {

MeshletCullingPass::MeshletCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> MeshletCullingPass::getDependencies() const
{
  return {};
}

void MeshletCullingPass::execute(const PassContext& context) const
{
  if(context.cmd == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("MeshletCullingPass");
  context.cmd->endEvent();
}

}  // namespace demo
