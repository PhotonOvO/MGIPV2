#include "PresentPass.h"
#include "../Renderer.h"

#include <array>

namespace demo {

PresentPass::PresentPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> PresentPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassGBufferColorHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void PresentPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
    return;
  context.cmd->beginEvent("Present");
  context.cmd->endEvent();
}

}  // namespace demo
