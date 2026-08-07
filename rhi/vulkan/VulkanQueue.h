#pragma once

#include "../RHIQueue.h"
#include "../../common/HandlePool.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

class VulkanCommandBuffer;

struct VulkanQueueSyncRecord
{
  VkSemaphore semaphore{VK_NULL_HANDLE};
};

class VulkanQueueSyncRegistry
{
public:
  [[nodiscard]] QueueSyncHandle registerSemaphore(VkSemaphore semaphore);
  void unregisterSemaphore(QueueSyncHandle handle);
  [[nodiscard]] VkSemaphore resolve(QueueSyncHandle handle) const;

private:
  demo::HandlePool<QueueSyncHandle, VulkanQueueSyncRecord> m_semaphores;
};

class VulkanQueue final : public Queue
{
public:
  VulkanQueue() = default;
  ~VulkanQueue() override;

  void init(VkDevice device, VkQueue queue, uint32_t familyIndex, uint32_t queueIndex,
            QueueIdentity identity, QueueInfo info, VulkanQueueSyncRegistry* syncRegistry);
  void deinit();
  void setQueueRegistry(std::array<VulkanQueue*, 3> queues) noexcept;

  [[nodiscard]] QueueIdentity identity() const noexcept override;
  [[nodiscard]] QueueInfo info() const noexcept override;
  [[nodiscard]] SubmissionToken submit(const SubmitBatch& batch) override;
  [[nodiscard]] bool isComplete(SubmissionToken token) const override;
  void wait(SubmissionToken token) override;
  void waitIdle() override;
  [[nodiscard]] SubmissionToken lastSubmittedToken() const noexcept override;

  [[nodiscard]] VkQueue nativeQueue() const noexcept { return m_queue; }
  [[nodiscard]] VkSemaphore nativeTimeline() const noexcept { return m_timeline; }
  [[nodiscard]] uint32_t familyIndex() const noexcept { return m_familyIndex; }
  [[nodiscard]] uint64_t completedValue() const;

private:
  [[nodiscard]] VulkanQueue& resolveQueue(QueueIdentity identity) const;
  void createTimeline(uint64_t initialValue);
  void recreateExhaustedTimeline();

  VkDevice m_device{VK_NULL_HANDLE};
  VkQueue m_queue{VK_NULL_HANDLE};
  VkSemaphore m_timeline{VK_NULL_HANDLE};
  uint32_t m_familyIndex{~0u};
  uint32_t m_queueIndex{0};
  QueueIdentity m_identity{};
  QueueInfo m_info{};
  VulkanQueueSyncRegistry* m_syncRegistry{nullptr};
  std::array<VulkanQueue*, 3> m_queues{};
  uint64_t m_lastSubmittedValue{0};
};

}  // namespace demo::rhi::vulkan