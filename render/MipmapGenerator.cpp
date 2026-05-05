#include "MipmapGenerator.h"

#include <algorithm>

namespace demo {

namespace {

void transitionMip(VkCommandBuffer       cmd,
                   VkImage               image,
                   uint32_t              mipLevel,
                   VkImageLayout         oldLayout,
                   VkImageLayout         newLayout,
                   VkPipelineStageFlags2 srcStageMask,
                   VkAccessFlags2        srcAccessMask,
                   VkPipelineStageFlags2 dstStageMask,
                   VkAccessFlags2        dstAccessMask)
{
  const VkImageMemoryBarrier2 barrier{
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = srcStageMask,
      .srcAccessMask       = srcAccessMask,
      .dstStageMask        = dstStageMask,
      .dstAccessMask       = dstAccessMask,
      .oldLayout           = oldLayout,
      .newLayout           = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = image,
      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, 0, 1},
  };

  const VkDependencyInfo depInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &depInfo);
}

}  // namespace

uint32_t MipmapGenerator::calculateMipLevelCount(uint32_t width, uint32_t height)
{
  uint32_t levels = 1;
  uint32_t maxDim = std::max(width, height);
  while(maxDim > 1)
  {
    maxDim >>= 1;
    ++levels;
  }
  return levels;
}

void MipmapGenerator::generateMipmaps(VkCommandBuffer cmd,
                                      VkImage         image,
                                      VkFormat        format,
                                      uint32_t        width,
                                      uint32_t        height,
                                      uint32_t        mipLevels)
{
  if(image == VK_NULL_HANDLE || mipLevels <= 1)
  {
    return;
  }

  int32_t mipWidth  = static_cast<int32_t>(width);
  int32_t mipHeight = static_cast<int32_t>(height);

  for(uint32_t mip = 0; mip + 1 < mipLevels; ++mip)
  {
    transitionMip(cmd,
                  image,
                  mip,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    blit.srcOffsets[0]  = VkOffset3D{0, 0, 0};
    blit.srcOffsets[1]  = VkOffset3D{mipWidth, mipHeight, 1};
    blit.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 0, 1};
    blit.dstOffsets[0]  = VkOffset3D{0, 0, 0};
    blit.dstOffsets[1]  = VkOffset3D{std::max(1, mipWidth / 2), std::max(1, mipHeight / 2), 1};

    vkCmdBlitImage(cmd,
                   image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   image,
                   VK_IMAGE_LAYOUT_GENERAL,
                   1,
                   &blit,
                   VK_FILTER_LINEAR);

    transitionMip(cmd,
                  image,
                  mip,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);

    mipWidth = std::max(1, mipWidth / 2);
    mipHeight = std::max(1, mipHeight / 2);
  }

  transitionMip(cmd,
                image,
                mipLevels - 1,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);

  (void)format;
}

}  // namespace demo
