#include "GPUDrivenSkyboxPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenSkyboxPass::GPUDrivenSkyboxPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenSkyboxPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::readWrite, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSkyboxPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr
     || !context.params->debugOptions.enableIBL || context.params->gpuDrivenSceneView == nullptr
     || !context.cameraAllocValid)
  {
    return;
  }

  const GPUDrivenRenderer::ScreenPassTargets targets = m_renderer->getScreenColorDepthTargets();
  const PipelineHandle  skyboxPipeline = m_renderer->getGPUDrivenSkyboxPipelineHandle();
  if(!targets.valid || skyboxPipeline.isNull())
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenSkybox");

  // Single source of truth for the color/depth state flips around the pass.
  const auto transition = [&](TextureHandle handle, uint64_t nativeImage, rhi::TextureAspect aspect,
                              rhi::ResourceAccess srcAccess, rhi::ResourceAccess dstAccess, rhi::ResourceState from,
                              rhi::ResourceState to) {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{handle.index, handle.generation},
        .nativeImage = nativeImage,
        .aspect      = aspect,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = srcAccess,
        .dstAccess   = dstAccess,
        .oldState    = from,
        .newState    = to,
        .isSwapchain = false,
    });
  };

  transition(kPassSceneColorHdrHandle, targets.colorImage, rhi::TextureAspect::color, rhi::ResourceAccess::read,
             rhi::ResourceAccess::write, rhi::ResourceState::General, rhi::ResourceState::ColorAttachment);
  transition(kPassSceneDepthHandle, targets.depthImage, targets.depthAspect, rhi::ResourceAccess::read,
             rhi::ResourceAccess::read, rhi::ResourceState::General, rhi::ResourceState::DepthStencilAttachment);

  const rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view    = targets.colorView,
      .state   = rhi::ResourceState::ColorAttachment,
      .loadOp  = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
  };
  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = targets.depthView,
      .state      = rhi::ResourceState::DepthStencilAttachment,
      .loadOp     = rhi::LoadOp::load,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea       = {{0, 0}, targets.extent},
      .colorTargets     = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget      = &depthTarget,
  });
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(targets.extent.width),
                                         static_cast<float>(targets.extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, targets.extent});

  // Pipeline + both descriptor sets bind purely through the RHI: the resolver maps
  // the GPUDriven-owned pipeline handle to its native pipeline and tracks the layout,
  // and the texture/scene sets flow through BindGroup handles.
  context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, skyboxPipeline);
  context.cmd->bindBindGroup(shaderio::LSetTextures, m_renderer->getLightingInputBindGroup(context.frameIndex), nullptr, 0);

  // Point the lighting-scene set at this frame's transient camera allocation, then
  // bind it (with its 2 dynamic UBOs) through the RHI bind group path.
  m_renderer->updateLightingSceneDescriptorSet(context.frameIndex, context.transientAllocator->getBufferOpaque(),
                                               context.cameraAlloc.offset);
  const uint32_t sceneDynamicOffsets[] = {context.cameraAlloc.offset, 0u};
  context.cmd->bindBindGroup(shaderio::LSetScene, m_renderer->getLightingSceneBindGroup(context.frameIndex),
                             sceneDynamicOffsets, 2);

  context.cmd->draw(3, 1, 0, 0);

  context.cmd->endRenderPass();

  transition(kPassSceneColorHdrHandle, targets.colorImage, rhi::TextureAspect::color, rhi::ResourceAccess::write,
             rhi::ResourceAccess::read, rhi::ResourceState::ColorAttachment, rhi::ResourceState::General);
  transition(kPassSceneDepthHandle, targets.depthImage, targets.depthAspect, rhi::ResourceAccess::read,
             rhi::ResourceAccess::read, rhi::ResourceState::DepthStencilAttachment, rhi::ResourceState::General);

  context.cmd->endEvent();
}

}  // namespace demo
