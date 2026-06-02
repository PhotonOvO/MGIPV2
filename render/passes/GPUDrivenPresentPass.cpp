#include "GPUDrivenPresentPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>

namespace demo {

GPUDrivenPresentPass::GPUDrivenPresentPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenPresentPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenPresentPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenPresent");
  const VkExtent2D srcExtent = m_renderer->getSceneViewDepthExtent();
  const VkExtent2D dstExtent = m_renderer->getSwapchainExtent();
  const VkImage srcImage = reinterpret_cast<VkImage>(m_renderer->getSceneViewOutputImageOpaque());
  const VkImage dstImage = m_renderer->getCurrentSwapchainImage();
  if(srcImage == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }

  const float srcAspect = static_cast<float>(srcExtent.width) / static_cast<float>(srcExtent.height);
  const float dstAspect = static_cast<float>(dstExtent.width) / static_cast<float>(dstExtent.height);
  VkOffset3D srcOffset0 = {0, 0, 0};
  VkOffset3D srcOffset1 = {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1};
  int32_t dstY0 = 0;
  int32_t dstY1 = static_cast<int32_t>(dstExtent.height);
  int32_t dstX0 = 0;
  int32_t dstX1 = static_cast<int32_t>(dstExtent.width);

  if(dstAspect > srcAspect)
  {
    const int32_t scaledWidth = static_cast<int32_t>(dstExtent.height * srcAspect);
    const int32_t barWidth = (dstExtent.width - scaledWidth) / 2;
    dstX0 = barWidth;
    dstX1 = barWidth + scaledWidth;
  }
  else if(dstAspect < srcAspect)
  {
    const int32_t scaledHeight = static_cast<int32_t>(dstExtent.width / srcAspect);
    const int32_t barHeight = (dstExtent.height - scaledHeight) / 2;
    dstY0 = barHeight;
    dstY1 = barHeight + scaledHeight;
  }

  VkOffset3D dstOffset0 = {dstX0, dstY0, 0};
  VkOffset3D dstOffset1 = {dstX1, dstY1, 1};

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::Transfer,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::TransferSrc,
      .isSwapchain = false,
  });
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::Transfer,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::TransferDst,
      .isSwapchain = true,
  });

  context.cmd->blitImage(rhi::ImageBlitDesc{
      .srcImage   = reinterpret_cast<uint64_t>(srcImage),
      .dstImage   = reinterpret_cast<uint64_t>(dstImage),
      .srcState   = rhi::ResourceState::TransferSrc,
      .dstState   = rhi::ResourceState::TransferDst,
      .aspect     = rhi::TextureAspect::color,
      .srcOffsets = {{srcOffset0.x, srcOffset0.y, srcOffset0.z}, {srcOffset1.x, srcOffset1.y, srcOffset1.z}},
      .dstOffsets = {{dstOffset0.x, dstOffset0.y, dstOffset0.z}, {dstOffset1.x, dstOffset1.y, dstOffset1.z}},
  });

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::Transfer,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::TransferDst,
      .newState = rhi::ResourceState::General,
      .isSwapchain = true,
  });
  context.cmd->setResourceState(
      rhi::ResourceHandle{rhi::ResourceKind::Texture, kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      rhi::ResourceState::General);
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::Transfer,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::TransferSrc,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  m_renderer->beginPresentPass(*context.cmd);
  context.cmd->endEvent();
}

}  // namespace demo
