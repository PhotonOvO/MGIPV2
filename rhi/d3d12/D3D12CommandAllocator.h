#pragma once

#include "../RHICommandAllocator.h"

#include <vector>

namespace demo::rhi::d3d12 {

class D3D12CommandBuffer;
class D3D12Device;
class D3D12Queue;

class D3D12CommandAllocator final : public CommandAllocator
{
public:
  D3D12CommandAllocator(D3D12Device& owner, D3D12Queue& queue);
  ~D3D12CommandAllocator() override;

  [[nodiscard]] QueueClass queueClass() const noexcept override;
  void reset(SubmissionToken completedToken = {}) override;
  [[nodiscard]] std::unique_ptr<CommandBuffer> createCommandBuffer() override;

  void noteSubmitted(D3D12CommandBuffer& commandBuffer, SubmissionToken token);
  void releaseCommandBuffer(D3D12CommandBuffer& commandBuffer,
                            void* nativeCommandList) noexcept;
  [[nodiscard]] void* nativeAllocator() const noexcept { return m_allocator; }

private:
  D3D12Device& m_owner;
  D3D12Queue& m_queue;
  void* m_allocator{nullptr};
  std::vector<D3D12CommandBuffer*> m_commandBuffers;
  SubmissionToken m_lastSubmission{};
};

}  // namespace demo::rhi::d3d12
