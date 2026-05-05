#include "LightResources.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace demo {

namespace {

constexpr VkDeviceSize kCoarseBoundsElementSize = sizeof(uint16_t) * 4;

utils::Buffer createStorageBuffer(VkDevice device,
                                  VmaAllocator allocator,
                                  VkDeviceSize size,
                                  VmaMemoryUsage memoryUsage,
                                  VmaAllocationCreateFlags flags = {})
{
  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size        = std::max<VkDeviceSize>(size, 16),
      .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo allocInfo{
      .flags = flags,
      .usage = memoryUsage,
  };

  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;
  return buffer;
}

utils::Buffer createUniformBuffer(VkDevice device,
                                  VmaAllocator allocator,
                                  VkDeviceSize size,
                                  VmaMemoryUsage memoryUsage,
                                  VmaAllocationCreateFlags flags = {})
{
  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size        = std::max<VkDeviceSize>(size, 16),
      .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo allocInfo{
      .flags = flags,
      .usage = memoryUsage,
  };

  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;
  return buffer;
}

void destroyBuffer(VmaAllocator allocator, utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

void updateMappedStorageBuffer(VmaAllocator allocator, utils::Buffer& buffer, std::span<const shaderio::LightData> lights)
{
  if(buffer.buffer == VK_NULL_HANDLE || lights.empty())
  {
    return;
  }

  const VkDeviceSize copySize = sizeof(shaderio::LightData) * lights.size();
  void* mappedData = buffer.mapped;
  bool  mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(allocator, buffer.allocation, &mappedData));
    mappedHere = true;
  }
  std::memcpy(mappedData, lights.data(), static_cast<size_t>(copySize));
  VK_CHECK(vmaFlushAllocation(allocator, buffer.allocation, 0, copySize));
  if(mappedHere)
  {
    vmaUnmapMemory(allocator, buffer.allocation);
  }
}

void updateMappedUniformBuffer(VmaAllocator allocator, utils::Buffer& buffer, const shaderio::LightCoarseCullingUniforms& uniforms)
{
  if(buffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  void* mappedData = buffer.mapped;
  bool  mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(allocator, buffer.allocation, &mappedData));
    mappedHere = true;
  }
  std::memcpy(mappedData, &uniforms, sizeof(uniforms));
  VK_CHECK(vmaFlushAllocation(allocator, buffer.allocation, 0, sizeof(uniforms)));
  if(mappedHere)
  {
    vmaUnmapMemory(allocator, buffer.allocation);
  }
}

}  // namespace

void LightResources::init(rhi::Device& device, VmaAllocator allocator, const CreateInfo& createInfo)
{
  deinit();

  m_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getNativeDevice()));
  m_allocator = allocator;
  m_maxPointLights = std::max(1u, createInfo.maxPointLights);
  m_maxSpotLights = std::max(1u, createInfo.maxSpotLights);
  m_frames.resize(std::max(1u, createInfo.frameCount));

  const VkDeviceSize pointLightBufferSize = sizeof(shaderio::LightData) * m_maxPointLights;
  const VkDeviceSize spotLightBufferSize = sizeof(shaderio::LightData) * m_maxSpotLights;
  const VkDeviceSize pointBoundsBufferSize = kCoarseBoundsElementSize * m_maxPointLights;
  const VkDeviceSize spotBoundsBufferSize = kCoarseBoundsElementSize * m_maxSpotLights;
  const VkDeviceSize coarseUniformBufferSize = sizeof(shaderio::LightCoarseCullingUniforms);

  for(FrameResources& frame : m_frames)
  {
    frame.pointLightBuffer =
        createStorageBuffer(m_device, m_allocator, pointLightBufferSize, VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.spotLightBuffer =
        createStorageBuffer(m_device, m_allocator, spotLightBufferSize, VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.pointCoarseBoundsBuffer =
        createStorageBuffer(m_device, m_allocator, pointBoundsBufferSize, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.spotCoarseBoundsBuffer =
        createStorageBuffer(m_device, m_allocator, spotBoundsBufferSize, VMA_MEMORY_USAGE_GPU_ONLY);
    frame.coarseCullingUniformBuffer =
        createUniformBuffer(m_device, m_allocator, coarseUniformBufferSize, VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
}

void LightResources::deinit()
{
  if(m_allocator != nullptr)
  {
    for(FrameResources& frame : m_frames)
    {
      destroyBuffer(m_allocator, frame.pointLightBuffer);
      destroyBuffer(m_allocator, frame.spotLightBuffer);
      destroyBuffer(m_allocator, frame.pointCoarseBoundsBuffer);
      destroyBuffer(m_allocator, frame.spotCoarseBoundsBuffer);
      destroyBuffer(m_allocator, frame.coarseCullingUniformBuffer);
    }
  }

  m_frames.clear();
  m_device = VK_NULL_HANDLE;
  m_allocator = nullptr;
  m_maxPointLights = 256;
  m_maxSpotLights = 128;
}

void LightResources::updatePointLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights)
{
  if(frameIndex >= m_frames.size())
  {
    return;
  }
  const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), m_maxPointLights);
  updateMappedStorageBuffer(m_allocator, m_frames[frameIndex].pointLightBuffer, std::span(lights.data(), count));
}

void LightResources::updateSpotLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights)
{
  if(frameIndex >= m_frames.size())
  {
    return;
  }
  const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), m_maxSpotLights);
  updateMappedStorageBuffer(m_allocator, m_frames[frameIndex].spotLightBuffer, std::span(lights.data(), count));
}

void LightResources::updateCoarseCullingUniforms(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms)
{
  if(frameIndex >= m_frames.size())
  {
    return;
  }
  updateMappedUniformBuffer(m_allocator, m_frames[frameIndex].coarseCullingUniformBuffer, uniforms);
}

VkBuffer LightResources::getPointLightBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].pointLightBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer LightResources::getSpotLightBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].spotLightBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer LightResources::getPointCoarseBoundsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].pointCoarseBoundsBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer LightResources::getSpotCoarseBoundsBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].spotCoarseBoundsBuffer.buffer : VK_NULL_HANDLE;
}

VkBuffer LightResources::getCoarseCullingUniformBuffer(uint32_t frameIndex) const
{
  return frameIndex < m_frames.size() ? m_frames[frameIndex].coarseCullingUniformBuffer.buffer : VK_NULL_HANDLE;
}

}  // namespace demo
