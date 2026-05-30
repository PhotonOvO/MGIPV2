#include "BatchUploadContext.h"

#include "UploadUtils.h"

#include <algorithm>
#include <cstring>
#include <execution>
#include <numeric>

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

std::vector<BatchUploadContext::Slice> BatchUploadContext::allocateSlices(const std::vector<VkDeviceSize>& sizes,
                                                                          VkDeviceSize alignment)
{
  return reserveSlices(sizes, alignment);
}

BatchUploadContext::Slice BatchUploadContext::mapReservedSlice(VkDeviceSize offset, VkDeviceSize size) const
{
  ASSERT(offset + size <= m_capacity, "BatchUploadContext reserved slice exceeded capacity");

  Slice slice{};
  slice.cpuPtr = static_cast<std::byte*>(m_stagingBuffer.mapped) + offset;
  slice.offset = offset;
  slice.size = size;
  return slice;
}

std::vector<BatchUploadContext::Slice> BatchUploadContext::reserveSlices(const std::vector<VkDeviceSize>& sizes,
                                                                         VkDeviceSize alignment)
{
  std::vector<Slice> slices;
  slices.reserve(sizes.size());

  VkDeviceSize cursor = m_head;
  for(const VkDeviceSize size : sizes)
  {
    const VkDeviceSize offset = alignUp(cursor, alignment);
    ASSERT(offset + size <= m_capacity, "BatchUploadContext staging reservation exceeded capacity");
    slices.push_back(mapReservedSlice(offset, size));
    cursor = offset + size;
  }

  m_head = cursor;
  return slices;
}

void BatchUploadContext::copyToSlices(std::span<const Slice> slices,
                                      std::span<const std::span<const std::byte>> sources)
{
  ASSERT(slices.size() == sources.size(), "BatchUploadContext slice/source count mismatch");

  std::vector<size_t> indices(slices.size());
  std::iota(indices.begin(), indices.end(), size_t{0});
  std::for_each(std::execution::par, indices.begin(), indices.end(), [&](size_t index) {
    const Slice& slice = slices[index];
    const std::span<const std::byte> source = sources[index];
    ASSERT(source.size_bytes() <= slice.size, "BatchUploadContext source does not fit reserved slice");
    if(source.empty())
    {
      return;
    }
    std::memcpy(slice.cpuPtr, source.data(), source.size_bytes());
  });
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

  std::vector<VkImageMemoryBarrier2> imageBarriers;
  imageBarriers.reserve(m_pendingUploads.size());

  for(const UploadOperation& op : m_pendingUploads)
  {
    if(op.type == UploadType::buffer)
    {
      vkCmdCopyBuffer(cmd, m_stagingBuffer.buffer, op.dstBuffer, 1, &op.bufferRegion);
    }
    else
    {
      vkCmdCopyBufferToImage(cmd, m_stagingBuffer.buffer, op.dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &op.imageRegion);
      imageBarriers.push_back(VkImageMemoryBarrier2{
          .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT,
          .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image               = op.dstImage,
          .subresourceRange =
              {
                  .aspectMask     = op.imageRegion.imageSubresource.aspectMask,
                  .baseMipLevel   = op.imageRegion.imageSubresource.mipLevel,
                  .levelCount     = 1,
                  .baseArrayLayer = op.imageRegion.imageSubresource.baseArrayLayer,
                  .layerCount     = op.imageRegion.imageSubresource.layerCount,
              },
      });
    }
  }

  if(!imageBarriers.empty())
  {
    const VkDependencyInfo dependencyInfo{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size()),
        .pImageMemoryBarriers    = imageBarriers.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
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
