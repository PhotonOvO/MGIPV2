#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"

#include <vector>

namespace demo::rhi {
class Device;
}

namespace demo {

class GPUMeshletBuffer
{
public:
  void init(VkDevice device, VmaAllocator allocator, rhi::Device* device_);
  void deinit();
  void clear();

  void uploadMeshlets(VkCommandBuffer cmd,
                      const std::vector<shaderio::Meshlet>& meshlets,
                      const std::vector<uint32_t>& meshletIndices,
                      const std::vector<shaderio::GPUCullObject>& meshletCullObjects);

  [[nodiscard]] uint64_t getMeshletDataAddress() const { return static_cast<uint64_t>(m_meshletDataBuffer.address); }
  [[nodiscard]] VkBuffer getMeshletDataBuffer() const { return m_meshletDataBuffer.buffer; }
  [[nodiscard]] uint64_t getMeshletCullObjectAddress() const { return static_cast<uint64_t>(m_meshletCullObjectBuffer.address); }
  [[nodiscard]] VkBuffer getMeshletCullObjectBuffer() const { return m_meshletCullObjectBuffer.buffer; }
  [[nodiscard]] uint64_t getMeshletIndexBufferHandle() const
  {
    return reinterpret_cast<uint64_t>(m_meshletIndexBuffer.buffer);
  }
  // Stable RHI handle for the meshlet index buffer (rebound across realloc via
  // VulkanResourceTable::updateBuffer). Consumed by RenderEncoder-based passes.
  [[nodiscard]] rhi::BufferHandle getMeshletIndexBufferRHIHandle() const { return m_meshletIndexBufferRHI; }
  [[nodiscard]] uint32_t getMeshletCount() const { return m_meshletCount; }
  [[nodiscard]] uint32_t getMeshletIndexCount() const { return m_meshletIndexCount; }

private:
  void ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount);
  void destroyBuffer(utils::Buffer& buffer);

  VkDevice      m_device{VK_NULL_HANDLE};
  VmaAllocator  m_allocator{nullptr};
  rhi::Device* m_rhiDevice{nullptr};
  rhi::BufferHandle m_meshletIndexBufferRHI{};
  utils::Buffer m_meshletDataBuffer{};
  utils::Buffer m_meshletCullObjectBuffer{};
  utils::Buffer m_meshletIndexBuffer{};
  uint32_t      m_meshletCount{0};
  uint32_t      m_meshletIndexCount{0};
  uint32_t      m_meshletCapacity{0};
  uint32_t      m_meshletIndexCapacity{0};
};

}  // namespace demo
