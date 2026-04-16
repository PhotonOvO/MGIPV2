#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"

#include <span>

namespace demo::upload {

struct StaticBufferUploadPolicy
{
  bool         allowDirectHostVisibleDeviceLocalUpload{false};
  VkDeviceSize directUploadThreshold{0};
};

[[nodiscard]] StaticBufferUploadPolicy buildStaticBufferUploadPolicy(const rhi::MemoryProperties& memoryProperties);

[[nodiscard]] utils::Buffer createUploadStagingBuffer(VkDevice device,
                                                      VmaAllocator allocator,
                                                      std::span<const std::byte> data);

[[nodiscard]] utils::Buffer createStaticBufferWithUpload(VkDevice device,
                                                         VmaAllocator allocator,
                                                         VkCommandBuffer cmd,
                                                         std::span<const std::byte> data,
                                                         VkBufferUsageFlags2KHR usage,
                                                         const StaticBufferUploadPolicy& policy,
                                                         std::vector<utils::Buffer>* deferredStagingBuffers);

}  // namespace demo::upload
