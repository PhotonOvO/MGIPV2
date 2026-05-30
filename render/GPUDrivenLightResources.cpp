#include "GPUDrivenLightResources.h"

#include <algorithm>
#include <cstring>

namespace demo {

namespace {

constexpr VkDeviceSize kCoarseBoundsElementSize = sizeof(uint16_t) * 4u;

}  // namespace

void GPUDrivenLightResources::init(VkDevice device, VmaAllocator allocator, const CreateInfo& createInfo)
{
  deinit();

  m_device = device;
  m_allocator = allocator;
  m_maxPointLights = std::max(1u, createInfo.maxPointLights);
  m_maxSpotLights = std::max(1u, createInfo.maxSpotLights);
  m_frames.resize(std::max(1u, createInfo.frameCount));

  const VkDeviceSize pointLightBytes = sizeof(shaderio::LightData) * static_cast<VkDeviceSize>(m_maxPointLights);
  const VkDeviceSize spotLightBytes = sizeof(shaderio::LightData) * static_cast<VkDeviceSize>(m_maxSpotLights);
  const VkDeviceSize pointCoarseBoundsBytes = kCoarseBoundsElementSize * static_cast<VkDeviceSize>(m_maxPointLights);
  const VkDeviceSize spotCoarseBoundsBytes = kCoarseBoundsElementSize * static_cast<VkDeviceSize>(m_maxSpotLights);
  const VkDeviceSize clusterCountBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(shaderio::LClusterCount);
  const VkDeviceSize clusterIndexBytes =
      sizeof(uint32_t) * static_cast<VkDeviceSize>(shaderio::LClusterCount) * shaderio::LMaxLightsPerCluster;

  for(FrameResources& frame : m_frames)
  {
    frame.pointLightBuffer =
        createStorageBuffer(pointLightBytes,
                            VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.spotLightBuffer =
        createStorageBuffer(spotLightBytes,
                            VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.pointCoarseBoundsBuffer = createStorageBuffer(pointCoarseBoundsBytes, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.spotCoarseBoundsBuffer = createStorageBuffer(spotCoarseBoundsBytes, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.lightingUniformBuffer =
        createUniformBuffer(sizeof(shaderio::LightingUniforms),
                            VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.coarseUniformBuffer =
        createUniformBuffer(sizeof(shaderio::LightCoarseCullingUniforms),
                            VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.clusteredUniformBuffer =
        createUniformBuffer(sizeof(shaderio::ClusteredLightUniforms),
                            VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.clusterCountsBuffer = createStorageBuffer(clusterCountBytes, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.clusterIndicesBuffer = createStorageBuffer(clusterIndexBytes, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.clusterStatsBuffer =
        createStorageBuffer(sizeof(ClusterStats),
                            VMA_MEMORY_USAGE_GPU_TO_CPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
}

void GPUDrivenLightResources::deinit()
{
  if(m_allocator != nullptr)
  {
    for(FrameResources& frame : m_frames)
    {
      destroyBuffer(frame.pointLightBuffer);
      destroyBuffer(frame.spotLightBuffer);
      destroyBuffer(frame.pointCoarseBoundsBuffer);
      destroyBuffer(frame.spotCoarseBoundsBuffer);
      destroyBuffer(frame.lightingUniformBuffer);
      destroyBuffer(frame.coarseUniformBuffer);
      destroyBuffer(frame.clusteredUniformBuffer);
      destroyBuffer(frame.clusterCountsBuffer);
      destroyBuffer(frame.clusterIndicesBuffer);
      destroyBuffer(frame.clusterStatsBuffer);
    }
  }

  m_frames.clear();
  m_device = VK_NULL_HANDLE;
  m_allocator = nullptr;
  m_maxPointLights = 256;
  m_maxSpotLights = 128;
  m_activePointLights = 0;
  m_activeSpotLights = 0;
  m_lastClusterStats = {};
}

void GPUDrivenLightResources::updateLights(uint32_t frameIndex,
                                           const std::vector<shaderio::LightData>& pointLights,
                                           const std::vector<shaderio::LightData>& spotLights)
{
  if(frameIndex >= m_frames.size())
  {
    m_activePointLights = 0;
    m_activeSpotLights = 0;
    return;
  }

  FrameResources& frame = m_frames[frameIndex];
  m_activePointLights = std::min<uint32_t>(static_cast<uint32_t>(pointLights.size()), m_maxPointLights);
  m_activeSpotLights = std::min<uint32_t>(static_cast<uint32_t>(spotLights.size()), m_maxSpotLights);
  if(m_activePointLights > 0)
  {
    updateMappedBuffer(frame.pointLightBuffer, pointLights.data(), sizeof(shaderio::LightData) * m_activePointLights);
  }
  if(m_activeSpotLights > 0)
  {
    updateMappedBuffer(frame.spotLightBuffer, spotLights.data(), sizeof(shaderio::LightData) * m_activeSpotLights);
  }
}

void GPUDrivenLightResources::updateUniforms(uint32_t frameIndex,
                                             const shaderio::LightingUniforms& lightingUniforms,
                                             const shaderio::LightCoarseCullingUniforms& coarseUniforms,
                                             const shaderio::ClusteredLightUniforms& clusteredUniforms)
{
  if(frameIndex >= m_frames.size())
  {
    return;
  }

  updateMappedBuffer(m_frames[frameIndex].lightingUniformBuffer, &lightingUniforms, sizeof(lightingUniforms));
  updateMappedBuffer(m_frames[frameIndex].coarseUniformBuffer, &coarseUniforms, sizeof(coarseUniforms));
  updateMappedBuffer(m_frames[frameIndex].clusteredUniformBuffer, &clusteredUniforms, sizeof(clusteredUniforms));
}

VkBuffer GPUDrivenLightResources::getPointLightBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].pointLightBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getSpotLightBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].spotLightBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getPointCoarseBoundsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].pointCoarseBoundsBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getSpotCoarseBoundsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].spotCoarseBoundsBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getCoarseUniformBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].coarseUniformBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getLightingUniformBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].lightingUniformBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getClusteredUniformBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].clusteredUniformBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getClusterCountsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterCountsBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getClusterIndicesBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterIndicesBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer GPUDrivenLightResources::getClusterStatsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterStatsBuffer.buffer : VK_NULL_HANDLE;
}

void GPUDrivenLightResources::cacheClusterStats(uint32_t frameIndex)
{
  if(frameIndex >= m_frames.size())
  {
    m_lastClusterStats = {};
    return;
  }

  const utils::Buffer& buffer = m_frames[frameIndex].clusterStatsBuffer;
  if(buffer.allocation == nullptr || buffer.mapped == nullptr)
  {
    m_lastClusterStats = {};
    return;
  }

  VK_CHECK(vmaInvalidateAllocation(m_allocator, buffer.allocation, 0, sizeof(ClusterStats)));
  std::memcpy(&m_lastClusterStats, buffer.mapped, sizeof(m_lastClusterStats));
}

GPUDrivenLightResources::Diagnostics GPUDrivenLightResources::getDiagnostics() const
{
  const uint64_t clusterCountsBytes = sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount);
  const uint64_t clusterIndicesBytes =
      sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount) * shaderio::LMaxLightsPerCluster;
  const uint64_t clusterStatsBytes = sizeof(ClusterStats);
  const uint64_t lightBytes =
      sizeof(shaderio::LightData) * static_cast<uint64_t>(m_maxPointLights + m_maxSpotLights)
      + kCoarseBoundsElementSize * static_cast<uint64_t>(m_maxPointLights + m_maxSpotLights)
      + sizeof(shaderio::LightingUniforms)
      + sizeof(shaderio::LightCoarseCullingUniforms)
      + sizeof(shaderio::ClusteredLightUniforms);

  const bool hasFrame = !m_frames.empty();
  return Diagnostics{
      .clusterGridX = shaderio::LClusterGridSizeX,
      .clusterGridY = shaderio::LClusterGridSizeY,
      .clusterGridZ = shaderio::LClusterGridSizeZ,
      .clusterCount = shaderio::LClusterCount,
      .maxLightsPerCluster = shaderio::LMaxLightsPerCluster,
      .maxPointLights = m_maxPointLights,
      .maxSpotLights = m_maxSpotLights,
      .activePointLights = m_activePointLights,
      .activeSpotLights = m_activeSpotLights,
      .maxOccupancy = m_lastClusterStats.maxOccupancy,
      .overflowClusterCount = m_lastClusterStats.overflowClusterCount,
      .appendedLightReferences = m_lastClusterStats.appendedLightReferences,
      .clusterMemoryBytes = clusterCountsBytes + clusterIndicesBytes + clusterStatsBytes,
      .lightMemoryBytes = lightBytes,
      .initialized = m_device != VK_NULL_HANDLE && hasFrame,
      .clusteredDescriptorsReady = hasFrame && m_frames.front().clusterCountsBuffer.buffer != VK_NULL_HANDLE
                                   && m_frames.front().clusterIndicesBuffer.buffer != VK_NULL_HANDLE,
      .lightingDescriptorsReady = hasFrame && m_frames.front().pointLightBuffer.buffer != VK_NULL_HANDLE
                                   && m_frames.front().coarseUniformBuffer.buffer != VK_NULL_HANDLE,
  };
}

utils::Buffer GPUDrivenLightResources::createStorageBuffer(VkDeviceSize size,
                                                           VmaMemoryUsage usage,
                                                           VmaAllocationCreateFlags flags) const
{
  const VkBufferCreateInfo bufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = std::max<VkDeviceSize>(size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = usage};
  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;
  return buffer;
}

utils::Buffer GPUDrivenLightResources::createUniformBuffer(VkDeviceSize size,
                                                           VmaMemoryUsage usage,
                                                           VmaAllocationCreateFlags flags) const
{
  const VkBufferCreateInfo bufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = std::max<VkDeviceSize>(size, 16),
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = usage};
  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;
  return buffer;
}

void GPUDrivenLightResources::destroyBuffer(utils::Buffer& buffer) const
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

void GPUDrivenLightResources::updateMappedBuffer(utils::Buffer& buffer, const void* data, VkDeviceSize size) const
{
  if(buffer.buffer == VK_NULL_HANDLE || data == nullptr || size == 0)
  {
    return;
  }

  void* mappedData = buffer.mapped;
  bool mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(m_allocator, buffer.allocation, &mappedData));
    mappedHere = true;
  }
  std::memcpy(mappedData, data, static_cast<size_t>(size));
  VK_CHECK(vmaFlushAllocation(m_allocator, buffer.allocation, 0, size));
  if(mappedHere)
  {
    vmaUnmapMemory(m_allocator, buffer.allocation);
  }
}

}  // namespace demo
