#include "HiZDepthPyramid.h"

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

namespace {

VkExtent2D computePyramidExtent(VkExtent2D sourceSize, uint32_t downsampleDivisor)
{
  downsampleDivisor = std::max(downsampleDivisor, 1u);
  return {
      std::max(1u, (sourceSize.width + downsampleDivisor - 1u) / downsampleDivisor),
      std::max(1u, (sourceSize.height + downsampleDivisor - 1u) / downsampleDivisor),
  };
}

uint32_t computeMipCount(VkExtent2D extent, uint32_t minMipSize)
{
  uint32_t maxDimension = std::max(extent.width, extent.height);
  uint32_t mipCount = 0;
  minMipSize = std::max(minMipSize, 1u);
  while(maxDimension >= minMipSize && maxDimension > 0)
  {
    ++mipCount;
    maxDimension >>= 1u;
  }
  return std::max(mipCount, 1u);
}

uint64_t estimateR32PyramidBytes(VkExtent2D extent, uint32_t mipCount)
{
  uint64_t totalBytes = 0;
  uint32_t width = std::max(extent.width, 1u);
  uint32_t height = std::max(extent.height, 1u);
  for(uint32_t mipLevel = 0; mipLevel < mipCount; ++mipLevel)
  {
    totalBytes += static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * sizeof(float);
    width = std::max(1u, width >> 1u);
    height = std::max(1u, height >> 1u);
  }
  return totalBytes;
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

void HiZDepthPyramid::init(rhi::Device& device, VmaAllocator allocator, uint32_t frameCount, VkExtent2D sourceSize)
{
  shutdown();

  m_rhiDevice = &device;
  m_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getBackendDeviceHandle()));
  m_allocator = allocator;
  m_frameCount = std::max(frameCount, 1u);
  m_mobilePolicy = MobilePolicy{
      .downsampleDivisor = 2u,
      .maxMipCount = shaderio::LDepthPyramidMaxMips,
      .minMipSize = 1u,
  };
  m_sourceSize = sourceSize;
  m_size = computePyramidExtent(sourceSize, m_mobilePolicy.downsampleDivisor);
  m_fullMipCount = computeMipCount(m_size, 1u);
  m_mipCount = std::min(computeMipCount(m_size, m_mobilePolicy.minMipSize), m_mobilePolicy.maxMipCount);
  m_estimatedMemoryBytes = estimateR32PyramidBytes(m_size, m_mipCount);

  if(m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE || m_mipCount == 0)
  {
    return;
  }

  m_perFrame.resize(m_frameCount);
  for(PerFrameResources& frameResources : m_perFrame)
  {
    frameResources.uniformBuffer = createUniformBuffer(m_allocator);
    frameResources.uniformBufferHandle =
        m_rhiDevice->registerExternalBuffer(reinterpret_cast<uint64_t>(frameResources.uniformBuffer.buffer));
  }

  const std::array<rhi::ArgumentBinding, 3> bindings{{
      rhi::ArgumentBinding{.binding = 0, .type = rhi::ArgumentType::sampledTexture, .visibility = rhi::ShaderStage::compute, .arrayCount = 1},
      rhi::ArgumentBinding{.binding = 1, .type = rhi::ArgumentType::storageTexture, .visibility = rhi::ShaderStage::compute, .arrayCount = shaderio::LDepthPyramidMaxMips},
      rhi::ArgumentBinding{.binding = 2, .type = rhi::ArgumentType::uniformBuffer, .visibility = rhi::ShaderStage::compute, .arrayCount = 1},
  }};
  m_argumentLayout = m_rhiDevice->createArgumentLayout(rhi::ArgumentLayoutDesc{
      .bindings     = bindings.data(),
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .debugName    = "hiz-depth-pyramid",
  });
  for(uint32_t frameIndex = 0; frameIndex < m_frameCount; ++frameIndex)
  {
    m_perFrame[frameIndex].argumentTable = m_rhiDevice->createArgumentTable(m_argumentLayout);
  }

  const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{m_argumentLayout}};
  const rhi::ComputePipelineDesc pipelineDesc{
      .shaderStage =
          rhi::PipelineShaderStageDesc{
              .stage        = rhi::ShaderStage::compute,
              .spirvCode    = shader_depth_pyramid_slang,
              .spirvSize    = std::size(shader_depth_pyramid_slang) * sizeof(uint32_t),
              .entryPoint   = "depthPyramid",
          },
      .argumentLayouts = argumentLayouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
      .specializationVariant = 0x7101u,
  };
  m_pipeline = m_rhiDevice->createComputePipeline(pipelineDesc);

  recreateResources();
}

void HiZDepthPyramid::shutdown()
{
  destroyImageResources();

  if(!m_pipeline.isNull() && m_rhiDevice != nullptr)
  {
    m_rhiDevice->destroyPipeline(m_pipeline);
    m_pipeline = {};
  }
  for(PerFrameResources& frameResources : m_perFrame)
  {
    if(!frameResources.argumentTable.isNull() && m_rhiDevice != nullptr)
    {
      m_rhiDevice->destroyArgumentTable(frameResources.argumentTable);
      frameResources.argumentTable = {};
    }
    if(!frameResources.uniformBufferHandle.isNull() && m_rhiDevice != nullptr)
    {
      m_rhiDevice->destroyBuffer(frameResources.uniformBufferHandle);
      frameResources.uniformBufferHandle = {};
    }
    if(frameResources.uniformBuffer.buffer != VK_NULL_HANDLE)
    {
      vmaDestroyBuffer(m_allocator, frameResources.uniformBuffer.buffer, frameResources.uniformBuffer.allocation);
      frameResources.uniformBuffer = {};
    }
  }
  if(!m_argumentLayout.isNull() && m_rhiDevice != nullptr)
  {
    m_rhiDevice->destroyArgumentLayout(m_argumentLayout);
    m_argumentLayout = {};
  }
  m_perFrame.clear();

  m_sourceDepth = {};
  m_lastBoundArgumentTable = {};
  m_lastBoundBinding = 0;
  m_generationCount = 0;
  m_estimatedMemoryBytes = 0;
  m_valid = false;
  m_layoutInitialized = false;
  m_frameCount = 0;
  m_mipCount = 0;
  m_fullMipCount = 0;
  m_size = {};
  m_sourceSize = {};
  m_device = VK_NULL_HANDLE;
  m_rhiDevice = nullptr;
  m_allocator = VK_NULL_HANDLE;
}

void HiZDepthPyramid::configureMobilePolicy(MobilePolicy policy)
{
  policy.downsampleDivisor = std::max(policy.downsampleDivisor, 1u);
  policy.maxMipCount = std::max(policy.maxMipCount, 1u);
  policy.minMipSize = std::max(policy.minMipSize, 1u);
  if(policy.downsampleDivisor == m_mobilePolicy.downsampleDivisor
     && policy.maxMipCount == m_mobilePolicy.maxMipCount
     && policy.minMipSize == m_mobilePolicy.minMipSize)
  {
    return;
  }

  m_mobilePolicy = policy;
  const VkExtent2D newSize = computePyramidExtent(m_sourceSize, m_mobilePolicy.downsampleDivisor);
  const uint32_t newFullMipCount = computeMipCount(newSize, 1u);
  const uint32_t newMipCount = std::min(computeMipCount(newSize, m_mobilePolicy.minMipSize), m_mobilePolicy.maxMipCount);
  if(newSize.width != m_size.width || newSize.height != m_size.height || newMipCount != m_mipCount)
  {
    m_size = newSize;
    m_fullMipCount = newFullMipCount;
    m_mipCount = newMipCount;
    m_estimatedMemoryBytes = estimateR32PyramidBytes(m_size, m_mipCount);
    recreateResources();
  }
}

void HiZDepthPyramid::resize(VkExtent2D sourceSize)
{
  if(m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
  {
    return;
  }

  const VkExtent2D newSize = computePyramidExtent(sourceSize, m_mobilePolicy.downsampleDivisor);
  const uint32_t newFullMipCount = computeMipCount(newSize, 1u);
  const uint32_t newMipCount = std::min(computeMipCount(newSize, m_mobilePolicy.minMipSize), m_mobilePolicy.maxMipCount);
  if(sourceSize.width == m_sourceSize.width && sourceSize.height == m_sourceSize.height
     && newSize.width == m_size.width && newSize.height == m_size.height && newMipCount == m_mipCount)
  {
    return;
  }

  m_sourceSize = sourceSize;
  m_size = newSize;
  m_fullMipCount = newFullMipCount;
  m_mipCount = newMipCount;
  m_estimatedMemoryBytes = estimateR32PyramidBytes(m_size, m_mipCount);
  recreateResources();
}

void HiZDepthPyramid::generate(uint32_t frameIndex,
                               rhi::CommandBuffer& rhiCmd,
                               VkExtent2D sourceSize,
                               rhi::TextureViewHandle sourceDepthView,
                               TextureHandle sourceDepth,
                               rhi::TextureHandle sourceDepthRHI)
{
  m_sourceDepth = sourceDepth;
  if(frameIndex >= m_perFrame.size())
  {
    m_valid = false;
    return;
  }
  const PerFrameResources& frameResources = m_perFrame[frameIndex];
  if(sourceDepthRHI.isNull() || sourceDepthView.isNull()
     || m_pipeline.isNull() || frameResources.argumentTable.isNull())
  {
    m_valid = false;
    return;
  }

  resize(sourceSize);
  if(!m_valid || m_image == VK_NULL_HANDLE || m_mipCount == 0)
  {
    return;
  }

  updateArgumentTable(frameIndex, sourceDepthView);

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
    const rhi::TextureBarrier initPyramidBarrier{
        .texture = m_imageHandle,
        .before = rhi::ResourceState::Undefined,
        .after = rhi::ResourceState::General,
        .range = {.aspect = rhi::TextureAspect::color, .baseMipLevel = 0, .levelCount = m_mipCount, .baseArrayLayer = 0, .layerCount = 1},
    };
    rhiCmd.resourceBarrier(&initPyramidBarrier, 1, nullptr, 0);
    m_layoutInitialized = true;
  }

  const rhi::TextureBarrier sourceBarrier{
      .texture = sourceDepthRHI,
      .before = rhi::ResourceState::ShaderRead,
      .after = rhi::ResourceState::ShaderRead,
      .range = {.aspect = rhi::TextureAspect::depth, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };
  rhiCmd.resourceBarrier(&sourceBarrier, 1, nullptr, 0);

  rhi::ComputeEncoder* encoder = rhiCmd.beginComputePass();
  if(encoder == nullptr)
  {
    m_valid = false;
    return;
  }
  encoder->setPipeline(m_pipeline);
  encoder->setArgumentTable(0, frameResources.argumentTable);
  encoder->dispatch(rhi::DispatchDesc{.groupCountX = (m_size.width + 7u) / 8u,
                                      .groupCountY = (m_size.height + 7u) / 8u,
                                      .groupCountZ = 1u});
  rhiCmd.endEncoding();

  const rhi::TextureBarrier pyramidBarrier{
      .texture = m_imageHandle,
      .before = rhi::ResourceState::General,
      .after = rhi::ResourceState::General,
      .range = {.aspect = rhi::TextureAspect::color, .baseMipLevel = 0, .levelCount = std::min<uint32_t>(m_mipCount, shaderio::LDepthPyramidMaxMips), .baseArrayLayer = 0, .layerCount = 1},
  };
  rhiCmd.resourceBarrier(&pyramidBarrier, 1, nullptr, 0);

  ++m_generationCount;
}

void HiZDepthPyramid::markBoundForCulling(rhi::ArgumentTableHandle table, uint32_t binding)
{
  m_lastBoundArgumentTable = table;
  m_lastBoundBinding = binding;
}

rhi::TextureViewHandle HiZDepthPyramid::getMipView(uint32_t mipLevel) const
{
  if(mipLevel >= m_mipViews.size())
  {
    return {};
  }
  return m_mipViews[mipLevel];
}

void HiZDepthPyramid::updateArgumentTable(uint32_t frameIndex, rhi::TextureViewHandle sourceDepthView)
{
  if(frameIndex >= m_perFrame.size() || sourceDepthView.isNull() || m_mipViews.empty())
  {
    return;
  }
  const PerFrameResources& frameResources = m_perFrame[frameIndex];
  if(frameResources.argumentTable.isNull() || frameResources.uniformBufferHandle.isNull())
  {
    return;
  }

  std::array<rhi::ArgumentWrite, shaderio::LDepthPyramidMaxMips + 2> writes{};
  uint32_t writeCount = 0;
  writes[writeCount++] = rhi::ArgumentWrite{
      .binding = 0,
      .type = rhi::ArgumentType::sampledTexture,
      .textureView = sourceDepthView,
      .accessIntent = rhi::ArgumentAccessIntent::sampledRead,
  };
  for(uint32_t i = 0; i < shaderio::LDepthPyramidMaxMips; ++i)
  {
    writes[writeCount++] = rhi::ArgumentWrite{
        .binding = 1,
        .arrayElement = i,
        .type = rhi::ArgumentType::storageTexture,
        .textureView = m_mipViews[std::min<uint32_t>(i, static_cast<uint32_t>(m_mipViews.size() - 1u))],
        .accessIntent = rhi::ArgumentAccessIntent::readWrite,
    };
  }
  writes[writeCount++] = rhi::ArgumentWrite{
      .binding = 2,
      .type = rhi::ArgumentType::uniformBuffer,
      .buffer = frameResources.uniformBufferHandle,
      .size = sizeof(shaderio::DepthPyramidUniforms),
  };
  m_rhiDevice->updateArgumentTable(frameResources.argumentTable, writeCount, writes.data());
}

void HiZDepthPyramid::recreateResources()
{
  destroyImageResources();
  m_sourceDepth = {};
  m_valid = false;
  m_estimatedMemoryBytes = estimateR32PyramidBytes(m_size, m_mipCount);

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
  m_imageHandle = m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(m_image));

  m_mipViews.resize(m_mipCount);
  for(uint32_t mipLevel = 0; mipLevel < m_mipCount; ++mipLevel)
  {
    rhi::TextureViewCreateDesc mipViewDesc{};
    mipViewDesc.image         = m_imageHandle;
    mipViewDesc.format        = rhi::TextureFormat::r32Sfloat;
    mipViewDesc.viewType      = rhi::ImageViewType::e2D;
    mipViewDesc.aspect        = rhi::TextureAspect::color;
    mipViewDesc.baseMipLevel  = mipLevel;
    mipViewDesc.levelCount    = 1;
    m_mipViews[mipLevel] = m_rhiDevice->createTextureView(mipViewDesc);
  }

  m_valid = true;
}

void HiZDepthPyramid::destroyImageResources()
{
  for(rhi::TextureViewHandle mipView : m_mipViews)
  {
    if(m_rhiDevice != nullptr)
    {
      m_rhiDevice->destroyTextureView(mipView);
    }
  }
  m_mipViews.clear();

  if(m_rhiDevice != nullptr && !m_imageHandle.isNull())
  {
    m_rhiDevice->destroyImage(m_imageHandle);
  }
  m_imageHandle = {};

  if(m_image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_image, m_imageAllocation);
    m_image = VK_NULL_HANDLE;
    m_imageAllocation = nullptr;
  }
  m_layoutInitialized = false;
}

}  // namespace demo
