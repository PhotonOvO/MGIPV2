#include "GPUDrivenPresentPass.h"

#include "../GPUDrivenRenderer.h"

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

  context.cmdBuffer->beginEvent("GPUDrivenPresent");
  const VkExtent2D srcExtent = m_renderer->getSceneViewDepthExtent();
  const VkExtent2D dstExtent = m_renderer->getSwapchainExtent();
  const VkImage srcImage = reinterpret_cast<VkImage>(m_renderer->getSceneViewOutputImageOpaque());
  const VkImage dstImage = m_renderer->getCurrentSwapchainImage();
  // Wave 6: blit through the registry. Both images are color, single mip/layer, so
  // the resourceBarrier aspect/range pitfalls that block depth/array textures do not
  // apply here. Falls back to the legacy native path if either handle is unresolved.
  const rhi::TextureHandle srcTex = m_renderer->getPassOutputTextureRHIHandle();
  const rhi::TextureHandle dstTex = m_renderer->getCurrentSwapchainTextureRHIHandle();
  if(srcImage == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE || srcTex.isNull() || dstTex.isNull())
  {
    context.cmdBuffer->endEvent();
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

  // Output -> TransferSrc, swapchain -> TransferDst (explicit layout, behavior
  // equivalent to the prior transitionTexture pair; single color mip/layer).
  const rhi::TextureSubresourceRange colorRange{rhi::TextureAspect::color, 0, 1, 0, 1};
  const rhi::TextureBarrier toBlit[] = {
      {.texture = srcTex, .before = rhi::ResourceState::General, .after = rhi::ResourceState::TransferSrc, .range = colorRange},
      {.texture = dstTex, .before = rhi::ResourceState::General, .after = rhi::ResourceState::TransferDst, .range = colorRange},
  };
  context.cmdBuffer->resourceBarrier(toBlit, 2, nullptr, 0);

  rhi::ComputeEncoder* enc = context.cmdBuffer->beginComputePass();
  enc->blitTexture(rhi::TextureBlitDesc{
      .srcTexture = srcTex,
      .dstTexture = dstTex,
      .aspect     = rhi::TextureAspect::color,
      .srcOffsets = {{srcOffset0.x, srcOffset0.y, srcOffset0.z}, {srcOffset1.x, srcOffset1.y, srcOffset1.z}},
      .dstOffsets = {{dstOffset0.x, dstOffset0.y, dstOffset0.z}, {dstOffset1.x, dstOffset1.y, dstOffset1.z}},
  });
  context.cmdBuffer->endEncoding();

  // Restore both images to General (swapchain stays General for the ImGui pass).
  const rhi::TextureBarrier fromBlit[] = {
      {.texture = dstTex, .before = rhi::ResourceState::TransferDst, .after = rhi::ResourceState::General, .range = colorRange},
      {.texture = srcTex, .before = rhi::ResourceState::TransferSrc, .after = rhi::ResourceState::General, .range = colorRange},
  };
  context.cmdBuffer->resourceBarrier(fromBlit, 2, nullptr, 0);

  // ImGui UI pass is a native-backend exception: beginPresentPass sets up dynamic
  // rendering to the swapchain through the legacy CommandList for the ImGui draw path.
  m_renderer->beginPresentPass(*context.cmd);
  context.cmdBuffer->endEvent();
}

}  // namespace demo
