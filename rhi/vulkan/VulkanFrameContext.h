#pragma once

#include "../RHIFrameContext.h"
#include "VulkanCommandPool.h"
#include "VulkanSynchronization.h"

#include <memory>
#include <vector>

struct VkQueue_T;

using VkQueue = VkQueue_T*;

namespace demo {
namespace rhi {
namespace vulkan {

class VulkanFrameContext final : public FrameContext
{
public:
  VulkanFrameContext() = default;
  ~VulkanFrameContext() override;

  void init(void* nativeDevice, uint32_t queueFamilyIndex, uint32_t frameCount) override;
  void deinit() override;

  void              beginFrame() override;
  SubmissionReceipt endFrame(CommandList* cmdList) override;
  void              setSwapchain(Swapchain* swapchain) override;

  SubmissionReceipt submitCommandLists(const SubmissionRequest* requests, uint32_t requestCount) override;
  void              waitForSubmission(SubmissionReceipt receipt) override;

  FrameData& getCurrentFrame() override;
  void       advanceToNextFrame() override;

  void waitCurrentFrame() override;
  void waitForFrameCompletion() override;
  void waitForFrame(uint64_t frameIndex) override;

  uint32_t getFrameCount() const override;
  uint32_t getCurrentFrameIndex() const override;
  uint64_t getCurrentFrameValue() const override;

  void* getTimelineSemaphore() const override;

  void     enqueueRetirement(ResourceHandle resource, uint64_t timelineValue) override;
  uint32_t processRetirements(uint64_t currentTimelineValue) override;

  DeferredDestructionQueue&       getDestructionQueue() override;
  const DeferredDestructionQueue& getDestructionQueue() const override;

  VkSemaphore     nativeTimelineSemaphore() const;
  VkCommandBuffer nativeCommandBuffer(uint32_t frameIndex) const;

private:
  struct FrameSlot
  {
    std::unique_ptr<VulkanCommandPool> commandPool;
    CommandList*                       commandList{nullptr};
    uint64_t                           lastSignalValue{0};
  };

  SubmissionReceipt submitCurrentFrame(CommandList& commandList);

  VkDevice                                 m_device{nullptr};
  uint32_t                                 m_queueFamilyIndex{~0U};
  VkQueue                                  m_graphicsQueue{nullptr};
  std::unique_ptr<VulkanTimelineSemaphore> m_timelineSemaphore;
  std::vector<FrameSlot>                   m_frames;
  std::vector<FrameData>                   m_frameData;
  Swapchain*                               m_swapchain{nullptr};
  InlineDeferredDestructionQueue           m_deferredDestructionQueue;
  uint32_t                                 m_currentFrameIndex{0};
  uint64_t                                 m_frameCounter{0};
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
