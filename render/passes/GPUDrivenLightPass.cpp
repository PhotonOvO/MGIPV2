#include "GPUDrivenLightPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenLightPass::GPUDrivenLightPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenLightPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 9> dependencies = {
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBuffer2Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassCSMShadowHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenLightPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenLightPass");

  const GPUDrivenSceneView* sceneView = context.params != nullptr ? context.params->gpuDrivenSceneView : nullptr;
  if(sceneView == nullptr || sceneView->sceneColorHdrView.isNull())
  {
    context.cmd->endEvent();
    return;
  }

  rhi::TextureViewHandle outputViewHandle = sceneView->sceneColorHdrView;
  const VkExtent2D outputExtent = sceneView->sceneDepthExtent;
  const rhi::Extent2D extent = {outputExtent.width, outputExtent.height};
  if(outputViewHandle.isNull())
  {
    context.cmd->endEvent();
    return;
  }

  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = outputViewHandle,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneColorHdrHandle.index, kPassSceneColorHdrHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  const rhi::RenderPassDesc passDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  };
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(passDesc);
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle lightPipeline = m_renderer->getGPUDrivenLightHdrPipelineHandle();
  if(lightPipeline.isNull())
  {
    context.cmdBuffer->endEncoding();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassSceneColorHdrHandle.index, kPassSceneColorHdrHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage),
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
    return;
  }

  enc->setPipeline(lightPipeline);
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                        rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

  if(!context.cameraAllocValid)
  {
    context.cmdBuffer->endEncoding();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassSceneColorHdrHandle.index, kPassSceneColorHdrHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage),
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
    return;
  }

  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
  m_renderer->updateLightingSceneDescriptorSet(context.frameIndex,
                                               context.transientAllocator->getBufferOpaque(),
                                               cameraAlloc.offset);
  const BindGroupHandle sceneBindGroup = m_renderer->getLightingSceneBindGroup(context.frameIndex);
  if(!sceneBindGroup.isNull())
  {
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, cameraAlloc.offset, 0);
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, 0, 0);
    enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                          rhi::ArgumentTableHandle{sceneBindGroup.index, sceneBindGroup.generation});  // bridge (Wave 8)
  }

  enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
  context.cmdBuffer->endEncoding();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneColorHdrHandle.index, kPassSceneColorHdrHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage),
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
