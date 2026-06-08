#pragma once

#include "../common/Handles.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace demo::upload {

struct StaticBufferUploadPolicy
{
  bool         allowDirectHostVisibleDeviceLocalUpload{false};
  uint64_t directUploadThreshold{0};
};

[[nodiscard]] StaticBufferUploadPolicy buildStaticBufferUploadPolicy(const rhi::MemoryProperties& memoryProperties);

[[nodiscard]] rhi::BufferHandle createUploadStagingBuffer(rhi::Device& device, std::span<const std::byte> data);

[[nodiscard]] rhi::BufferHandle createMappedUploadStagingBuffer(rhi::Device& device, uint64_t size);

[[nodiscard]] rhi::BufferHandle createStaticBuffer(rhi::Device& device, uint64_t size, rhi::BufferUsageFlags usage);

[[nodiscard]] rhi::BufferHandle createStaticBufferWithUpload(rhi::Device& device,
                                                             rhi::CommandBuffer& cmd,
                                                             std::span<const std::byte> data,
                                                             rhi::BufferUsageFlags usage,
                                                             const StaticBufferUploadPolicy& policy,
                                                             std::vector<rhi::BufferHandle>* deferredStagingBuffers);

}  // namespace demo::upload
