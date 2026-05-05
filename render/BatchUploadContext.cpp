#include "BatchUploadContext.h"

#include "UploadUtils.h"

namespace demo {

namespace {

[[nodiscard]] VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
  const VkDeviceSize safeAlignment = alignment == 0 ? 1 : alignment;
  const VkDeviceSize mask          = safeAlignment - 1;
  return (value + mask) & ~mask;
}

}  // namespace

void BatchUploadContext::init(VkDevice device, VmaAllocator allocator, VkDeviceSize totalSize)
{
  destroy();

  m_device     = device;
  m_allocator  = allocator;
  m_capacity   = totalSize;
  m_head       = 0;

  if(totalSize == 0)
  {
    return;
  }

  m_stagingBuffer = upload::createMappedUploadStagingBuffer(device, allocator, totalSize);
}

BatchUploadContext::Slice BatchUploadContext::allocate(VkDeviceSize size, VkDeviceSize alignment)
{
  const VkDeviceSize offset = alignUp(m_head, alignment);
  ASSERT(offset + size <= m_capacity, "BatchUploadContext staging allocation exceeded capacity");

  Slice slice{};
  slice.cpuPtr = static_cast<std::byte*>(m_stagingBuffer.mapped) + offset;
  slice.offset = offset;
  slice.size   = size;

  m_head = offset + size;
  return slice;
}

void BatchUploadContext::recordTextureUpload(const Slice& slice, VkImage dstImage, const VkBufferImageCopy& region)
{
  UploadOperation op{};
  op.type      = UploadType::image;
  op.dstImage  = dstImage;
  op.imageRegion = region;
  op.imageRegion.bufferOffset += slice.offset;
  m_pendingUploads.push_back(op);
}

void BatchUploadContext::recordBufferUpload(const Slice& slice, VkBuffer dstBuffer, const VkBufferCopy& region)
{
  UploadOperation op{};
  op.type         = UploadType::buffer;
  op.dstBuffer    = dstBuffer;
  op.bufferRegion = region;
  op.bufferRegion.srcOffset += slice.offset;
  m_pendingUploads.push_back(op);
}

void BatchUploadContext::executeUploads(VkCommandBuffer cmd) const
{
  if(m_stagingBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  for(const UploadOperation& op : m_pendingUploads)
  {
    if(op.type == UploadType::buffer)
    {
      vkCmdCopyBuffer(cmd, m_stagingBuffer.buffer, op.dstBuffer, 1, &op.bufferRegion);
    }
    else
    {
      vkCmdCopyBufferToImage(cmd, m_stagingBuffer.buffer, op.dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &op.imageRegion);
    }
  }
}

utils::Buffer BatchUploadContext::releaseStagingBuffer()
{
  utils::Buffer buffer = m_stagingBuffer;
  m_stagingBuffer      = {};
  m_capacity           = 0;
  m_head               = 0;
  m_pendingUploads.clear();
  return buffer;
}

void BatchUploadContext::destroy()
{
  if(m_stagingBuffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_stagingBuffer.buffer, m_stagingBuffer.allocation);
  }

  m_device        = VK_NULL_HANDLE;
  m_allocator     = nullptr;
  m_stagingBuffer = {};
  m_capacity      = 0;
  m_head          = 0;
  m_pendingUploads.clear();
}

}  // namespace demo
