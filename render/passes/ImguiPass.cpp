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
  // The swapchain layout for the UI phase is managed explicitly by
  // Renderer::beginPresentPass()/endPresentPass(). Exposing it as a normal
  // pass dependency makes PassExecutor inject barriers while dynamic rendering
  // is active, which is illegal without dynamicRenderingLocalRead.
  static const std::array<PassResourceDependency, 0> dependencies = {};
  return {dependencies.data(), 0};
}

void ImguiPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("ImGui");
  // PresentPass already began dynamic rendering, just render UI and end
  m_renderer->executeImGuiPass(*context.cmd, *context.params);
  m_renderer->endPresentPass(*context.cmd);
  context.cmd->endEvent();
}

}  // namespace demo
