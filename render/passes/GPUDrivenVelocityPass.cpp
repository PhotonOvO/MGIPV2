#include "GPUDrivenVelocityPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
  if(sceneView == nullptr || sceneView->velocityImage == VK_NULL_HANDLE || sceneView->velocityView == VK_NULL_HANDLE)
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
      .view = rhi::TextureViewHandle::fromNative(sceneView->velocityView),
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
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

  const PipelineHandle pipelineHandle = m_renderer->getVelocityPipelineHandle();
  const VkPipeline pipeline = reinterpret_cast<VkPipeline>(m_renderer->getNativeGraphicsPipeline(pipelineHandle));
  const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(m_renderer->getLightPipelineLayout());
  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getLightingInputDescriptorSet());
  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  const VkDescriptorSet cameraSet = reinterpret_cast<VkDescriptorSet>(
      m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  if(pipeline != VK_NULL_HANDLE && layout != VK_NULL_HANDLE && textureSet != VK_NULL_HANDLE
     && cameraSet != VK_NULL_HANDLE)
  {
    const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, shaderio::LSetTextures, 1, &textureSet, 0, nullptr);
    const uint32_t cameraDynamicOffset = context.cameraAlloc.offset;
    vkCmdBindDescriptorSets(vkCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout,
                            shaderio::LSetScene,
                            1,
                            &cameraSet,
                            1,
                            &cameraDynamicOffset);
    context.cmd->draw(3, 1, 0, 0);
  }

  context.cmd->endRenderPass();
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
