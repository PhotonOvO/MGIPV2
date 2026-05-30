#include "GPUDrivenBloomPrefilterPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

GPUDrivenBloomPrefilterPass::GPUDrivenBloomPrefilterPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenBloomPrefilterPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassBloomHalfHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenBloomPrefilterPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr
     || !context.params->debugOptions.enablePostProcessing
     || !context.params->debugOptions.enableBloom)
  {
    return;
  }

  const VkImage bloomImage = m_renderer->getBloomHalfImage();
  const VkImageView bloomView = m_renderer->getBloomHalfView();
  const VkExtent2D bloomExtent = m_renderer->getBloomHalfExtent();
  if(bloomImage == VK_NULL_HANDLE || bloomView == VK_NULL_HANDLE || bloomExtent.width == 0u || bloomExtent.height == 0u)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenBloomPrefilter");
  const auto restoreBloomState = [&]() {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassBloomHalfHandle.index, kPassBloomHalfHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(bloomImage),
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::write,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::ColorAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
  };
  rhi::TextureViewHandle bloomViewHandle = rhi::TextureViewHandle::fromNative(bloomView);
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = bloomViewHandle,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassBloomHalfHandle.index, kPassBloomHalfHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(bloomImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  const rhi::Extent2D extent{bloomExtent.width, bloomExtent.height};
  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle pipelineHandle = m_renderer->getBloomPrefilterPipelineHandle();
  if(pipelineHandle.isNull())
  {
    context.cmd->endRenderPass();
    restoreBloomState();
    context.cmd->endEvent();
    return;
  }
  const VkPipeline pipeline = reinterpret_cast<VkPipeline>(
      m_renderer->getNativeGraphicsPipeline(pipelineHandle));
  const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(m_renderer->getLightPipelineLayout());
  const VkDescriptorSet descriptorSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getLightingInputDescriptorSet());
  if(pipeline != VK_NULL_HANDLE && layout != VK_NULL_HANDLE && descriptorSet != VK_NULL_HANDLE)
  {
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
    vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, shaderio::LSetTextures, 1, &descriptorSet, 0, nullptr);

    const VkExtent2D sourceExtent = m_renderer->getSceneExtent();
    const shaderio::PostProcessUniforms postProcessUniforms{
        .params0 = glm::vec4(context.params->debugOptions.postExposure,
                             context.params->debugOptions.bloomIntensity,
                             context.params->debugOptions.bloomThreshold,
                             context.params->debugOptions.enableBloom ? 1.0f : 0.0f),
        .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, sourceExtent.width)),
                             1.0f / static_cast<float>(std::max(1u, sourceExtent.height)),
                             1.0f / static_cast<float>(std::max(1u, bloomExtent.width)),
                             1.0f / static_cast<float>(std::max(1u, bloomExtent.height))),
        .params2 = glm::vec4(0.0f),
        .params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .params4 = glm::vec4(0.0f),
        .params5 = glm::vec4((context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableTAA
                              && !m_renderer->getTAAResolvePipelineHandle().isNull()) ? 1.0f : 0.0f,
                             0.0f,
                             context.params->debugOptions.taaBlendWeight,
                             0.0f),
    };
    const TransientAllocator::Allocation postProcessAlloc =
        context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
    std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
    context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
    m_renderer->updateLightingSceneDescriptorSet(context.frameIndex,
                                                 reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                                                 context.cameraAlloc.offset);
    const VkDescriptorSet sceneDescriptorSet =
        reinterpret_cast<VkDescriptorSet>(m_renderer->getLightingSceneDescriptorSet(context.frameIndex));
    if(sceneDescriptorSet != VK_NULL_HANDLE)
    {
      const std::array<uint32_t, 2> dynamicOffsets{context.cameraAlloc.offset, postProcessAlloc.offset};
      vkCmdBindDescriptorSets(vkCmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout,
                              shaderio::LSetScene,
                              1,
                              &sceneDescriptorSet,
                              static_cast<uint32_t>(dynamicOffsets.size()),
                              dynamicOffsets.data());
      context.cmd->draw(3, 1, 0, 0);
    }
  }

  context.cmd->endRenderPass();
  restoreBloomState();
  context.cmd->endEvent();
}

}  // namespace demo
