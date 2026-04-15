#include "DepthPyramidPass.h"

#include "../Renderer.h"

#include <array>

namespace demo {

DepthPyramidPass::DepthPyramidPass(Renderer* renderer)
    : m_renderer(renderer)
{}

PassNode::HandleSlice<PassResourceDependency> DepthPyramidPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::write, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void DepthPyramidPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("DepthPyramidPass");
  m_renderer->executeDepthPyramidPass(*context.cmd, *context.params);
  context.cmd->endEvent();
}

}  // namespace demo
