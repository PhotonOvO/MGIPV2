#pragma once

#include "../common/Common.h"

#include <vector>

namespace demo {

class BatchUploadContext
{
public:
  struct Slice
  {
    void*        cpuPtr{nullptr};
    VkDeviceSize offset{0};
    VkDeviceSize size{0};
  };

  void init(VkDevice device, VmaAllocator allocator, VkDeviceSize totalSize);
  [[nodiscard]] bool isInitialized() const { return m_stagingBuffer.buffer != VK_NULL_HANDLE; }
  [[nodiscard]] Slice allocate(VkDeviceSize size, VkDeviceSize alignment);

  void recordTextureUpload(const Slice& slice, VkImage dstImage, const VkBufferImageCopy& region);
  void recordBufferUpload(const Slice& slice, VkBuffer dstBuffer, const VkBufferCopy& region);
  void executeUploads(VkCommandBuffer cmd) const;

  [[nodiscard]] utils::Buffer releaseStagingBuffer();
  void destroy();

private:
  enum class UploadType
  {
    buffer,
    image,
  };

  struct UploadOperation
  {
    UploadType        type{UploadType::buffer};
    VkBuffer          dstBuffer{VK_NULL_HANDLE};
    VkImage           dstImage{VK_NULL_HANDLE};
    VkBufferCopy      bufferRegion{};
    VkBufferImageCopy imageRegion{};
  };

  VkDevice            m_device{VK_NULL_HANDLE};
  VmaAllocator        m_allocator{nullptr};
  utils::Buffer       m_stagingBuffer{};
  VkDeviceSize        m_capacity{0};
  VkDeviceSize        m_head{0};
  std::vector<UploadOperation> m_pendingUploads;
};

}  // namespace demo
