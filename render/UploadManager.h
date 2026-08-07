#pragma once

#include "../rhi/RHICommandAllocator.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIQueue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace demo {

class UploadManager
{
public:
  struct StagingSlice
  {
    rhi::BufferHandle buffer{};
    uint64_t offset{0};
    uint64_t size{0};
  };

  UploadManager() = default;
  ~UploadManager();

  UploadManager(const UploadManager&) = delete;
  UploadManager& operator=(const UploadManager&) = delete;

  void init(rhi::Device& device, uint32_t slotCount,
            uint64_t stagingCapacityBytes = 1024u * 1024u);
  void shutdown();
  void execute(const std::function<void(rhi::CommandBuffer&)>& record);
  void executeTransfer(const std::function<void(rhi::CommandBuffer&)>& record);
  void stageAndExecuteTransfer(
    std::span<const std::byte> data,
    const std::function<void(rhi::CommandBuffer&, const StagingSlice&)>& record);
  void flush(bool waitForCompletion);

  [[nodiscard]] rhi::QueueClass transferExecutionQueueClass() const noexcept;

private:
  struct Slot
  {
    std::unique_ptr<rhi::CommandAllocator> allocator;
    std::unique_ptr<rhi::CommandBuffer> commandBuffer;
    rhi::SubmissionToken submission{};
    rhi::BufferHandle stagingBuffer{};
    void* stagingMapped{nullptr};
    uint64_t stagingCapacity{0};
  };

  struct Channel
  {
    rhi::Queue* queue{nullptr};
    rhi::QueueClass queueClass{rhi::QueueClass::graphics};
    std::vector<Slot> slots;
    uint32_t nextSlot{0};
  };

  void initChannel(Channel& channel, rhi::Device& device, rhi::Queue* queue,
                   rhi::QueueClass queueClass, uint32_t slotCount,
                   uint64_t stagingCapacityBytes);
  void destroyChannel(Channel& channel);
  Slot& prepareSlot(Channel& channel);
  void ensureStagingCapacity(Slot& slot, uint64_t requiredCapacity);
  void submitSlot(Channel& channel, Slot& slot, const char* debugLabel,
                  bool waitAfterSubmit);
  void executeOn(Channel& channel,
                 const std::function<void(rhi::CommandBuffer&)>& record,
                 const char* debugLabel, bool waitAfterSubmit);
  static void flushChannel(Channel& channel, bool waitForCompletion);

  rhi::Device* m_device{nullptr};
  Channel m_graphics;
  Channel m_transfer;
};

}  // namespace demo