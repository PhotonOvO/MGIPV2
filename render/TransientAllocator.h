#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIResourceLifetime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

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
    rhi::GpuPtr  gpu{};  // FrameGpuAllocator: GPU address of this region (0 if buffer has no device address)
  };

  void                     init(rhi::Device& device, uint32_t bufferSize);
  [[nodiscard]] Allocation allocate(uint32_t size, uint32_t alignment);
  void                     flushAllocation(const Allocation& allocation, uint32_t size) const;

  // gpu_temp_allocate-style typed helpers. allocateTyped returns a writable pointer
  // into the mapped ring buffer plus the binding offset; the caller fills *data then
  // calls flushAllocation (no-op when host-coherent). allocateAndWrite collapses the
  // ubiquitous allocate + memcpy + flush into one call for plain-old-data uniforms.
  template <typename T>
  struct TypedAllocation
  {
    T*           data{nullptr};
    BufferHandle handle{};
    uint32_t     offset{0};
    rhi::GpuPtr  gpu{};

    [[nodiscard]] Allocation toUntyped() const { return Allocation{data, handle, offset, gpu}; }
  };

  template <typename T>
  [[nodiscard]] TypedAllocation<T> allocateTyped(uint32_t alignment = alignof(T))
  {
    const Allocation a = allocate(static_cast<uint32_t>(sizeof(T)), alignment);
    return TypedAllocation<T>{static_cast<T*>(a.cpuPtr), a.handle, a.offset, a.gpu};
  }

  template <typename T>
  [[nodiscard]] Allocation allocateAndWrite(const T& value, uint32_t alignment = alignof(T))
  {
    const Allocation a = allocate(static_cast<uint32_t>(sizeof(T)), alignment);
    std::memcpy(a.cpuPtr, &value, sizeof(T));
    flushAllocation(a, static_cast<uint32_t>(sizeof(T)));
    return a;
  }
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

  [[nodiscard]] rhi::BufferHandle getBufferHandle() const { return m_buffer; }
  [[nodiscard]] uint32_t getCapacity() const { return m_capacity; }
  [[nodiscard]] uint64_t getLastLogicalReleaseTimeline() const { return m_lastLogicalReleaseTimeline; }

private:
  rhi::Device*  m_device{nullptr};
  rhi::BufferHandle m_buffer{};
  void*         m_mappedData{nullptr};
  uint32_t      m_capacity{0};
  uint32_t      m_head{0};
  uint64_t      m_lastLogicalReleaseTimeline{0};
};

}  // namespace demo
