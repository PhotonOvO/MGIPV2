#pragma once

#include "../RHIQueue.h"

#include <array>
#include <cstdint>

namespace MTL {
class Drawable;
class SharedEvent;
}

namespace MTL4 {
class CommandBuffer;
class CommandQueue;
}

namespace demo::rhi::metal {

class MetalCommandBuffer;

class MetalQueue final : public Queue
{
public:
  MetalQueue() = default;
  ~MetalQueue() override;

  void init(MTL4::CommandQueue* queue, MTL::SharedEvent* completionEvent,
            QueueIdentity identity, QueueInfo info);
  void deinit();
  void setQueueRegistry(std::array<MetalQueue*, 3> queues) noexcept;

  [[nodiscard]] QueueIdentity identity() const noexcept override;
  [[nodiscard]] QueueInfo info() const noexcept override;
  [[nodiscard]] SubmissionToken submit(const SubmitBatch& batch) override;
  [[nodiscard]] bool isComplete(SubmissionToken token) const override;
  void wait(SubmissionToken token) override;
  void waitIdle() override;
  [[nodiscard]] SubmissionToken lastSubmittedToken() const noexcept override;

  void waitDrawable(MTL::Drawable* drawable);

  [[nodiscard]] MTL4::CommandQueue* nativeQueue() const noexcept { return m_queue; }
  [[nodiscard]] MTL::SharedEvent* completionEvent() const noexcept { return m_completionEvent; }
  [[nodiscard]] uint64_t completedValue() const noexcept;

private:
  [[nodiscard]] MetalQueue& resolveQueue(QueueIdentity identity) const;

  MTL4::CommandQueue* m_queue{nullptr};
  MTL::SharedEvent* m_completionEvent{nullptr};
  QueueIdentity m_identity{};
  QueueInfo m_info{};
  std::array<MetalQueue*, 3> m_queues{};
  uint64_t m_lastSubmittedValue{0};
};

}  // namespace demo::rhi::metal
