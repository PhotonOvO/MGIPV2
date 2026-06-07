#include "TransientAllocator.h"

namespace demo {

namespace {

[[nodiscard]] uint32_t alignUp(uint32_t value, uint32_t alignment)
{
  const uint32_t safeAlignment = alignment == 0 ? 1u : alignment;
  const uint32_t remainder = value % safeAlignment;
  if(remainder == 0u)
  {
    return value;
  }
  return value + (safeAlignment - remainder);
}

}  // namespace

void TransientAllocator::init(rhi::Device& device, uint32_t bufferSize)
{
  m_device                     = &device;
  m_capacity                   = bufferSize;
  m_head                       = 0;
  m_lastLogicalReleaseTimeline = 0;

  m_buffer = device.createBuffer(rhi::BufferDesc{
      .size        = bufferSize,
      .usage       = rhi::BufferUsageFlags::transferSrc | rhi::BufferUsageFlags::transferDst
                   | rhi::BufferUsageFlags::uniform | rhi::BufferUsageFlags::storage
                   | rhi::BufferUsageFlags::vertex | rhi::BufferUsageFlags::index
                   | rhi::BufferUsageFlags::indirect | rhi::BufferUsageFlags::shaderDeviceAddress,
      .memoryUsage = rhi::MemoryUsage::cpuToGpu,
      .allowGpuAddress = true,
      .allowIndirectArgument = true,
      .debugName = "TransientAllocatorBuffer",
  });
  m_mappedData = device.mapBuffer(m_buffer);
}

TransientAllocator::Allocation TransientAllocator::allocate(uint32_t size, uint32_t alignment)
{
  ASSERT(!m_buffer.isNull() && m_mappedData != nullptr, "TransientAllocator must be initialized before allocate");

  const uint32_t alignedOffset = alignUp(m_head, alignment);
  ASSERT(alignedOffset + size <= m_capacity, "Per-frame transient allocator out of memory");

  Allocation allocation{};
  allocation.cpuPtr = static_cast<std::byte*>(m_mappedData) + alignedOffset;
  allocation.handle = m_buffer;
  allocation.offset = alignedOffset;
  const rhi::GpuPtr baseGpu = m_device != nullptr ? m_device->getBufferGpuAddress(m_buffer) : rhi::GpuPtr{};
  allocation.gpu    = rhi::GpuPtr{baseGpu.value != 0 ? baseGpu.value + alignedOffset : 0};

  m_head = alignedOffset + size;
  return allocation;
}

void TransientAllocator::flushAllocation(const Allocation& allocation, uint32_t size) const
{
  ASSERT(allocation.handle == m_buffer, "TransientAllocator flush requires transient allocator handle");
  (void)allocation;
  (void)size;
}

void TransientAllocator::markLogicalRelease(uint64_t submitTimelineValue)
{
  m_lastLogicalReleaseTimeline = submitTimelineValue;
}

void TransientAllocator::destroy()
{
  if(m_device != nullptr && !m_buffer.isNull())
  {
    m_device->destroyBuffer(m_buffer);
  }

  m_device                     = nullptr;
  m_buffer                     = {};
  m_mappedData                 = nullptr;
  m_capacity                   = 0;
  m_head                       = 0;
  m_lastLogicalReleaseTimeline = 0;
}

}  // namespace demo
