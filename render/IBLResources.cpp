#include "IBLResources.h"
#include <cmath>

namespace demo {

void IBLResources::init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  m_device = device;
  m_allocator = allocator;

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();

  // Create cube map sampler (trilinear for mip sampling)
  const VkSamplerCreateInfo cubeSamplerInfo{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = VK_LOD_CLAMP_NONE,
  };
  VK_CHECK(vkCreateSampler(m_device, &cubeSamplerInfo, nullptr, &m_cubeMapSampler));
  dutil.setObjectName(m_cubeMapSampler, "IBL_CubeMapSampler");

  // Create LUT sampler (bilinear, clamp)
  const VkSamplerCreateInfo lutSamplerInfo{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  };
  VK_CHECK(vkCreateSampler(m_device, &lutSamplerInfo, nullptr, &m_lutSampler));
  dutil.setObjectName(m_lutSampler, "IBL_LUTSampler");

  // Create prefiltered environment cube map
  // 6 faces, with mip chain for roughness levels
  m_maxMipLevel = static_cast<uint32_t>(std::floor(std::log2(createInfo.cubeMapSize)));

  const VkImageCreateInfo cubeInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.cubeMapFormat,
      .extent = {createInfo.cubeMapSize, createInfo.cubeMapSize, 1},
      .mipLevels = m_maxMipLevel + 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateImage(m_allocator, &cubeInfo, &imageAllocInfo,
      &m_prefilteredMap.image, &m_prefilteredMap.allocation, nullptr));
  dutil.setObjectName(m_prefilteredMap.image, "IBL_PrefilteredMap");

  // Create cube map view
  const VkImageViewCreateInfo cubeViewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_prefilteredMap.image,
      .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
      .format = createInfo.cubeMapFormat,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel = 0,
          .levelCount = m_maxMipLevel + 1,
          .baseArrayLayer = 0,
          .layerCount = 6,
      },
  };
  VK_CHECK(vkCreateImageView(m_device, &cubeViewInfo, nullptr, &m_prefilteredMapView));
  dutil.setObjectName(m_prefilteredMapView, "IBL_PrefilteredMapView");

  // Create irradiance map (smaller, single mip)
  const VkImageCreateInfo irradianceInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.cubeMapFormat,
      .extent = {32, 32, 1},  // Low resolution for diffuse irradiance
      .mipLevels = 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VK_CHECK(vmaCreateImage(m_allocator, &irradianceInfo, &imageAllocInfo,
      &m_irradianceMap.image, &m_irradianceMap.allocation, nullptr));
  dutil.setObjectName(m_irradianceMap.image, "IBL_IrradianceMap");

  const VkImageViewCreateInfo irradianceViewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_irradianceMap.image,
      .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
      .format = createInfo.cubeMapFormat,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .levelCount = 1,
          .layerCount = 6,
      },
  };
  VK_CHECK(vkCreateImageView(m_device, &irradianceViewInfo, nullptr, &m_irradianceMapView));
  dutil.setObjectName(m_irradianceMapView, "IBL_IrradianceMapView");

  // Create DFG LUT texture
  const VkImageCreateInfo lutInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.dfgLUTFormat,
      .extent = {createInfo.dfgLUTSize, createInfo.dfgLUTSize, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VK_CHECK(vmaCreateImage(m_allocator, &lutInfo, &imageAllocInfo,
      &m_dfgLUT.image, &m_dfgLUT.allocation, nullptr));
  dutil.setObjectName(m_dfgLUT.image, "IBL_DFGLUT");

  const VkImageViewCreateInfo lutViewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_dfgLUT.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = createInfo.dfgLUTFormat,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .levelCount = 1,
          .layerCount = 1,
      },
  };
  VK_CHECK(vkCreateImageView(m_device, &lutViewInfo, nullptr, &m_dfgLUTView));
  dutil.setObjectName(m_dfgLUTView, "IBL_DFGLUTView");

  // Initialize images to GENERAL layout and clear
  utils::cmdInitImageLayout(cmd, m_prefilteredMap.image);
  const VkClearColorValue cubeClearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
  const VkImageSubresourceRange cubeRange{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = m_maxMipLevel + 1,
      .baseArrayLayer = 0,
      .layerCount = 6,
  };
  vkCmdClearColorImage(cmd, m_prefilteredMap.image, VK_IMAGE_LAYOUT_GENERAL, &cubeClearValue, 1, &cubeRange);

  utils::cmdInitImageLayout(cmd, m_irradianceMap.image);
  const VkImageSubresourceRange irradianceRange{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 6,
  };
  vkCmdClearColorImage(cmd, m_irradianceMap.image, VK_IMAGE_LAYOUT_GENERAL, &cubeClearValue, 1, &irradianceRange);

  utils::cmdInitImageLayout(cmd, m_dfgLUT.image);
  const VkClearColorValue lutClearValue = {{0.5f, 0.5f, 0.0f, 1.0f}};  // Neutral DFG values
  const VkImageSubresourceRange lutRange{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
  };
  vkCmdClearColorImage(cmd, m_dfgLUT.image, VK_IMAGE_LAYOUT_GENERAL, &lutClearValue, 1, &lutRange);

  // Note: Actual texture data upload would happen here when loading from HDR/KTX files
  // For now, images are allocated but empty (would need proper IBL generation pass)
}

void IBLResources::deinit()
{
  if(m_prefilteredMapView != VK_NULL_HANDLE)
    vkDestroyImageView(m_device, m_prefilteredMapView, nullptr);
  if(m_irradianceMapView != VK_NULL_HANDLE)
    vkDestroyImageView(m_device, m_irradianceMapView, nullptr);
  if(m_dfgLUTView != VK_NULL_HANDLE)
    vkDestroyImageView(m_device, m_dfgLUTView, nullptr);
  if(m_cubeMapSampler != VK_NULL_HANDLE)
    vkDestroySampler(m_device, m_cubeMapSampler, nullptr);
  if(m_lutSampler != VK_NULL_HANDLE)
    vkDestroySampler(m_device, m_lutSampler, nullptr);

  if(m_prefilteredMap.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_prefilteredMap.image, m_prefilteredMap.allocation);
  if(m_irradianceMap.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_irradianceMap.image, m_irradianceMap.allocation);
  if(m_dfgLUT.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_dfgLUT.image, m_dfgLUT.allocation);

  *this = IBLResources{};
}

}  // namespace demo