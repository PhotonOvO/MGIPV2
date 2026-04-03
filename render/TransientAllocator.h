#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIResourceLifetime.h"

#include <cstddef>
#include <cstdint>

namespace demo {

inline constexpr BufferHandle kTransientAllocatorBufferHandle{0xF301u, 1u};

class TransientAllocator
{
public:
  struct Allocation
  {
    void*        cpuPtr{nullptr};
    BufferHandle handle{};
    uint32_t     offset{0};
  };

  void                     init(rhi::Device& device, VmaAllocator allocator, uint32_t bufferSize);
  [[nodiscard]] Allocation allocate(uint32_t size, uint32_t alignment);
  void                     flushAllocation(const Allocation& allocation, uint32_t size) const;
  void                     markLogicalRelease(uint64_t submitTimelineValue);
  void                     reset() { m_head = 0; }
  void                     destroy();

  [[nodiscard]] static constexpr rhi::ResourceLifetimeTier lifetimeTier()
  {
    return rhi::ResourceLifetimeTier::PerFrame;
  }
  [[nodiscard]] static constexpr rhi::RetirementPolicy retirementPolicy()
  {
    return rhi::RetirementPolicy::frameCount(1);
  }

  [[nodiscard]] uint64_t getBufferOpaque() const { return reinterpret_cast<uint64_t>(m_buffer.buffer); }
  [[nodiscard]] uint64_t getLastLogicalReleaseTimeline() const { return m_lastLogicalReleaseTimeline; }

private:
  VkDevice      m_device{VK_NULL_HANDLE};
  VmaAllocator  m_allocator{nullptr};
  utils::Buffer m_buffer{};
  void*         m_mappedData{nullptr};
  bool          m_isHostCoherent{false};
  uint32_t      m_capacity{0};
  uint32_t      m_head{0};
  uint64_t      m_lastLogicalReleaseTimeline{0};
};

}  // namespace demo
