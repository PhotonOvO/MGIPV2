#include "GPUDrivenAOPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>

namespace demo {

GPUDrivenAOPass::GPUDrivenAOPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenAOPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
                                      rhi::ResourceState::ShaderRead),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenAOPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
  {
    return;
  }
  if(!context.params->debugOptions.enableAO || m_renderer->getAOTracePipelineOpaque() == 0
     || m_renderer->getAODenoisePipelineOpaque() == 0)
  {
    return;
  }

  const uint32_t frameIndex = context.frameIndex;
  const rhi::ArgumentTableHandle aoTable = m_renderer->getAOArgumentTable(frameIndex);
  const rhi::ArgumentTableHandle aoDenoiseTable = m_renderer->getAODenoiseArgumentTable(frameIndex);
  const PipelineHandle aoTracePipeline = m_renderer->getAOTracePipelineHandle();
  const PipelineHandle aoDenoisePipeline = m_renderer->getAODenoisePipelineHandle();
  if(aoTable.isNull() || aoDenoiseTable.isNull() || aoTracePipeline.isNull() || aoDenoisePipeline.isNull()
     || m_renderer->getAORawImageOpaque() == 0 || m_renderer->getAODenoisedImageOpaque() == 0)
  {
    return;
  }

  const rhi::Extent2D halfExtent = m_renderer->getPhase7HalfExtent();
  const rhi::Extent2D sceneExtent = m_renderer->getSceneExtent();
  const shaderio::GPUDrivenAOPushConstants push{
      .params0 = glm::vec4(halfExtent.width, halfExtent.height, context.params->debugOptions.aoRadius,
                           context.params->debugOptions.aoIntensity),
      .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, sceneExtent.width)),
                           1.0f / static_cast<float>(std::max(1u, sceneExtent.height)),
                           64.0f,
                           0.35f),
  };

  const uint32_t groupsX = (halfExtent.width + 7u) / 8u;
  const uint32_t groupsY = (halfExtent.height + 7u) / 8u;

  rhi::ComputeEncoder* trace = context.commandBuffer->beginComputePass();
  trace->setPipeline(aoTracePipeline);
  trace->setArgumentTable(0, aoTable);
  trace->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
  trace->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
  context.commandBuffer->endEncoding();

  // Same-pass barrier: AO trace dispatch writes the raw AO texture consumed by
  // the denoise dispatch below; no pass boundary exists for PassExecutor to model.
  context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                             rhi::HazardFlags::textureWrites);

  rhi::ComputeEncoder* denoise = context.commandBuffer->beginComputePass();
  denoise->setPipeline(aoDenoisePipeline);
  denoise->setArgumentTable(0, aoDenoiseTable);
  denoise->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
  denoise->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
  context.commandBuffer->endEncoding();
  // Local output barrier: AO denoise writes a renderer-private texture sampled by
  // later lighting; it has no PassResourceDependency handle in the graph yet.
  context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                             rhi::HazardFlags::textureWrites);
}

}  // namespace demo
