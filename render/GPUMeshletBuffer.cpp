#include "GPUMeshletBuffer.h"

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
  m_device = VK_NULL_HANDLE;
  m_allocator = nullptr;
}

void GPUMeshletBuffer::clear()
{
  destroyBuffer(m_meshletDataBuffer);
  destroyBuffer(m_meshletVertexBuffer);
  destroyBuffer(m_meshletIndexBuffer);
  m_meshletCount = 0;
}

void GPUMeshletBuffer::uploadMeshlets(VkCommandBuffer cmd,
                                      const std::vector<shaderio::Meshlet>& meshlets,
                                      const std::vector<uint32_t>& meshletIndices)
{
  (void)cmd;
  clear();
  m_meshletCount = static_cast<uint32_t>(meshlets.size());
  if(meshlets.empty())
  {
    return;
  }

  const VkDeviceSize meshletBytes = sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(meshlets.size());
  const VkDeviceSize indexBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(meshletIndices.size());

  m_meshletDataBuffer = createBuffer(m_device,
                                     m_allocator,
                                     meshletBytes,
                                     VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                                     VMA_MEMORY_USAGE_CPU_TO_GPU,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  std::memcpy(m_meshletDataBuffer.mapped, meshlets.data(), static_cast<size_t>(meshletBytes));
  VK_CHECK(vmaFlushAllocation(m_allocator, m_meshletDataBuffer.allocation, 0, meshletBytes));

  if(indexBytes > 0)
  {
    m_meshletIndexBuffer = createBuffer(m_device,
                                        m_allocator,
                                        indexBytes,
                                        VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                                        VMA_MEMORY_USAGE_CPU_TO_GPU,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    std::memcpy(m_meshletIndexBuffer.mapped, meshletIndices.data(), static_cast<size_t>(indexBytes));
    VK_CHECK(vmaFlushAllocation(m_allocator, m_meshletIndexBuffer.allocation, 0, indexBytes));
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
