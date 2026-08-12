#pragma once

#include "../RHICommandAllocator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MTL {
class Buffer;
class ComputePipelineState;
class Device;
class ResidencySet;
}

namespace MTL4 {
class ArgumentTable;
class CommandAllocator;
}

namespace demo::rhi::metal {

class MetalCommandBuffer;
class MetalQueue;
class MetalResourceTable;

struct MetalRootDataAllocation
{
  MTL::Buffer* buffer{nullptr};
  uint64_t offset{0};
  uint64_t gpuAddress{0};
};

class MetalCommandAllocator final : public CommandAllocator
{
public:
  MetalCommandAllocator(MetalQueue& queue, MTL::Device* device,
                        MetalResourceTable* resourceTable,
                        MTL::ComputePipelineState* blitPipeline);
  ~MetalCommandAllocator() override;

  [[nodiscard]] QueueClass queueClass() const noexcept override;
  void reset(SubmissionToken completedToken = {}) override;
  [[nodiscard]] std::unique_ptr<CommandBuffer> createCommandBuffer() override;

  [[nodiscard]] MTL4::ArgumentTable* allocateArgumentTable(
    uint32_t bufferCount, uint32_t textureCount, uint32_t samplerCount,
    const char* label = nullptr);
  [[nodiscard]] MetalRootDataAllocation allocateRootData(
    std::span<const std::byte> data, uint32_t alignment);
  void addScratchAllocations(MTL::ResidencySet& residencySet) const;

  void noteSubmitted(MetalCommandBuffer& commandBuffer, SubmissionToken token);
  void releaseCommandBuffer(MetalCommandBuffer& commandBuffer) noexcept;

  [[nodiscard]] MetalQueue& queue() const noexcept { return m_queue; }
  [[nodiscard]] MTL::Device* device() const noexcept { return m_device; }
  [[nodiscard]] MTL::ComputePipelineState* blitPipeline() const noexcept
  {
    return m_blitPipeline;
  }
  [[nodiscard]] MTL4::CommandAllocator* nativeAllocator() const noexcept { return m_allocator; }
  [[nodiscard]] MetalResourceTable* resourceTable() const noexcept { return m_resourceTable; }

private:
  struct ScratchBuffer
  {
    MTL::Buffer* buffer{nullptr};
    uint64_t capacity{0};
    uint64_t offset{0};
  };

  [[nodiscard]] ScratchBuffer& acquireScratch(uint64_t size, uint32_t alignment);
  void releaseSnapshots() noexcept;

  MetalQueue& m_queue;
  MTL::Device* m_device{nullptr};
  MTL::ComputePipelineState* m_blitPipeline{nullptr};
  MTL4::CommandAllocator* m_allocator{nullptr};
  MetalResourceTable* m_resourceTable{nullptr};
  std::vector<MetalCommandBuffer*> m_commandBuffers;
  std::vector<MTL4::ArgumentTable*> m_argumentTableSnapshots;
  std::vector<ScratchBuffer> m_scratchBuffers;
  SubmissionToken m_lastSubmission{};
};

}  // namespace demo::rhi::metal
