#include "GPUDrivenAOPass.h"

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
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenAOPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.params == nullptr)
  {
    return;
  }
  if(!context.params->debugOptions.enableAO || m_renderer->getAOTracePipelineOpaque() == 0
     || m_renderer->getAODenoisePipelineOpaque() == 0)
  {
    return;
  }

  const uint32_t frameIndex = context.frameIndex;
  const BindGroupHandle aoBindGroup = m_renderer->getAOBindGroup(frameIndex);
  const BindGroupHandle aoDenoiseBindGroup = m_renderer->getAODenoiseBindGroup(frameIndex);
  const PipelineHandle aoTracePipeline = m_renderer->getAOTracePipelineHandle();
  const PipelineHandle aoDenoisePipeline = m_renderer->getAODenoisePipelineHandle();
  if(aoBindGroup.isNull() || aoDenoiseBindGroup.isNull() || aoTracePipeline.isNull() || aoDenoisePipeline.isNull()
     || m_renderer->getAORawImageOpaque() == 0 || m_renderer->getAODenoisedImageOpaque() == 0)
  {
    return;
  }

  const VkExtent2D halfExtent = m_renderer->getPhase7HalfExtent();
  const VkExtent2D sceneExtent = m_renderer->getSceneExtent();
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

  rhi::ComputeEncoder* trace = context.cmdBuffer->beginComputePass();
  trace->setPipeline(aoTracePipeline);
  trace->setArgumentTable(0, rhi::ArgumentTableHandle{aoBindGroup.index, aoBindGroup.generation});  // bridge (Wave 8)
  trace->setRootConstants(0, &push, sizeof(push));
  trace->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
  context.cmdBuffer->endEncoding();

  // Trace output feeds the denoise pass and later fragment sampling.
  context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                             rhi::HazardFlags::textureWrites);

  rhi::ComputeEncoder* denoise = context.cmdBuffer->beginComputePass();
  denoise->setPipeline(aoDenoisePipeline);
  denoise->setArgumentTable(0, rhi::ArgumentTableHandle{aoDenoiseBindGroup.index, aoDenoiseBindGroup.generation});
  denoise->setRootConstants(0, &push, sizeof(push));
  denoise->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
  context.cmdBuffer->endEncoding();
  context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                             rhi::HazardFlags::textureWrites);
}

}  // namespace demo
