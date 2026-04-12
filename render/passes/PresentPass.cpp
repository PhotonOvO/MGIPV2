#include "PresentPass.h"
#include "../Renderer.h"
#include "../Pass.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>

namespace demo {

PresentPass::PresentPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> PresentPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void PresentPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr)
    return;

  context.cmd->beginEvent("Present");

  // Get source (OutputTexture) and destination (swapchain) images
  VkImage srcImage = m_renderer->getSceneResources().getOutputTextureImage();
  VkImage dstImage = m_renderer->getCurrentSwapchainImage();

  if(srcImage == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }

  // Get native Vulkan command buffer for blit operation
  VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

  // Get actual dimensions for source and destination
  const VkExtent2D srcExtent = m_renderer->getSceneResources().getSize();
  const VkExtent2D dstExtent = m_renderer->getSwapchainExtent();

  // Transition source image to TRANSFER_SRC_OPTIMAL
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::Transfer,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::read,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::TransferSrc,
      .isSwapchain = false,
  });

  // Transition destination image to TRANSFER_DST_OPTIMAL
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::Transfer,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::TransferDst,
      .isSwapchain = true,
  });

  // Blit with Y flip: src uses srcExtent, dst uses dstExtent
  const VkImageBlit blitRegion{
      .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .srcOffsets     = {{0, 0, 0}, {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1}},
      .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .dstOffsets     = {{0, static_cast<int32_t>(dstExtent.height), 0}, {static_cast<int32_t>(dstExtent.width), 0, 1}},
  };
  vkCmdBlitImage(vkCmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &blitRegion, VK_FILTER_LINEAR);

  // Transition destination image back to GENERAL for ImGui pass
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::Transfer,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::TransferDst,
      .newState    = rhi::ResourceState::General,
      .isSwapchain = true,
  });

  // Transition source image back to GENERAL for next frame
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::Transfer,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::TransferSrc,
      .newState    = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  // Begin dynamic rendering to swapchain for subsequent passes (ImguiPass)
  m_renderer->beginPresentPass(*context.cmd);

  context.cmd->endEvent();
}

}  // namespace demo
