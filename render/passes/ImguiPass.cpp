#include "ImguiPass.h"
#include "../Renderer.h"

#include <array>

namespace demo {

ImguiPass::ImguiPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ImguiPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void ImguiPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("ImGui");
  m_renderer->beginPresentPass(*context.cmd);
  m_renderer->executeImGuiPass(*context.cmd, *context.params);
  m_renderer->endPresentPass(*context.cmd);
  context.cmd->endEvent();
}

}  // namespace demo
