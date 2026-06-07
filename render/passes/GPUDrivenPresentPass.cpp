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
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenPresent");
  const rhi::Extent2D srcExtent = m_renderer->getSceneViewDepthExtent();
  const rhi::Extent2D dstExtent = m_renderer->getSwapchainExtent();
  // Wave 6: blit through the registry. Both images are color, single mip/layer, so
  // the resourceBarrier aspect/range pitfalls that block depth/array textures do not
  // apply here. If either handle is unresolved, the pass exits without recording a blit.
  const rhi::TextureHandle srcTex = m_renderer->getPassOutputTextureRHIHandle();
  const rhi::TextureHandle dstTex = m_renderer->getCurrentSwapchainTextureRHIHandle();
  if(srcTex.isNull() || dstTex.isNull())
  {
    context.commandBuffer->endEvent();
    return;
  }

  const float srcAspect = static_cast<float>(srcExtent.width) / static_cast<float>(srcExtent.height);
  const float dstAspect = static_cast<float>(dstExtent.width) / static_cast<float>(dstExtent.height);
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

  // Special resource boundary: present blit needs explicit source/destination
  // transfer layouts; this is an explicit layout boundary.
  const rhi::TextureSubresourceRange colorRange{rhi::TextureAspect::color, 0, 1, 0, 1};
  const rhi::TextureBarrier toBlit[] = {
      {.texture = srcTex, .before = rhi::ResourceState::General, .after = rhi::ResourceState::TransferSrc, .range = colorRange},
      {.texture = dstTex, .before = rhi::ResourceState::General, .after = rhi::ResourceState::TransferDst, .range = colorRange},
  };
  context.commandBuffer->resourceBarrier(toBlit, 2, nullptr, 0);

  rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
  enc->blitTexture(rhi::TextureBlitDesc{
      .srcTexture = srcTex,
      .dstTexture = dstTex,
      .aspect     = rhi::TextureAspect::color,
      .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1}},
      .dstOffsets = {{dstX0, dstY0, 0}, {dstX1, dstY1, 1}},
  });
  context.commandBuffer->endEncoding();

  // Special resource boundary: restore blit source/destination to General before
  // the subsequent ImGui/present path uses the swapchain.
  const rhi::TextureBarrier fromBlit[] = {
      {.texture = dstTex, .before = rhi::ResourceState::TransferDst, .after = rhi::ResourceState::General, .range = colorRange},
      {.texture = srcTex, .before = rhi::ResourceState::TransferSrc, .after = rhi::ResourceState::General, .range = colorRange},
  };
  context.commandBuffer->resourceBarrier(fromBlit, 2, nullptr, 0);

  // ImGui UI pass is a native-backend exception: beginPresentPass sets up dynamic
  // rendering to the swapchain for the ImGui draw path.
  m_renderer->beginPresentPass(*context.commandBuffer);
  context.commandBuffer->endEvent();
}

}  // namespace demo
