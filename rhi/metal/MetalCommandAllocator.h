#pragma once

#include "../RHICommandAllocator.h"
#include "../RHITypes.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace MTL {
class Allocation;
class ArgumentEncoder;
class Buffer;
class ComputePipelineState;
class Device;
class Function;
class IndirectCommandBuffer;
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
                        MTL::ComputePipelineState* blitPipeline,
                        MTL::ComputePipelineState* indexedIndirectUInt32Pipeline,
                        MTL::ComputePipelineState* indexedIndirectUInt16Pipeline,
                        MTL::Function* indexedIndirectFunction);
  ~MetalCommandAllocator() override;

  [[nodiscard]] QueueClass queueClass() const noexcept override;
  void reset(SubmissionToken completedToken = {}) override;
  [[nodiscard]] std::unique_ptr<CommandBuffer> createCommandBuffer() override;

  [[nodiscard]] MTL4::ArgumentTable* allocateArgumentTable(
    uint32_t bufferCount, uint32_t textureCount, uint32_t samplerCount,
    const char* label = nullptr);
  [[nodiscard]] MTL::IndirectCommandBuffer* acquireIndexedIndirectCommandBuffer(
    uint32_t maxCommandCount);
  [[nodiscard]] MetalRootDataAllocation allocateRootData(
    std::span<const std::byte> data, uint32_t alignment);
  [[nodiscard]] MetalRootDataAllocation allocateArgumentData(
    uint64_t size, uint32_t alignment);
  void appendScratchAllocations(
    std::vector<const MTL::Allocation*>& allocations) const;

  void acquireRecording(MetalCommandBuffer& commandBuffer);
  void releaseRecording(MetalCommandBuffer& commandBuffer) noexcept;
  void noteSubmitted(MetalCommandBuffer& commandBuffer, SubmissionToken token);
  void releaseCommandBuffer(MetalCommandBuffer& commandBuffer) noexcept;

  [[nodiscard]] MetalQueue& queue() const noexcept { return m_queue; }
  [[nodiscard]] MTL::Device* device() const noexcept { return m_device; }
  [[nodiscard]] MTL::ComputePipelineState* blitPipeline() const noexcept
  {
    return m_blitPipeline;
  }
  [[nodiscard]] MTL::ComputePipelineState* indexedIndirectPipeline(
    IndexFormat format) const noexcept
  {
    return format == IndexFormat::uint16
      ? m_indexedIndirectUInt16Pipeline
      : m_indexedIndirectUInt32Pipeline;
  }
  [[nodiscard]] MTL::ArgumentEncoder* indexedIndirectArgumentEncoder() const noexcept
  {
    return m_indexedIndirectArgumentEncoder;
  }
  [[nodiscard]] MTL4::CommandAllocator* nativeAllocator() const noexcept { return m_allocator; }
  [[nodiscard]] MetalResourceTable* resourceTable() const noexcept { return m_resourceTable; }

private:
  struct ArgumentTableSnapshot
  {
    MTL4::ArgumentTable* table{nullptr};
    uint32_t bufferCount{0};
    uint32_t textureCount{0};
    uint32_t samplerCount{0};
    bool inUse{false};
  };

  struct ScratchBuffer
  {
    MTL::Buffer* buffer{nullptr};
    uint64_t capacity{0};
    uint64_t offset{0};
  };

  struct IndexedIndirectCommandBuffer
  {
    MTL::IndirectCommandBuffer* commandBuffer{nullptr};
    uint32_t capacity{0};
    bool inUse{false};
  };

  [[nodiscard]] ScratchBuffer& acquireScratch(uint64_t size, uint32_t alignment);
  [[nodiscard]] MetalRootDataAllocation allocateScratchData(
    uint64_t size, uint32_t alignment);
  void recycleSnapshots() noexcept;
  void releaseSnapshots() noexcept;
  void recycleIndexedIndirectCommandBuffers() noexcept;
  void releaseIndexedIndirectCommandBuffers() noexcept;

  MetalQueue& m_queue;
  MTL::Device* m_device{nullptr};
  MTL::ComputePipelineState* m_blitPipeline{nullptr};
  MTL::ComputePipelineState* m_indexedIndirectUInt32Pipeline{nullptr};
  MTL::ComputePipelineState* m_indexedIndirectUInt16Pipeline{nullptr};
  MTL::ArgumentEncoder* m_indexedIndirectArgumentEncoder{nullptr};
  MTL4::CommandAllocator* m_allocator{nullptr};
  MetalResourceTable* m_resourceTable{nullptr};
  std::vector<MetalCommandBuffer*> m_commandBuffers;
  std::vector<ArgumentTableSnapshot> m_argumentTableSnapshots;
  std::vector<IndexedIndirectCommandBuffer> m_indexedIndirectCommandBuffers;
  std::vector<ScratchBuffer> m_scratchBuffers;
  SubmissionToken m_lastSubmission{};
  std::mutex m_recordingMutex;
  MetalCommandBuffer* m_recordingOwner{nullptr};
};

}  // namespace demo::rhi::metal
