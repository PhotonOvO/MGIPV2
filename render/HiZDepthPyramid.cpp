#include "HiZDepthPyramid.h"

#include "../rhi/vulkan/VulkanCommandList.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

namespace {

VkExtent2D computePyramidExtent(VkExtent2D sourceSize)
{
  return {
      std::max(1u, (sourceSize.width + 1u) / 2u),
      std::max(1u, (sourceSize.height + 1u) / 2u),
  };
}

uint32_t computeMipCount(VkExtent2D extent)
{
  uint32_t maxDimension = std::max(extent.width, extent.height);
  uint32_t mipCount = 0;
  while(maxDimension > 0)
  {
    ++mipCount;
    maxDimension >>= 1u;
  }
  return mipCount;
}

utils::Buffer createUniformBuffer(VmaAllocator allocator)
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
  };

  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = sizeof(shaderio::DepthPyramidUniforms),
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocationCreateInfo{};
  allocationCreateInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  allocationCreateInfo.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocationCreateInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;
  return buffer;
}

}  // namespace

void HiZDepthPyramid::init(VkDevice device, VmaAllocator allocator, uint32_t frameCount, VkExtent2D sourceSize)
{
  shutdown();

  m_device = device;
  m_allocator = allocator;
  m_frameCount = std::max(frameCount, 1u);
  m_size = computePyramidExtent(sourceSize);
  m_mipCount = computeMipCount(m_size);

  if(m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE || m_mipCount == 0)
  {
    return;
  }

  m_perFrame.resize(m_frameCount);
  for(PerFrameResources& frameResources : m_perFrame)
  {
    frameResources.uniformBuffer = createUniformBuffer(m_allocator);
  }

  const VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_frameCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, shaderio::LDepthPyramidMaxMips * m_frameCount},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, m_frameCount},
  };

  const VkDescriptorPoolCreateInfo poolInfo{
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets       = m_frameCount,
      .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
      .pPoolSizes    = poolSizes,
  };
  VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, shaderio::LDepthPyramidMaxMips, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};

  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(m_device, &setLayoutInfo, nullptr, &m_descriptorSetLayout));

  VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_descriptorPool,
      .descriptorSetCount = m_frameCount,
  };
  std::vector<VkDescriptorSetLayout> setLayouts(m_frameCount, m_descriptorSetLayout);
  allocInfo.pSetLayouts = setLayouts.data();
  std::vector<VkDescriptorSet> descriptorSets(m_frameCount, VK_NULL_HANDLE);
  VK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, descriptorSets.data()));
  for(uint32_t frameIndex = 0; frameIndex < m_frameCount; ++frameIndex)
  {
    m_perFrame[frameIndex].descriptorSet = descriptorSets[frameIndex];
  }

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts    = &m_descriptorSetLayout,
  };
  VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

  const VkShaderModule shaderModule =
      utils::createShaderModule(m_device, {shader_depth_pyramid_slang, std::size(shader_depth_pyramid_slang)});
  const VkPipelineShaderStageCreateInfo shaderStageInfo{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "depthPyramid",
  };

  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStageInfo,
      .layout = m_pipelineLayout,
  };
  VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline));
  vkDestroyShaderModule(m_device, shaderModule, nullptr);

  recreateResources();
}

void HiZDepthPyramid::shutdown()
{
  destroyImageResources();

  if(m_pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
  }
  if(m_pipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
  }
  if(m_descriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
  }
  if(m_descriptorSetLayout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    m_descriptorSetLayout = VK_NULL_HANDLE;
  }
  for(PerFrameResources& frameResources : m_perFrame)
  {
    if(frameResources.uniformBuffer.buffer != VK_NULL_HANDLE)
    {
      vmaDestroyBuffer(m_allocator, frameResources.uniformBuffer.buffer, frameResources.uniformBuffer.allocation);
      frameResources.uniformBuffer = {};
    }
    frameResources.descriptorSet = VK_NULL_HANDLE;
  }
  m_perFrame.clear();

  m_sourceDepth = {};
  m_lastBoundSet = VK_NULL_HANDLE;
  m_lastBoundBinding = 0;
  m_generationCount = 0;
  m_valid = false;
  m_layoutInitialized = false;
  m_frameCount = 0;
  m_mipCount = 0;
  m_size = {};
  m_device = VK_NULL_HANDLE;
  m_allocator = VK_NULL_HANDLE;
}

void HiZDepthPyramid::resize(VkExtent2D sourceSize)
{
  if(m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
  {
    return;
  }

  const VkExtent2D newSize = computePyramidExtent(sourceSize);
  const uint32_t newMipCount = computeMipCount(newSize);
  if(newSize.width == m_size.width && newSize.height == m_size.height && newMipCount == m_mipCount)
  {
    return;
  }

  m_size = newSize;
  m_mipCount = newMipCount;
  recreateResources();
}

void HiZDepthPyramid::generate(uint32_t frameIndex,
                               VkCommandBuffer cmd,
                               VkExtent2D sourceSize,
                               VkImage sourceDepthImage,
                               VkImageView sourceDepthView,
                               TextureHandle sourceDepth)
{
  m_sourceDepth = sourceDepth;
  if(frameIndex >= m_perFrame.size())
  {
    m_valid = false;
    return;
  }
  const PerFrameResources& frameResources = m_perFrame[frameIndex];
  if(cmd == VK_NULL_HANDLE || sourceDepthImage == VK_NULL_HANDLE || sourceDepthView == VK_NULL_HANDLE
     || m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE || frameResources.descriptorSet == VK_NULL_HANDLE)
  {
    m_valid = false;
    return;
  }

  resize(sourceSize);
  if(!m_valid || m_image == VK_NULL_HANDLE || m_mipCount == 0)
  {
    return;
  }

  updateDescriptorSet(frameIndex, sourceDepthView);

  const shaderio::DepthPyramidUniforms uniforms{
      .sourceWidth   = sourceSize.width,
      .sourceHeight  = sourceSize.height,
      ._padding0     = 0u,
      ._padding1     = 0u,
      .pyramidWidth  = m_size.width,
      .pyramidHeight = m_size.height,
      .mipCount      = std::min<uint32_t>(m_mipCount, shaderio::LDepthPyramidMaxMips),
      ._padding2     = 0u,
  };

  std::memcpy(frameResources.uniformBuffer.mapped, &uniforms, sizeof(uniforms));
  VK_CHECK(vmaFlushAllocation(m_allocator, frameResources.uniformBuffer.allocation, 0, sizeof(uniforms)));

  if(!m_layoutInitialized)
  {
    const VkImageMemoryBarrier2 initPyramidBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = m_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = m_mipCount,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const VkDependencyInfo initPyramidDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &initPyramidBarrier,
    };
    vkCmdPipelineBarrier2(cmd, &initPyramidDependency);
    m_layoutInitialized = true;
  }

  const VkImageMemoryBarrier2 sourceBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
      .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = sourceDepthImage,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
      },
  };

  const VkDependencyInfo dependencyInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &sourceBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &frameResources.descriptorSet, 0, nullptr);
  vkCmdDispatch(cmd, (m_size.width + 7u) / 8u, (m_size.height + 7u) / 8u, 1u);

  const VkImageMemoryBarrier2 pyramidBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = m_image,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel = 0,
          .levelCount = std::min<uint32_t>(m_mipCount, shaderio::LDepthPyramidMaxMips),
          .baseArrayLayer = 0,
          .layerCount = 1,
      },
  };
  const VkDependencyInfo pyramidDependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &pyramidBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &pyramidDependency);

  ++m_generationCount;
}

void HiZDepthPyramid::bindForCulling(VkDescriptorSet set, uint32_t binding)
{
  m_lastBoundSet = set;
  m_lastBoundBinding = binding;
  if(m_device == VK_NULL_HANDLE || set == VK_NULL_HANDLE || m_mipViews.empty())
  {
    return;
  }

  std::array<VkDescriptorImageInfo, shaderio::LDepthPyramidMaxMips> pyramidMipInfos{};
  for(uint32_t i = 0; i < static_cast<uint32_t>(pyramidMipInfos.size()); ++i)
  {
    pyramidMipInfos[i] = VkDescriptorImageInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = m_mipViews[std::min<uint32_t>(i, static_cast<uint32_t>(m_mipViews.size() - 1u))],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
  }

  const VkWriteDescriptorSet write{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .dstArrayElement = 0,
      .descriptorCount = static_cast<uint32_t>(pyramidMipInfos.size()),
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .pImageInfo = pyramidMipInfos.data(),
  };
  vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

VkImageView HiZDepthPyramid::getMipView(uint32_t mipLevel) const
{
  if(mipLevel >= m_mipViews.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_mipViews[mipLevel];
}

void HiZDepthPyramid::updateDescriptorSet(uint32_t frameIndex, VkImageView sourceDepthView)
{
  if(frameIndex >= m_perFrame.size() || sourceDepthView == VK_NULL_HANDLE || m_mipViews.empty())
  {
    return;
  }
  const PerFrameResources& frameResources = m_perFrame[frameIndex];
  if(frameResources.descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  const VkDescriptorImageInfo sourceDepthInfo{
      .sampler = VK_NULL_HANDLE,
      .imageView = sourceDepthView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  std::array<VkDescriptorImageInfo, shaderio::LDepthPyramidMaxMips> pyramidMipInfos{};
  for(uint32_t i = 0; i < static_cast<uint32_t>(pyramidMipInfos.size()); ++i)
  {
    pyramidMipInfos[i] = VkDescriptorImageInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = m_mipViews[std::min<uint32_t>(i, static_cast<uint32_t>(m_mipViews.size() - 1u))],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
  }

  const VkDescriptorBufferInfo uniformInfo{
      .buffer = frameResources.uniformBuffer.buffer,
      .offset = 0,
      .range = sizeof(shaderio::DepthPyramidUniforms),
  };

  const std::array<VkWriteDescriptorSet, 3> writes{{
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSet,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo = &sourceDepthInfo,
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSet,
          .dstBinding = 1,
          .descriptorCount = static_cast<uint32_t>(pyramidMipInfos.size()),
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = pyramidMipInfos.data(),
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSet,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &uniformInfo,
      },
  }};

  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void HiZDepthPyramid::recreateResources()
{
  destroyImageResources();
  m_sourceDepth = {};
  m_valid = false;

  if(m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE || m_mipCount == 0
     || m_size.width == 0 || m_size.height == 0)
  {
    return;
  }

  const VkImageCreateInfo imageInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32_SFLOAT,
      .extent = {m_size.width, m_size.height, 1},
      .mipLevels = m_mipCount,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VmaAllocationCreateInfo allocationInfo{};
  allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &m_image, &m_imageAllocation, nullptr));

  m_mipViews.resize(m_mipCount, VK_NULL_HANDLE);
  for(uint32_t mipLevel = 0; mipLevel < m_mipCount; ++mipLevel)
  {
    const VkImageViewCreateInfo mipViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = mipLevel,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VK_CHECK(vkCreateImageView(m_device, &mipViewInfo, nullptr, &m_mipViews[mipLevel]));
  }

  m_valid = true;
}

void HiZDepthPyramid::destroyImageResources()
{
  for(VkImageView mipView : m_mipViews)
  {
    if(mipView != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, mipView, nullptr);
    }
  }
  m_mipViews.clear();

  if(m_image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_image, m_imageAllocation);
    m_image = VK_NULL_HANDLE;
    m_imageAllocation = nullptr;
  }
  m_layoutInitialized = false;
}

}  // namespace demo
