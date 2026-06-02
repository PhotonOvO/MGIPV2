#include "GPUDrivenAOPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
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

  context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, aoTracePipeline);
  context.cmd->bindBindGroup(0, aoBindGroup, nullptr, 0);
  context.cmd->pushConstants(rhi::ShaderStage::compute, 0, sizeof(push), &push);
  context.cmd->dispatch((halfExtent.width + 7u) / 8u, (halfExtent.height + 7u) / 8u, 1u);

  // Trace output feeds the denoise pass and later fragment sampling.
  context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                             rhi::PipelineStage::Compute | rhi::PipelineStage::FragmentShader,
                             rhi::ResourceAccess::readWrite);

  context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, aoDenoisePipeline);
  context.cmd->bindBindGroup(0, aoDenoiseBindGroup, nullptr, 0);
  context.cmd->pushConstants(rhi::ShaderStage::compute, 0, sizeof(push), &push);
  context.cmd->dispatch((halfExtent.width + 7u) / 8u, (halfExtent.height + 7u) / 8u, 1u);
  context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                             rhi::PipelineStage::Compute | rhi::PipelineStage::FragmentShader,
                             rhi::ResourceAccess::readWrite);
}

}  // namespace demo
