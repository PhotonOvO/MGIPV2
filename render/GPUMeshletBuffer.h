#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIHandles.h"
#include "ShaderInterop.h"

#include <cstdint>
#include <vector>

namespace demo::rhi {
class Device;
}

namespace demo {

class GPUMeshletBuffer
{
public:
  struct BufferRecord
  {
    uintptr_t buffer{0};
    uintptr_t allocation{0};
    uintptr_t address{0};
    void*     mapped{nullptr};
  };

  void init(uintptr_t device, uintptr_t allocator, rhi::Device* device_);
  void deinit();
  void clear();

  void uploadMeshlets(const std::vector<shaderio::Meshlet>& meshlets,
                      const std::vector<uint32_t>& meshletIndices,
                      const std::vector<shaderio::GPUCullObject>& meshletCullObjects);

  [[nodiscard]] uint64_t getMeshletDataAddress() const { return static_cast<uint64_t>(m_meshletDataBuffer.address); }
  [[nodiscard]] uintptr_t getMeshletDataBuffer() const { return m_meshletDataBuffer.buffer; }
  [[nodiscard]] uint64_t getMeshletCullObjectAddress() const { return static_cast<uint64_t>(m_meshletCullObjectBuffer.address); }
  [[nodiscard]] uintptr_t getMeshletCullObjectBuffer() const { return m_meshletCullObjectBuffer.buffer; }
  [[nodiscard]] uint64_t getMeshletIndexBufferHandle() const
  {
    return static_cast<uint64_t>(m_meshletIndexBuffer.buffer);
  }
  // Stable RHI handle for the meshlet index buffer (rebound across realloc via
  // VulkanResourceTable::updateBuffer). Consumed by RenderEncoder-based passes.
  [[nodiscard]] rhi::BufferHandle getMeshletIndexBufferRHIHandle() const { return m_meshletIndexBufferRHI; }
  [[nodiscard]] uint32_t getMeshletCount() const { return m_meshletCount; }
  [[nodiscard]] uint32_t getMeshletIndexCount() const { return m_meshletIndexCount; }

private:
  void ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount);
  void destroyBuffer(BufferRecord& buffer);

  uintptr_t     m_device{0};
  uintptr_t     m_allocator{0};
  rhi::Device* m_rhiDevice{nullptr};
  rhi::BufferHandle m_meshletIndexBufferRHI{};
  BufferRecord m_meshletDataBuffer{};
  BufferRecord m_meshletCullObjectBuffer{};
  BufferRecord m_meshletIndexBuffer{};
  uint32_t      m_meshletCount{0};
  uint32_t      m_meshletIndexCount{0};
  uint32_t      m_meshletCapacity{0};
  uint32_t      m_meshletIndexCapacity{0};
};

}  // namespace demo
