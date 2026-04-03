#include "AnimateVerticesPass.h"
#include "../Renderer.h"

#include <array>

namespace demo {

AnimateVerticesPass::AnimateVerticesPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> AnimateVerticesPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void AnimateVerticesPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("AnimateVertices");
  // Forward to Renderer's compute pass execution wrapper
  m_renderer->executeComputePass(*context.cmd, *context.params);
  context.cmd->endEvent();
}

}  // namespace demo
