#include "GPUMeshletBuffer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace demo {

namespace {

utils::Buffer createBuffer(VkDevice device,
                           VmaAllocator allocator,
                           VkDeviceSize size,
                           VkBufferUsageFlags2KHR usage,
                           VmaMemoryUsage memoryUsage,
                           VmaAllocationCreateFlags flags)
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };
  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};
  if((allocInfo.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) != 0)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;

  const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.buffer};
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  return buffer;
}

}  // namespace

void GPUMeshletBuffer::init(VkDevice device, VmaAllocator allocator)
{
  m_device = device;
  m_allocator = allocator;
}

void GPUMeshletBuffer::deinit()
{
  destroyBuffer(m_meshletDataBuffer);
  destroyBuffer(m_meshletVertexBuffer);
  destroyBuffer(m_meshletIndexBuffer);
  m_meshletCount = 0;
  m_meshletIndexCount = 0;
  m_meshletCapacity = 0;
  m_meshletIndexCapacity = 0;
  m_device = VK_NULL_HANDLE;
  m_allocator = nullptr;
}

void GPUMeshletBuffer::clear()
{
  destroyBuffer(m_meshletDataBuffer);
  destroyBuffer(m_meshletVertexBuffer);
  destroyBuffer(m_meshletIndexBuffer);
  m_meshletCount = 0;
  m_meshletIndexCount = 0;
  m_meshletCapacity = 0;
  m_meshletIndexCapacity = 0;
}

void GPUMeshletBuffer::uploadMeshlets(VkCommandBuffer cmd,
                                      const std::vector<shaderio::Meshlet>& meshlets,
                                      const std::vector<uint32_t>& meshletIndices)
{
  (void)cmd;
  const uint32_t newMeshletCount = static_cast<uint32_t>(meshlets.size());
  const uint32_t newIndexCount = static_cast<uint32_t>(meshletIndices.size());
  const uint32_t previousMeshletCount = m_meshletCount;
  const uint32_t previousIndexCount = m_meshletIndexCount;
  const bool capacityGrowth =
      newMeshletCount > m_meshletCapacity || (newIndexCount > 0 && newIndexCount > m_meshletIndexCapacity);
  const bool rewriteAll = capacityGrowth || newMeshletCount < m_meshletCount || newIndexCount < m_meshletIndexCount;
  if(rewriteAll)
  {
    clear();
  }

  m_meshletCount = newMeshletCount;
  m_meshletIndexCount = newIndexCount;
  if(meshlets.empty())
  {
    return;
  }

  ensureCapacities(newMeshletCount, newIndexCount);

  const uint32_t meshletStart = rewriteAll ? 0u : std::min(previousMeshletCount, newMeshletCount);
  const uint32_t meshletUploadCount = newMeshletCount - meshletStart;
  if(meshletUploadCount > 0)
  {
    const VkDeviceSize meshletOffsetBytes =
        sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(meshletStart);
    const VkDeviceSize meshletUploadBytes =
        sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(meshletUploadCount);
    std::memcpy(static_cast<std::byte*>(m_meshletDataBuffer.mapped) + meshletOffsetBytes,
                meshlets.data() + meshletStart,
                static_cast<size_t>(meshletUploadBytes));
    VK_CHECK(vmaFlushAllocation(m_allocator, m_meshletDataBuffer.allocation, meshletOffsetBytes, meshletUploadBytes));
  }

  const uint32_t indexStart = rewriteAll ? 0u : std::min(previousIndexCount, newIndexCount);
  const uint32_t indexUploadCount = newIndexCount - indexStart;
  if(indexUploadCount > 0)
  {
    const VkDeviceSize indexOffsetBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(indexStart);
    const VkDeviceSize indexUploadBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(indexUploadCount);
    std::memcpy(static_cast<std::byte*>(m_meshletIndexBuffer.mapped) + indexOffsetBytes,
                meshletIndices.data() + indexStart,
                static_cast<size_t>(indexUploadBytes));
    VK_CHECK(vmaFlushAllocation(m_allocator, m_meshletIndexBuffer.allocation, indexOffsetBytes, indexUploadBytes));
  }
}

void GPUMeshletBuffer::ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount)
{
  if(requiredMeshletCount > m_meshletCapacity)
  {
    destroyBuffer(m_meshletDataBuffer);
    m_meshletCapacity = std::max(requiredMeshletCount, std::max(64u, m_meshletCapacity * 2u));
    const VkDeviceSize meshletBytes = sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(m_meshletCapacity);
    m_meshletDataBuffer = createBuffer(m_device,
                                       m_allocator,
                                       meshletBytes,
                                       VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                                       VMA_MEMORY_USAGE_CPU_TO_GPU,
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  }

  if(requiredIndexCount > 0 && requiredIndexCount > m_meshletIndexCapacity)
  {
    destroyBuffer(m_meshletIndexBuffer);
    m_meshletIndexCapacity = std::max(requiredIndexCount, std::max(128u, m_meshletIndexCapacity * 2u));
    const VkDeviceSize indexBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(m_meshletIndexCapacity);
    m_meshletIndexBuffer = createBuffer(m_device,
                                        m_allocator,
                                        indexBytes,
                                        VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                                        VMA_MEMORY_USAGE_CPU_TO_GPU,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  }
}

void GPUMeshletBuffer::destroyBuffer(utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

}  // namespace demo
