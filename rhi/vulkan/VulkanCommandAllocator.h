#pragma once

#include "../RHICommandAllocator.h"

#include <vector>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

class VulkanCommandBuffer;
class VulkanQueue;
class VulkanResourceTable;

class VulkanCommandAllocator final : public CommandAllocator
{
public:
  VulkanCommandAllocator(VulkanQueue& queue, VkDevice device,
                         VulkanResourceTable* resourceTable);
  ~VulkanCommandAllocator() override;

  [[nodiscard]] QueueClass queueClass() const noexcept override;
  void reset(SubmissionToken completedToken = {}) override;
  [[nodiscard]] std::unique_ptr<CommandBuffer> createCommandBuffer() override;

  void noteSubmitted(VulkanCommandBuffer& commandBuffer, SubmissionToken token);
  void releaseCommandBuffer(VulkanCommandBuffer& commandBuffer,
                            VkCommandBuffer nativeCommandBuffer) noexcept;

  [[nodiscard]] VulkanQueue& queue() const noexcept { return m_queue; }
  [[nodiscard]] VkCommandPool nativePool() const noexcept { return m_pool; }
  [[nodiscard]] VulkanResourceTable* resourceTable() const noexcept
  {
    return m_resourceTable;
  }

private:
  VulkanQueue& m_queue;
  VkDevice m_device{VK_NULL_HANDLE};
  VkCommandPool m_pool{VK_NULL_HANDLE};
  VulkanResourceTable* m_resourceTable{nullptr};
  std::vector<VulkanCommandBuffer*> m_commandBuffers;
  SubmissionToken m_lastSubmission{};
};

}  // namespace demo::rhi::vulkan