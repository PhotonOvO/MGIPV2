#pragma once

#include "../rhi/RHICommandAllocator.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIQueue.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace demo {

class FrameScheduler
{
public:
  FrameScheduler() = default;
  ~FrameScheduler();

  FrameScheduler(const FrameScheduler&) = delete;
  FrameScheduler& operator=(const FrameScheduler&) = delete;

  void init(rhi::Device& device, uint32_t frameCount);
  void shutdown();

  [[nodiscard]] rhi::CommandBuffer& beginFrame();
  [[nodiscard]] rhi::SubmissionToken submitFrame(
    rhi::CommandBuffer& commandBuffer,
    rhi::SubmitWaitPoint waitPoint = {},
    rhi::SubmitSignalPoint signalPoint = {});

  void waitForFrame(uint32_t frameIndex);
  void waitForAllFrames();

  [[nodiscard]] uint32_t frameCount() const noexcept;
  [[nodiscard]] uint32_t currentFrameIndex() const noexcept;
  [[nodiscard]] rhi::SubmissionToken currentFrameToken() const noexcept;
  [[nodiscard]] rhi::Queue& queue() const;

private:
  struct FrameSlot
  {
    std::unique_ptr<rhi::CommandAllocator> allocator;
    std::unique_ptr<rhi::CommandBuffer> commandBuffer;
    rhi::SubmissionToken submission{};
  };

  rhi::Device* m_device{nullptr};
  rhi::Queue* m_queue{nullptr};
  std::vector<FrameSlot> m_frames;
  uint32_t m_currentFrameIndex{0};
  bool m_recording{false};
};

}  // namespace demo