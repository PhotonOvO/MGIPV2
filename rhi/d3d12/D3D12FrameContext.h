#pragma once

#include "../RHIFrameContext.h"

#include <memory>
#include <vector>

namespace demo::rhi::d3d12 {

class D3D12FrameContext final : public FrameContext
{
public:
  D3D12FrameContext() = default;
  ~D3D12FrameContext() override;

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
    void*        commandAllocator{nullptr};  // ID3D12CommandAllocator
    void*        commandList{nullptr};       // ID3D12GraphicsCommandList
    void*        fence{nullptr};             // ID3D12Fence
    void*        fenceEvent{nullptr};        // HANDLE
    uint64_t     fenceValue{0};
    CommandList* commandListWrapper{nullptr};
  };

  // NOTES: D3D12 Frame Synchronization Strategy
  // D3D12 uses fences for timeline-style synchronization:
  //
  // Timeline Semaphore Emulation:
  // - ID3D12Fence provides timeline-like behavior
  // - GetCompletedValue() returns current completed value
  // - SetEventOnCompletion() can signal an event at a specific value
  // - Signal(fence, value) on command queue sets fence value
  //
  // Frame Flow:
  // 1. beginFrame(): Reset command allocator for current frame
  // 2. Record commands into command list
  // 3. endFrame():
  //    - Close command list
  //    - Signal fence with ++m_frameCounter
  //    - Execute command list on queue
  // 4. nextFrame():
  //    - Wait for fence at frame's fenceValue
  //    - Reset command allocator for reuse
  //
  // Wait Strategy:
  // - Wait on fence event: fence->SetEventOnCompletion(waitValue, event)
  // - WaitForSingleObject(event, timeout)
  // - Or poll: fence->GetCompletedValue() >= waitValue

  SubmissionReceipt submitCurrentFrame(CommandList& commandList);

  void* m_device{nullptr};        // ID3D12Device
  void* m_commandQueue{nullptr};  // ID3D12CommandQueue

  std::vector<FrameSlot>         m_frames;
  std::vector<FrameData>         m_frameData;
  Swapchain*                     m_swapchain{nullptr};
  InlineDeferredDestructionQueue m_deferredDestructionQueue;
  uint32_t                       m_currentFrameIndex{0};
  uint64_t                       m_frameCounter{0};

  bool m_initialized{false};
};

}  // namespace demo::rhi::d3d12