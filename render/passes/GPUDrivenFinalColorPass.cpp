#include "GPUDrivenFinalColorPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

GPUDrivenFinalColorPass::GPUDrivenFinalColorPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenFinalColorPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassBloomOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenFinalColorPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  const VkImage outputImage = m_renderer->getOutputTextureImage();
  const rhi::TextureViewHandle outputView = m_renderer->getOutputTextureView();
  const VkExtent2D outputExtent = m_renderer->getSceneExtent();
  if(outputImage == VK_NULL_HANDLE || outputView.isNull() || outputExtent.width == 0u || outputExtent.height == 0u)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenFinalColor");
  const auto restoreOutputState = [&]() {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(outputImage),
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
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = outputView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(outputImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  const rhi::Extent2D extent{outputExtent.width, outputExtent.height};
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle pipelineHandle = m_renderer->getFinalColorPipelineHandle();
  if(pipelineHandle.isNull())
  {
    context.cmdBuffer->endEncoding();
    restoreOutputState();
    context.cmd->endEvent();
    return;
  }
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  if(!pipelineHandle.isNull() && !inputBindGroup.isNull())
  {
    enc->setPipeline(pipelineHandle);
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                          rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

    const float exposure = context.params->debugOptions.enablePostProcessing
                               ? std::max(context.params->debugOptions.postExposure, 0.01f)
                               : 1.0f;
    const shaderio::PostProcessUniforms postProcessUniforms{
        .params0 = glm::vec4(exposure,
                             context.params->debugOptions.bloomIntensity,
                             context.params->debugOptions.bloomThreshold,
                             (context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableBloom) ? 1.0f : 0.0f),
        .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, outputExtent.width)),
                             1.0f / static_cast<float>(std::max(1u, outputExtent.height)),
                             1.0f / static_cast<float>(std::max(1u, outputExtent.width)),
                             1.0f / static_cast<float>(std::max(1u, outputExtent.height))),
        .params2 = glm::vec4((context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableAdaptiveExposure) ? 1.0f : 0.0f,
                             context.params->debugOptions.exposureTargetLuminance,
                             context.params->debugOptions.minAutoExposure,
                             context.params->debugOptions.maxAutoExposure),
        .params3 = glm::vec4((context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableColorGrading)
                                 ? context.params->debugOptions.colorSaturation
                                 : 1.0f,
                             (context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableColorGrading)
                                 ? context.params->debugOptions.colorContrast
                                 : 1.0f,
                             (context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableColorGrading)
                                 ? context.params->debugOptions.colorGamma
                                 : 1.0f,
                             (context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableColorGrading)
                                 ? context.params->debugOptions.vignetteIntensity
                                 : 0.0f),
        .params4 = glm::vec4((context.params->debugOptions.enablePostProcessing
                               && context.params->debugOptions.enableLensEffects) ? 1.0f : 0.0f,
                              context.params->debugOptions.lensDirtIntensity,
                              (context.params->debugOptions.enablePostProcessing
                               && context.params->debugOptions.enableColorGrading)
                                  ? std::clamp(context.params->debugOptions.colorLutStrength, 0.0f, 1.0f)
                                  : 0.0f,
                              0.0f),
        .params5 = glm::vec4((context.params->debugOptions.enablePostProcessing
                              && context.params->debugOptions.enableTAA
                              && !m_renderer->getTAAResolvePipelineHandle().isNull()) ? 1.0f : 0.0f,
                             0.0f,
                             context.params->debugOptions.taaBlendWeight,
                             context.params->debugOptions.showVelocity ? 1.0f : 0.0f),
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
  restoreOutputState();
  context.cmd->endEvent();
}

}  // namespace demo
