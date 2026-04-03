#include "SceneOpaquePass.h"
#include "../Renderer.h"
#include "../DrawStreamWriter.h"

#include <array>
#include <span>

namespace demo {

SceneOpaquePass::SceneOpaquePass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> SceneOpaquePass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassGBufferColorHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBufferDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void SceneOpaquePass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.drawStream == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("SceneOpaque");

  DrawStreamWriter writer{};
  writer.clear();

  const rhi::ResourceIndex materialIndex = m_renderer->resolveMaterialResourceIndex(context.params->materialHandle);
  const rhi::ResourceIndex sceneIndex    = m_renderer->getSceneBindlessResourceIndex();

  writer.setPipeline(m_renderer->getGraphicsPipelineHandle(Renderer::GraphicsPipelineVariant::nonTextured));
  writer.setMaterialIndex(materialIndex);
  writer.setMesh(kNullMeshHandle);
  writer.setDynamicBufferIndex(sceneIndex);
  writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
  writer.draw(0, 3, 1);

  writer.setPipeline(m_renderer->getGraphicsPipelineHandle(Renderer::GraphicsPipelineVariant::textured));
  writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
  writer.draw(3, 3, 1);

  std::vector<StreamEntry>& drawStream = *context.drawStream;
  drawStream                           = writer.entries();
  m_renderer->executeGraphicsPass(*context.cmd, *context.params, std::span<const StreamEntry>(drawStream));

  context.cmd->endEvent();
}

}  // namespace demo
