#include "UploadManager.h"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace demo {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

}  // namespace

UploadManager::~UploadManager()
{
  shutdown();
}

void UploadManager::init(
  rhi::Device& device, uint32_t slotCount, uint64_t stagingCapacityBytes)
{
  require(m_device == nullptr, "UploadManager::init called twice");
  require(slotCount != 0, "UploadManager::init requires slots");
  require(stagingCapacityBytes != 0,
          "UploadManager::init requires a nonzero staging capacity");
  m_device = &device;
  rhi::Queue* graphics = device.getQueue(rhi::QueueClass::graphics);
  initChannel(m_graphics, device, graphics,
              rhi::QueueClass::graphics, slotCount, stagingCapacityBytes);

  rhi::Queue* transfer = device.getQueue(rhi::QueueClass::transfer);
  // D3D12 copy queues return resources through COMMON after a blocking upload.
  // Vulkan uses a transfer wrapper only when it shares the graphics family;
  // dedicated-family ownership needs paired release/acquire barriers.
  const bool canUseTransfer = transfer != nullptr && transfer->info().isValid() &&
    (device.getBackendInfo().type == rhi::BackendType::d3d12 ||
     !transfer->info().dedicated);
  if(canUseTransfer)
    initChannel(m_transfer, device, transfer,
                rhi::QueueClass::transfer, slotCount, stagingCapacityBytes);
}

void UploadManager::initChannel(
  Channel& channel, rhi::Device& device, rhi::Queue* queue,
  rhi::QueueClass queueClass, uint32_t slotCount,
  uint64_t stagingCapacityBytes)
{
  channel.queue = queue;
  channel.queueClass = queueClass;
  require(channel.queue != nullptr,
          "UploadManager::initChannel requires an available queue");
  channel.slots.reserve(slotCount);
  for(uint32_t index = 0; index < slotCount; ++index)
  {
    Slot slot;
    slot.allocator = device.createCommandAllocator(queueClass);
    require(slot.allocator != nullptr,
            "UploadManager failed to create command allocator");
    slot.commandBuffer = slot.allocator->createCommandBuffer();
    require(slot.commandBuffer != nullptr,
            "UploadManager failed to create command buffer");
    ensureStagingCapacity(slot, stagingCapacityBytes);
    channel.slots.push_back(std::move(slot));
  }
}

void UploadManager::shutdown()
{
  if(m_graphics.queue != nullptr || m_transfer.queue != nullptr)
  {
    try
    {
      flush(true);
    }
    catch(...)
    {
    }
  }
  destroyChannel(m_transfer);
  destroyChannel(m_graphics);
  m_device = nullptr;
}

void UploadManager::destroyChannel(Channel& channel)
{
  if(m_device != nullptr)
  {
    for(Slot& slot : channel.slots)
    {
      if(slot.stagingBuffer.isValid())
      {
        if(slot.stagingMapped != nullptr)
          m_device->unmapBuffer(slot.stagingBuffer);
        m_device->destroyBuffer(slot.stagingBuffer);
      }
      slot.stagingMapped = nullptr;
      slot.stagingBuffer = {};
      slot.stagingCapacity = 0;
    }
  }
  channel = {};
}

void UploadManager::execute(
  const std::function<void(rhi::CommandBuffer&)>& record)
{
  executeOn(m_graphics, record, "UploadManager.Graphics", false);
}

void UploadManager::executeTransfer(
  const std::function<void(rhi::CommandBuffer&)>& record)
{
  Channel& channel = m_transfer.queue != nullptr ? m_transfer : m_graphics;
  executeOn(channel, record,
            channel.queueClass == rhi::QueueClass::transfer
              ? "UploadManager.Transfer"
              : "UploadManager.TransferFallback",
            channel.queueClass == rhi::QueueClass::transfer);
}

UploadManager::Slot& UploadManager::prepareSlot(Channel& channel)
{
  require(m_device != nullptr && channel.queue != nullptr && !channel.slots.empty(),
          "UploadManager requires initialization");
  Slot& slot = channel.slots[channel.nextSlot];
  if(slot.submission.isValid())
    channel.queue->wait(slot.submission);
  return slot;
}

void UploadManager::ensureStagingCapacity(
  Slot& slot, uint64_t requiredCapacity)
{
  if(slot.stagingCapacity >= requiredCapacity)
    return;
  require(m_device != nullptr, "UploadManager staging requires a device");
  require(requiredCapacity <= (std::numeric_limits<uint64_t>::max)() / 2u,
          "UploadManager staging request is too large");

  if(slot.stagingBuffer.isValid())
  {
    if(slot.stagingMapped != nullptr)
      m_device->unmapBuffer(slot.stagingBuffer);
    m_device->destroyBuffer(slot.stagingBuffer);
    slot.stagingBuffer = {};
    slot.stagingMapped = nullptr;
    slot.stagingCapacity = 0;
  }

  uint64_t capacity = 256;
  while(capacity < requiredCapacity)
    capacity *= 2u;
  slot.stagingBuffer = m_device->createBuffer(rhi::BufferDesc{
    .size = capacity,
    .usage = rhi::BufferUsageFlags::transferSrc,
    .memoryUsage = rhi::MemoryUsage::cpuToGpu,
    .debugName = "UploadManager staging slot",
  });
  require(slot.stagingBuffer.isValid(),
          "UploadManager failed to create a staging buffer");
  slot.stagingMapped = m_device->mapBuffer(slot.stagingBuffer);
  require(slot.stagingMapped != nullptr,
          "UploadManager failed to map a staging buffer");
  slot.stagingCapacity = capacity;
}

void UploadManager::submitSlot(
  Channel& channel, Slot& slot, const char* debugLabel, bool waitAfterSubmit)
{
  slot.commandBuffer->end();
  std::array<rhi::CommandBuffer*, 1> commandBuffers{slot.commandBuffer.get()};
  slot.submission = channel.queue->submit(rhi::SubmitBatch{
    .commandBuffers = commandBuffers,
    .debugLabel = debugLabel,
  });
  if(waitAfterSubmit)
    channel.queue->wait(slot.submission);
  channel.nextSlot = (channel.nextSlot + 1u)
    % static_cast<uint32_t>(channel.slots.size());
}

void UploadManager::executeOn(
  Channel& channel,
  const std::function<void(rhi::CommandBuffer&)>& record,
  const char* debugLabel, bool waitAfterSubmit)
{
  require(static_cast<bool>(record),
          "UploadManager::execute requires a recording callback");
  Slot& slot = prepareSlot(channel);
  slot.allocator->reset(slot.submission);
  slot.commandBuffer->begin(*slot.allocator);
  record(*slot.commandBuffer);
  submitSlot(channel, slot, debugLabel, waitAfterSubmit);
}

void UploadManager::stageAndExecuteTransfer(
  std::span<const std::byte> data,
  const std::function<void(rhi::CommandBuffer&, const StagingSlice&)>& record)
{
  require(!data.empty(), "UploadManager staging requires data");
  require(static_cast<bool>(record),
          "UploadManager staging requires a recording callback");
  Channel& channel = m_transfer.queue != nullptr ? m_transfer : m_graphics;
  Slot& slot = prepareSlot(channel);
  ensureStagingCapacity(slot, data.size());
  std::memcpy(slot.stagingMapped, data.data(), data.size());
  const rhi::RHIResult flush =
    m_device->flushMappedBufferRange(slot.stagingBuffer, 0, data.size());
  require(flush || flush.error.code == rhi::RHIErrorCode::unsupported,
          "UploadManager failed to flush staging data");

  slot.allocator->reset(slot.submission);
  slot.commandBuffer->begin(*slot.allocator);
  record(*slot.commandBuffer,
         StagingSlice{slot.stagingBuffer, 0, data.size()});
  submitSlot(
    channel, slot,
    channel.queueClass == rhi::QueueClass::transfer
      ? "UploadManager.StagedTransfer"
      : "UploadManager.StagedTransferFallback",
    channel.queueClass == rhi::QueueClass::transfer);
}

void UploadManager::flush(bool waitForCompletion)
{
  if(m_graphics.queue == nullptr && m_transfer.queue == nullptr)
    return;

  flushChannel(m_graphics, waitForCompletion);
  flushChannel(m_transfer, waitForCompletion);
  if(m_device != nullptr)
    m_device->collectGarbage();
}

void UploadManager::flushChannel(Channel& channel, bool waitForCompletion)
{
  if(channel.queue == nullptr || !waitForCompletion)
    return;
  for(const Slot& slot : channel.slots)
  {
    if(slot.submission.isValid())
      channel.queue->wait(slot.submission);
  }
}

rhi::QueueClass UploadManager::transferExecutionQueueClass() const noexcept
{
  return m_transfer.queue != nullptr
    ? rhi::QueueClass::transfer
    : rhi::QueueClass::graphics;
}
}  // namespace demo