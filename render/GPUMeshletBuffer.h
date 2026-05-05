#pragma once

#include "../common/Common.h"

#include <vector>

namespace demo {

class GPUMeshletBuffer
{
public:
  void init(VkDevice device, VmaAllocator allocator);
  void deinit();
  void clear();

  void uploadMeshlets(VkCommandBuffer cmd,
                      const std::vector<shaderio::Meshlet>& meshlets,
                      const std::vector<uint32_t>& meshletIndices);

  [[nodiscard]] uint64_t getMeshletDataAddress() const { return static_cast<uint64_t>(m_meshletDataBuffer.address); }
  [[nodiscard]] uint32_t getMeshletCount() const { return m_meshletCount; }

private:
  void destroyBuffer(utils::Buffer& buffer);

  VkDevice      m_device{VK_NULL_HANDLE};
  VmaAllocator  m_allocator{nullptr};
  utils::Buffer m_meshletDataBuffer{};
  utils::Buffer m_meshletVertexBuffer{};
  utils::Buffer m_meshletIndexBuffer{};
  uint32_t      m_meshletCount{0};
};

}  // namespace demo
