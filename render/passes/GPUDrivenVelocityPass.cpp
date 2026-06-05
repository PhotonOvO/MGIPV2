#include "GPUDrivenVelocityPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenVelocityPass::GPUDrivenVelocityPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenVelocityPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassVelocityHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenVelocityPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr || !context.cameraAllocValid)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->velocityImage == VK_NULL_HANDLE || sceneView->velocityView.isNull())
  {
    return;
  }

  const VkExtent2D extent = m_renderer->getSceneExtent();
  if(extent.width == 0u || extent.height == 0u)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenVelocity");
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassVelocityHandle.index, kPassVelocityHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->velocityImage),
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
      .view = sceneView->velocityView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
  };
  const rhi::Extent2D rhiExtent{extent.width, extent.height};
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, rhiExtent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, rhiExtent});

  const PipelineHandle pipelineHandle = m_renderer->getVelocityPipelineHandle();
  m_renderer->updateLightingSceneDescriptorSet(context.frameIndex,
                                               context.transientAllocator->getBufferOpaque(),
                                               context.cameraAlloc.offset);
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  const BindGroupHandle sceneBindGroup = m_renderer->getLightingSceneBindGroup(context.frameIndex);
  if(!pipelineHandle.isNull() && !inputBindGroup.isNull() && !sceneBindGroup.isNull())
  {
    enc->setPipeline(pipelineHandle);
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                          rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)
    // LSetScene carries 2 dynamic UBOs (camera + scene); offsets flush in this order.
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, context.cameraAlloc.offset, 0);
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, 0, 0);
    enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                          rhi::ArgumentTableHandle{sceneBindGroup.index, sceneBindGroup.generation});  // bridge (Wave 8)
    enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
  }

  context.cmdBuffer->endEncoding();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassVelocityHandle.index, kPassVelocityHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->velocityImage),
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
