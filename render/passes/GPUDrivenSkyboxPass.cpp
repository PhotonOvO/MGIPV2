#include "GPUDrivenSkyboxPass.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
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
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.executor == nullptr || context.params == nullptr
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

  context.cmdBuffer->beginEvent("GPUDrivenSkybox");

  // Single source of truth for the color/depth state flips around the pass.
  const auto transition = [&](uint64_t nativeImage, rhi::TextureAspect aspect, rhi::ResourceState from,
                              rhi::ResourceState to) {
    const rhi::TextureBarrier barrier{
        .texture = context.executor->resolveBarrierTexture(nativeImage),
        .before  = from,
        .after   = to,
        .range   = {.aspect = aspect, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0, .layerCount = ~0u},
    };
    context.cmdBuffer->resourceBarrier(&barrier, 1, nullptr, 0);
  };

  transition(targets.colorImage, rhi::TextureAspect::color, rhi::ResourceState::General, rhi::ResourceState::ColorAttachment);
  transition(targets.depthImage, targets.depthAspect, rhi::ResourceState::General, rhi::ResourceState::DepthStencilAttachment);

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

  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea       = {{0, 0}, targets.extent},
      .colorTargets     = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget      = &depthTarget,
  });
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(targets.extent.width),
                                 static_cast<float>(targets.extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, targets.extent});

  // Pipeline + both descriptor sets bind purely through the RHI: the resolver maps
  // the GPUDriven-owned pipeline handle to its native pipeline and tracks the layout,
  // and the texture/scene sets flow through ArgumentTable handles (BindGroup bridge).
  enc->setPipeline(skyboxPipeline);
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                        rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

  // Point the lighting-scene set at this frame's transient camera allocation, then
  // bind it (with its 2 dynamic UBOs, flushed in binding order) through the RHI.
  m_renderer->updateLightingSceneDescriptorSet(context.frameIndex, context.transientAllocator->getBufferOpaque(),
                                               context.cameraAlloc.offset);
  const BindGroupHandle sceneBindGroup = m_renderer->getLightingSceneBindGroup(context.frameIndex);
  enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, context.cameraAlloc.offset, 0);
  enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, 0, 0);
  enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                        rhi::ArgumentTableHandle{sceneBindGroup.index, sceneBindGroup.generation});  // bridge (Wave 8)

  enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});

  context.cmdBuffer->endEncoding();

  transition(targets.colorImage, rhi::TextureAspect::color, rhi::ResourceState::ColorAttachment, rhi::ResourceState::General);
  transition(targets.depthImage, targets.depthAspect, rhi::ResourceState::DepthStencilAttachment, rhi::ResourceState::General);

  context.cmdBuffer->endEvent();
}

}  // namespace demo
