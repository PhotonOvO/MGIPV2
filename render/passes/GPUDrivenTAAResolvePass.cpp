#include "GPUDrivenTAAResolvePass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

GPUDrivenTAAResolvePass::GPUDrivenTAAResolvePass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenTAAResolvePass::getDependencies() const
{
  static const std::array<PassResourceDependency, 4> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassVelocityHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHistoryWriteHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenTAAResolvePass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  const bool taaEnabled = context.params->debugOptions.enablePostProcessing && context.params->debugOptions.enableTAA;
  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || !taaEnabled || sceneView->sceneColorHistoryWriteImage == VK_NULL_HANDLE
     || sceneView->sceneColorHistoryWriteView.isNull())
  {
    return;
  }

  const VkExtent2D extent = m_renderer->getSceneExtent();
  if(extent.width == 0u || extent.height == 0u)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenTAAResolve");
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneColorHistoryWriteHandle.index, kPassSceneColorHistoryWriteHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHistoryWriteImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  const rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = sceneView->sceneColorHistoryWriteView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };
  const rhi::Extent2D rhiExtent{extent.width, extent.height};
  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, rhiExtent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, rhiExtent});

  const PipelineHandle pipelineHandle = m_renderer->getTAAResolvePipelineHandle();
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  if(!pipelineHandle.isNull() && !inputBindGroup.isNull() && context.cameraAllocValid)
  {
    context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, pipelineHandle);
    context.cmd->bindBindGroup(shaderio::LSetTextures, inputBindGroup, nullptr, 0);

    const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
    const shaderio::PostProcessUniforms postProcessUniforms{
        .params0 = glm::vec4(context.params->debugOptions.postExposure,
                             context.params->debugOptions.bloomIntensity,
                             context.params->debugOptions.bloomThreshold,
                             context.params->debugOptions.enableBloom ? 1.0f : 0.0f),
        .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, extent.width)),
                             1.0f / static_cast<float>(std::max(1u, extent.height)),
                             1.0f / static_cast<float>(std::max(1u, extent.width)),
                             1.0f / static_cast<float>(std::max(1u, extent.height))),
        .params2 = glm::vec4(0.0f),
        .params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .params4 = glm::vec4(0.0f),
        .params5 = glm::vec4(1.0f,
                             m_renderer->isTAAHistoryValid() ? 1.0f : 0.0f,
                             std::clamp(context.params->debugOptions.taaBlendWeight, 0.0f, 0.98f),
                             0.0f),
    };
    const TransientAllocator::Allocation postProcessAlloc =
        context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
    std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
    context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
    m_renderer->updateLightingSceneDescriptorSet(context.frameIndex,
                                                 context.transientAllocator->getBufferOpaque(),
                                                 cameraAlloc.offset);
    const BindGroupHandle sceneBindGroup = m_renderer->getLightingSceneBindGroup(context.frameIndex);
    if(!sceneBindGroup.isNull())
    {
      const std::array<uint32_t, 2> dynamicOffsets{cameraAlloc.offset, postProcessAlloc.offset};
      context.cmd->bindBindGroup(shaderio::LSetScene, sceneBindGroup, dynamicOffsets.data(),
                                 static_cast<uint32_t>(dynamicOffsets.size()));
      context.cmd->draw(3, 1, 0, 0);
    }
  }

  context.cmd->endRenderPass();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneColorHistoryWriteHandle.index, kPassSceneColorHistoryWriteHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHistoryWriteImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::ColorAttachment,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });
  context.cmd->endEvent();
}

}  // namespace demo
