#pragma once

#include "../RHIFrameContext.h"

#include <memory>
#include <vector>

namespace demo::rhi::metal {

class MetalFrameContext final : public FrameContext
{
public:
  MetalFrameContext() = default;
  ~MetalFrameContext() override;

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
  void waitForFrame(uint64_t frameIndex) override;

  uint32_t getFrameCount() const override;
  uint32_t getCurrentFrameIndex() const override;
  uint64_t getCurrentFrameValue() const override;

  void* getTimelineSemaphore() const override;

  void     enqueueRetirement(ResourceHandle resource, uint64_t timelineValue) override;
  uint32_t processRetirements(uint64_t currentTimelineValue) override;

  DeferredDestructionQueue&       getDestructionQueue() override;
  const DeferredDestructionQueue& getDestructionQueue() const override;

private:
  struct FrameSlot
  {
    CommandList* commandList{nullptr};
    uint64_t     lastSignalValue{0};
    void*        commandBuffer{nullptr};      // id<MTLCommandBuffer>
    void*        completionHandler{nullptr};  // Tracking object for completion
  };

  // NOTES: Metal Frame Synchronization Strategy
  // Metal doesn't have timeline semaphores like Vulkan
  // Use MTLCommandBuffer completion handlers instead:
  //
  // 1. Create command buffer: queue.commandBuffer
  // 2. Add completion handler: [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
  //      // Mark frame as complete, update timeline value
  //    }]
  // 3. Submit command buffer: [commandBuffer commit]
  // 4. Track completion with a monotonic counter (like Vulkan timeline)
  //
  // Wait Strategy:
  // - Wait on completion handler or MTLSharedEvent (if available)
  // - Use dispatch_semaphore for CPU-side waiting
  //
  // NOTES: Metal's synchronization is implicit and event-driven
  // No explicit timeline semaphores, but we can simulate the behavior

  SubmissionReceipt submitCurrentFrame(CommandList& commandList);

  void* m_device{nullptr};  // id<MTLDevice>
  void* m_queue{nullptr};   // id<MTLCommandQueue>

  std::vector<FrameSlot>         m_frames;
  std::vector<FrameData>         m_frameData;
  Swapchain*                     m_swapchain{nullptr};
  InlineDeferredDestructionQueue m_deferredDestructionQueue;
  uint32_t                       m_currentFrameIndex{0};
  uint64_t                       m_frameCounter{0};

  bool m_initialized{false};
};

}  // namespace demo::rhi::metal