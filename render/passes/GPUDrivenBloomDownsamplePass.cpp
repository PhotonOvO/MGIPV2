#include "GPUDrivenBloomDownsamplePass.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

GPUDrivenBloomDownsamplePass::GPUDrivenBloomDownsamplePass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenBloomDownsamplePass::getDependencies() const
{
  static const std::array<PassResourceDependency, 9> dependencies = {
      PassResourceDependency::texture(kPassBloomHalfHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassBloomQuarterHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomEighthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomSixteenthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomThirtySecondHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomUpsampleSixteenthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomUpsampleEighthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomUpsampleQuarterHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassBloomOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenBloomDownsamplePass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.executor == nullptr || context.params == nullptr
     || !context.params->debugOptions.enablePostProcessing
     || !context.params->debugOptions.enableBloom)
  {
    return;
  }

  struct BloomStep
  {
    VkImage image{VK_NULL_HANDLE};
    rhi::TextureViewHandle view{};
    VkExtent2D extent{};
    VkExtent2D sourceExtent{};
    TextureHandle handle{};
    PipelineHandle pipeline{};
    uint32_t sourceIndex{0u};
    uint32_t lowerIndex{0u};
    float radius{1.0f};
  };

  const PipelineHandle downsamplePipeline = m_renderer->getBloomDownsamplePipelineHandle();
  const PipelineHandle upsamplePipeline = m_renderer->getBloomUpsamplePipelineHandle();
  if(downsamplePipeline.isNull() || upsamplePipeline.isNull())
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenBloomDownsample");
  const auto transitionStep = [&](uint64_t nativeImage, rhi::ResourceState before, rhi::ResourceState after) {
    const rhi::TextureBarrier barrier{
        .texture = context.executor->resolveBarrierTexture(nativeImage),
        .before  = before,
        .after   = after,
        .range   = {.aspect = rhi::TextureAspect::color, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0, .layerCount = ~0u},
    };
    context.cmdBuffer->resourceBarrier(&barrier, 1, nullptr, 0);
  };
  const auto renderStep = [&](const BloomStep& step) {
    if(step.image == VK_NULL_HANDLE || step.view.isNull() || step.extent.width == 0u || step.extent.height == 0u)
    {
      return;
    }

    transitionStep(reinterpret_cast<uint64_t>(step.image), rhi::ResourceState::General, rhi::ResourceState::ColorAttachment);

    rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = step.view,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const rhi::Extent2D extent{step.extent.width, step.extent.height};
    rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
        .renderArea = {{0, 0}, extent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = nullptr,
    });
    enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    enc->setScissor(rhi::Rect2D{{0, 0}, extent});

    const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
    if(!step.pipeline.isNull() && !inputBindGroup.isNull())
    {
      enc->setPipeline(step.pipeline);
      enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                            rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

    const shaderio::PostProcessUniforms postProcessUniforms{
        .params0 = glm::vec4(context.params->debugOptions.postExposure,
                             context.params->debugOptions.bloomIntensity,
                             context.params->debugOptions.bloomThreshold,
                             context.params->debugOptions.enableBloom ? 1.0f : 0.0f),
        .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, step.sourceExtent.width)),
                             1.0f / static_cast<float>(std::max(1u, step.sourceExtent.height)),
                             1.0f / static_cast<float>(std::max(1u, step.extent.width)),
                             1.0f / static_cast<float>(std::max(1u, step.extent.height))),
        .params2 = glm::vec4(0.0f),
        .params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .params4 = glm::vec4(0.0f),
        .params5 = glm::vec4((context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableTAA
                              && !m_renderer->getTAAResolvePipelineHandle().isNull()) ? 1.0f : 0.0f,
                             static_cast<float>(step.sourceIndex),
                             static_cast<float>(step.lowerIndex),
                             step.radius),
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
    transitionStep(reinterpret_cast<uint64_t>(step.image), rhi::ResourceState::ColorAttachment, rhi::ResourceState::General);
  };

  renderStep(BloomStep{m_renderer->getBloomQuarterImage(), m_renderer->getBloomQuarterView(), m_renderer->getBloomQuarterExtent(), m_renderer->getBloomHalfExtent(), kPassBloomQuarterHandle, downsamplePipeline, 5u, 0u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomEighthImage(), m_renderer->getBloomEighthView(), m_renderer->getBloomEighthExtent(), m_renderer->getBloomQuarterExtent(), kPassBloomEighthHandle, downsamplePipeline, 6u, 0u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomSixteenthImage(), m_renderer->getBloomSixteenthView(), m_renderer->getBloomSixteenthExtent(), m_renderer->getBloomEighthExtent(), kPassBloomSixteenthHandle, downsamplePipeline, 13u, 0u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomThirtySecondImage(), m_renderer->getBloomThirtySecondView(), m_renderer->getBloomThirtySecondExtent(), m_renderer->getBloomSixteenthExtent(), kPassBloomThirtySecondHandle, downsamplePipeline, 14u, 0u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomUpsampleSixteenthImage(), m_renderer->getBloomUpsampleSixteenthView(), m_renderer->getBloomUpsampleSixteenthExtent(), m_renderer->getBloomThirtySecondExtent(), kPassBloomUpsampleSixteenthHandle, upsamplePipeline, 14u, 15u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomUpsampleEighthImage(), m_renderer->getBloomUpsampleEighthView(), m_renderer->getBloomUpsampleEighthExtent(), m_renderer->getBloomSixteenthExtent(), kPassBloomUpsampleEighthHandle, upsamplePipeline, 13u, 16u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomUpsampleQuarterImage(), m_renderer->getBloomUpsampleQuarterView(), m_renderer->getBloomUpsampleQuarterExtent(), m_renderer->getBloomEighthExtent(), kPassBloomUpsampleQuarterHandle, upsamplePipeline, 6u, 17u, 1.0f});
  renderStep(BloomStep{m_renderer->getBloomOutputImage(), m_renderer->getBloomOutputView(), m_renderer->getBloomOutputExtent(), m_renderer->getBloomQuarterExtent(), kPassBloomOutputHandle, upsamplePipeline, 5u, 18u, 1.0f});
  context.cmdBuffer->endEvent();
}

}  // namespace demo
