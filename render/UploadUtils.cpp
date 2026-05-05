#include "UploadUtils.h"

#include <algorithm>
#include <cstring>

namespace demo::upload {

namespace {

constexpr VkDeviceSize kKiB = 1024ull;
constexpr VkDeviceSize kMiB = 1024ull * kKiB;

[[nodiscard]] bool tryCreateBufferInternal(VkDevice device,
                                           VmaAllocator allocator,
                                           VkDeviceSize size,
                                           VkBufferUsageFlags2KHR usage,
                                           VmaAllocationCreateInfo allocInfo,
                                           utils::Buffer& outBuffer)
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };

  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  if(size > 64ull * kKiB)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  }

  utils::Buffer     buffer{};
  VmaAllocationInfo allocationInfo{};
  const VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo);
  if(result != VK_SUCCESS)
  {
    return false;
  }
  buffer.mapped = allocationInfo.pMappedData;

  const VkBufferDeviceAddressInfo addressInfo{
      .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = buffer.buffer,
  };
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  outBuffer = buffer;
  return true;
}

[[nodiscard]] utils::Buffer createBufferInternal(VkDevice device,
                                                 VmaAllocator allocator,
                                                 VkDeviceSize size,
                                                 VkBufferUsageFlags2KHR usage,
                                                 VmaAllocationCreateInfo allocInfo)
{
  utils::Buffer buffer{};
  const bool created = tryCreateBufferInternal(device, allocator, size, usage, allocInfo, buffer);
  ASSERT(created, "createBufferInternal failed");
  return buffer;
}

void writeAllocationBytes(VmaAllocator allocator, VmaAllocation allocation, std::span<const std::byte> data)
{
  if(data.empty())
  {
    return;
  }

  VmaAllocationInfo allocationInfo{};
  vmaGetAllocationInfo(allocator, allocation, &allocationInfo);

  void* mappedData = allocationInfo.pMappedData;
  bool  mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(allocator, allocation, &mappedData));
    mappedHere = true;
  }

  std::memcpy(mappedData, data.data(), data.size_bytes());

  VkMemoryPropertyFlags memoryPropertyFlags{};
  vmaGetAllocationMemoryProperties(allocator, allocation, &memoryPropertyFlags);
  if((memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
  {
    VK_CHECK(vmaFlushAllocation(allocator, allocation, 0, data.size_bytes()));
  }

  if(mappedHere)
  {
    vmaUnmapMemory(allocator, allocation);
  }
}

[[nodiscard]] bool supportsDirectUploadMemory(const rhi::MemoryProperties& memoryProperties, VkDeviceSize& heapSizeOut)
{
  heapSizeOut = 0;
  for(const rhi::MemoryTypeInfo& memoryType : memoryProperties.memoryTypes)
  {
    const uint32_t requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if((memoryType.propertyFlags & requiredFlags) != requiredFlags)
    {
      continue;
    }

    if(memoryType.heapIndex >= memoryProperties.memoryHeaps.size())
    {
      continue;
    }

    heapSizeOut = std::max(heapSizeOut, static_cast<VkDeviceSize>(memoryProperties.memoryHeaps[memoryType.heapIndex].size));
  }

  return heapSizeOut > 0;
}

[[nodiscard]] bool tryCreateDirectUploadBuffer(VkDevice device,
                                               VmaAllocator allocator,
                                               std::span<const std::byte> data,
                                               VkBufferUsageFlags2KHR usage,
                                               utils::Buffer& outBuffer)
{
  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage          = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  allocInfo.flags          = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  allocInfo.requiredFlags  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  utils::Buffer buffer{};
  if(!tryCreateBufferInternal(device, allocator, data.size_bytes(), usage, allocInfo, buffer))
  {
    return false;
  }

  VkMemoryPropertyFlags memoryPropertyFlags{};
  vmaGetAllocationMemoryProperties(allocator, buffer.allocation, &memoryPropertyFlags);
  const uint32_t requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  if((memoryPropertyFlags & requiredFlags) != requiredFlags)
  {
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    return false;
  }

  writeAllocationBytes(allocator, buffer.allocation, data);
  outBuffer = buffer;
  return true;
}

}  // namespace

StaticBufferUploadPolicy buildStaticBufferUploadPolicy(const rhi::MemoryProperties& memoryProperties)
{
  VkDeviceSize directUploadHeapSize = 0;
  if(!supportsDirectUploadMemory(memoryProperties, directUploadHeapSize))
  {
    return {};
  }

  // Keep direct writes conservative: they are useful on ReBAR heaps for small/medium
  // static buffers, but large uploads still favor staging to avoid saturating PCIe.
  const VkDeviceSize threshold = std::clamp(directUploadHeapSize / 64ull, 8ull * kMiB, 64ull * kMiB);
  return StaticBufferUploadPolicy{
      .allowDirectHostVisibleDeviceLocalUpload = true,
      .directUploadThreshold                   = threshold,
  };
}

utils::Buffer createUploadStagingBuffer(VkDevice device, VmaAllocator allocator, std::span<const std::byte> data)
{
  utils::Buffer stagingBuffer = createMappedUploadStagingBuffer(device, allocator, data.size_bytes());
  writeAllocationBytes(allocator, stagingBuffer.allocation, data);
  return stagingBuffer;
}

utils::Buffer createMappedUploadStagingBuffer(VkDevice device, VmaAllocator allocator, VkDeviceSize size)
{
  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
  allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

  return createBufferInternal(device, allocator, size, VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR, allocInfo);
}

utils::Buffer createStaticBuffer(VkDevice device,
                                 VmaAllocator allocator,
                                 VkDeviceSize size,
                                 VkBufferUsageFlags2KHR usage)
{
  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  return createBufferInternal(device, allocator, size, usage | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR, allocInfo);
}

utils::Buffer createStaticBufferWithUpload(VkDevice device,
                                           VmaAllocator allocator,
                                           VkCommandBuffer cmd,
                                           std::span<const std::byte> data,
                                           VkBufferUsageFlags2KHR usage,
                                           const StaticBufferUploadPolicy& policy,
                                           std::vector<utils::Buffer>* deferredStagingBuffers)
{
  if(policy.allowDirectHostVisibleDeviceLocalUpload && policy.directUploadThreshold > 0
     && data.size_bytes() <= policy.directUploadThreshold)
  {
    utils::Buffer directBuffer{};
    if(tryCreateDirectUploadBuffer(device, allocator, data, usage, directBuffer))
    {
      return directBuffer;
    }
  }

  utils::Buffer stagingBuffer = createUploadStagingBuffer(device, allocator, data);

  utils::Buffer gpuBuffer = createStaticBuffer(device, allocator, data.size_bytes(), usage);

  const VkBufferCopy copyRegion{.size = data.size_bytes()};
  vkCmdCopyBuffer(cmd, stagingBuffer.buffer, gpuBuffer.buffer, 1, &copyRegion);

  if(deferredStagingBuffers != nullptr)
  {
    deferredStagingBuffers->push_back(stagingBuffer);
  }
  else
  {
    vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
  }

  return gpuBuffer;
}

}  // namespace demo::upload
