#include "TransientAllocator.h"

namespace demo {

namespace {

[[nodiscard]] uint32_t alignUp(uint32_t value, uint32_t alignment)
{
  const uint32_t safeAlignment = alignment == 0 ? 1u : alignment;
  const uint32_t mask          = safeAlignment - 1u;
  return (value + mask) & ~mask;
}

}  // namespace

void TransientAllocator::init(rhi::Device& device, VmaAllocator allocator, uint32_t bufferSize)
{
  m_device                     = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getNativeDevice()));
  m_allocator                  = allocator;
  m_capacity                   = bufferSize;
  m_head                       = 0;
  m_lastLogicalReleaseTimeline = 0;

  const VkBufferCreateInfo bufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size  = bufferSize,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  const VmaAllocationCreateInfo allocInfo{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_CPU_ONLY,  // Use system RAM, not device-local heap
  };

  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_buffer.buffer, &m_buffer.allocation, &allocationInfo));
  m_mappedData = allocationInfo.pMappedData;

  VkMemoryPropertyFlags memoryPropertyFlags{};
  vmaGetAllocationMemoryProperties(m_allocator, m_buffer.allocation, &memoryPropertyFlags);
  m_isHostCoherent = (memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

TransientAllocator::Allocation TransientAllocator::allocate(uint32_t size, uint32_t alignment)
{
  ASSERT(m_buffer.buffer != VK_NULL_HANDLE && m_mappedData != nullptr, "TransientAllocator must be initialized before allocate");

  const uint32_t alignedOffset = alignUp(m_head, alignment);
  ASSERT(alignedOffset + size <= m_capacity, "Per-frame transient allocator out of memory");

  Allocation allocation{};
  allocation.cpuPtr = static_cast<std::byte*>(m_mappedData) + alignedOffset;
  allocation.handle = kTransientAllocatorBufferHandle;
  allocation.offset = alignedOffset;

  m_head = alignedOffset + size;
  return allocation;
}

void TransientAllocator::flushAllocation(const Allocation& allocation, uint32_t size) const
{
  ASSERT(allocation.handle == kTransientAllocatorBufferHandle, "TransientAllocator flush requires transient allocator handle");
  if(size == 0 || m_isHostCoherent)
  {
    return;
  }

  VK_CHECK(vmaFlushAllocation(m_allocator, m_buffer.allocation, allocation.offset, size));
}

void TransientAllocator::markLogicalRelease(uint64_t submitTimelineValue)
{
  m_lastLogicalReleaseTimeline = submitTimelineValue;
}

void TransientAllocator::destroy()
{
  if(m_buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_buffer.buffer, m_buffer.allocation);
  }

  m_device                     = VK_NULL_HANDLE;
  m_allocator                  = nullptr;
  m_buffer                     = {};
  m_mappedData                 = nullptr;
  m_isHostCoherent             = false;
  m_capacity                   = 0;
  m_head                       = 0;
  m_lastLogicalReleaseTimeline = 0;
}

}  // namespace demo
