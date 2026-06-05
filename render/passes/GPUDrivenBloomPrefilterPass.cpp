#include "GPUDrivenBloomPrefilterPass.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
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
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.executor == nullptr || context.params == nullptr
     || !context.params->debugOptions.enablePostProcessing
     || !context.params->debugOptions.enableBloom)
  {
    return;
  }

  const VkImage bloomImage = m_renderer->getBloomHalfImage();
  const rhi::TextureViewHandle bloomView = m_renderer->getBloomHalfView();
  const VkExtent2D bloomExtent = m_renderer->getBloomHalfExtent();
  if(bloomImage == VK_NULL_HANDLE || bloomView.isNull() || bloomExtent.width == 0u || bloomExtent.height == 0u)
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenBloomPrefilter");
  const rhi::TextureHandle bloomBarrierTex = context.executor->resolveBarrierTexture(reinterpret_cast<uint64_t>(bloomImage));
  const auto transitionBloom = [&](rhi::ResourceState before, rhi::ResourceState after) {
    const rhi::TextureBarrier barrier{
        .texture = bloomBarrierTex,
        .before  = before,
        .after   = after,
        .range   = {.aspect = rhi::TextureAspect::color, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0, .layerCount = ~0u},
    };
    context.cmdBuffer->resourceBarrier(&barrier, 1, nullptr, 0);
  };
  const auto restoreBloomState = [&]() { transitionBloom(rhi::ResourceState::ColorAttachment, rhi::ResourceState::General); };
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = bloomView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  transitionBloom(rhi::ResourceState::General, rhi::ResourceState::ColorAttachment);

  const rhi::Extent2D extent{bloomExtent.width, bloomExtent.height};
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle pipelineHandle = m_renderer->getBloomPrefilterPipelineHandle();
  if(pipelineHandle.isNull())
  {
    context.cmdBuffer->endEncoding();
    restoreBloomState();
    context.cmdBuffer->endEvent();
    return;
  }
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  if(!pipelineHandle.isNull() && !inputBindGroup.isNull())
  {
    enc->setPipeline(pipelineHandle);
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                          rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

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
                                                 context.transientAllocator->getBufferOpaque(),
                                                 context.cameraAlloc.offset);
    const BindGroupHandle sceneBindGroup = m_renderer->getLightingSceneBindGroup(context.frameIndex);
    if(!sceneBindGroup.isNull())
    {
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, context.cameraAlloc.offset, 0);
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, postProcessAlloc.offset, 0);
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                            rhi::ArgumentTableHandle{sceneBindGroup.index, sceneBindGroup.generation});  // bridge (Wave 8)
      enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
    }
  }

  context.cmdBuffer->endEncoding();
  restoreBloomState();
  context.cmdBuffer->endEvent();
}

}  // namespace demo
