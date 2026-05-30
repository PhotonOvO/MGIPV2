#include "GPUDrivenSkyboxPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

namespace {

[[nodiscard]] rhi::TextureAspect sceneDepthAspect(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return rhi::TextureAspect::depthStencil;
    default:
      return rhi::TextureAspect::depth;
  }
}

}  // namespace

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
     || !context.params->debugOptions.enableIBL)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneColorHdrImage == VK_NULL_HANDLE
     || sceneView->sceneColorHdrView == VK_NULL_HANDLE || sceneView->sceneDepthImage == VK_NULL_HANDLE
     || sceneView->sceneDepthView == VK_NULL_HANDLE || sceneView->sceneDepthExtent.width == 0u
     || sceneView->sceneDepthExtent.height == 0u)
  {
    return;
  }

  const PipelineHandle skyboxPipeline = m_renderer->getGPUDrivenSkyboxPipelineHandle();
  if(skyboxPipeline.isNull())
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenSkybox");

  const rhi::TextureAspect depthAspect = sceneDepthAspect(sceneView->sceneDepthFormat);
  const auto restoreResourcesForSampling = [&]() {
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
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
        .aspect = depthAspect,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::read,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::DepthStencilAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
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
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
      .aspect = depthAspect,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

  const rhi::Extent2D extent{sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(sceneView->sceneColorHdrView),
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
  };
  const rhi::DepthTargetDesc depthTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(sceneView->sceneDepthView),
      .state = rhi::ResourceState::DepthStencilAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = &depthTarget,
  });
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const VkPipeline pipeline = reinterpret_cast<VkPipeline>(m_renderer->getNativeGraphicsPipeline(skyboxPipeline));
  const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(m_renderer->getLightPipelineLayout());
  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getLightingInputDescriptorSet());
  if(pipeline != VK_NULL_HANDLE && layout != VK_NULL_HANDLE && textureSet != VK_NULL_HANDLE && context.cameraAllocValid)
  {
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
    vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    m_renderer->updateLightingSceneDescriptorSet(context.frameIndex,
                                                 reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                                                 context.cameraAlloc.offset);
    VkDescriptorSet sceneDescriptorSet =
        reinterpret_cast<VkDescriptorSet>(m_renderer->getLightingSceneDescriptorSet(context.frameIndex));
    if(sceneDescriptorSet != VK_NULL_HANDLE)
    {
      const std::array<uint32_t, 2> dynamicOffsets{context.cameraAlloc.offset, 0u};
      vkCmdBindDescriptorSets(vkCmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout,
                              shaderio::LSetScene,
                              1,
                              &sceneDescriptorSet,
                              static_cast<uint32_t>(dynamicOffsets.size()),
                              dynamicOffsets.data());
    }
    context.cmd->draw(3, 1, 0, 0);
  }

  context.cmd->endRenderPass();
  restoreResourcesForSampling();
  context.cmd->endEvent();
}

}  // namespace demo
