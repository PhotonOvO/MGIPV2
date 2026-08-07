#pragma once

#include "../RHIQueue.h"

#include <array>
#include <cstdint>

namespace demo::rhi::d3d12 {

class D3D12Queue final : public Queue
{
public:
  D3D12Queue() = default;
  ~D3D12Queue() override;

  void init(void* nativeDevice, void* nativeQueue, uint32_t nativeType,
            QueueIdentity identity, QueueInfo info);
  void deinit();
  void setQueueRegistry(std::array<D3D12Queue*, 3> queues) noexcept;

  [[nodiscard]] QueueIdentity identity() const noexcept override;
  [[nodiscard]] QueueInfo info() const noexcept override;
  [[nodiscard]] SubmissionToken submit(const SubmitBatch& batch) override;
  [[nodiscard]] bool isComplete(SubmissionToken token) const override;
  void wait(SubmissionToken token) override;
  void waitIdle() override;
  [[nodiscard]] SubmissionToken lastSubmittedToken() const noexcept override;

  [[nodiscard]] void* nativeDevice() const noexcept { return m_device; }
  [[nodiscard]] uint32_t nativeType() const noexcept { return m_nativeType; }
  [[nodiscard]] uint64_t completedValue() const noexcept;

private:
  [[nodiscard]] D3D12Queue& resolveQueue(QueueIdentity identity) const;
  void recreateExhaustedTimeline();

  void* m_device{nullptr};
  void* m_queue{nullptr};
  void* m_fence{nullptr};
  void* m_fenceEvent{nullptr};
  uint32_t m_nativeType{0};
  QueueIdentity m_identity{};
  QueueInfo m_info{};
  std::array<D3D12Queue*, 3> m_queues{};
  uint64_t m_lastSubmittedValue{0};
};

}  // namespace demo::rhi::d3d12