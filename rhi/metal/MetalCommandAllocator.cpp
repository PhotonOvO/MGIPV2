#include "MetalCommandAllocator.h"

#include "MetalCommandBuffer.h"
#include "MetalCommon.h"
#include "MetalQueue.h"

#include "../RHIDebugCounters.h"

#include <Metal/MTL4ArgumentTable.hpp>
#include <Metal/MTL4CommandAllocator.hpp>
#include <Metal/MTLArgumentEncoder.hpp>
#include <Metal/MTLBuffer.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLIndirectCommandBuffer.hpp>
#include <Metal/MTLLibrary.hpp>
#include <Metal/MTLResidencySet.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace demo::rhi::metal {
namespace {

constexpr uint64_t kInitialRootScratchSize = 1u << 20u;

uint64_t alignUp(uint64_t value, uint64_t alignment)
{
  const uint64_t safeAlignment = std::max<uint64_t>(alignment, 1);
  return (value + safeAlignment - 1) & ~(safeAlignment - 1);
}

}  // namespace

MetalCommandAllocator::MetalCommandAllocator(
  MetalQueue& queue, MTL::Device* device, MetalResourceTable* resourceTable,
  MTL::ComputePipelineState* blitPipeline,
  MTL::ComputePipelineState* indexedIndirectUInt32Pipeline,
  MTL::ComputePipelineState* indexedIndirectUInt16Pipeline,
  MTL::Function* indexedIndirectFunction)
  : m_queue(queue), m_device(device), m_blitPipeline(blitPipeline),
    m_indexedIndirectUInt32Pipeline(indexedIndirectUInt32Pipeline),
    m_indexedIndirectUInt16Pipeline(indexedIndirectUInt16Pipeline),
    m_resourceTable(resourceTable)
{
  require(m_device != nullptr && m_blitPipeline != nullptr &&
            m_indexedIndirectUInt32Pipeline != nullptr &&
            m_indexedIndirectUInt16Pipeline != nullptr &&
            indexedIndirectFunction != nullptr &&
            m_resourceTable != nullptr,
          "MetalCommandAllocator requires an initialized device and resource table");
  m_allocator = m_device->newCommandAllocator();
  require(m_allocator != nullptr, "Metal failed to create MTL4::CommandAllocator");
  m_indexedIndirectArgumentEncoder = indexedIndirectFunction->newArgumentEncoder(0);
  if(m_indexedIndirectArgumentEncoder == nullptr)
  {
    m_allocator->release();
    m_allocator = nullptr;
    throw std::runtime_error("Metal failed to create the indexed-indirect argument encoder");
  }
}

MetalCommandAllocator::~MetalCommandAllocator()
{
  if(!m_commandBuffers.empty())
    std::terminate();
  {
    std::lock_guard lock(m_recordingMutex);
    if(m_recordingOwner != nullptr)
      std::terminate();
  }
  if(m_lastSubmission.isValid())
  {
    try
    {
      m_queue.wait(m_lastSubmission);
    }
    catch(...)
    {
    }
  }
  releaseSnapshots();
  releaseIndexedIndirectCommandBuffers();
  for(ScratchBuffer& scratch : m_scratchBuffers)
    scratch.buffer->release();
  m_scratchBuffers.clear();
  if(m_indexedIndirectArgumentEncoder != nullptr)
    m_indexedIndirectArgumentEncoder->release();
  if(m_allocator != nullptr)
    m_allocator->release();
}

QueueClass MetalCommandAllocator::queueClass() const noexcept
{
  return m_queue.info().queueClass;
}

void MetalCommandAllocator::reset(SubmissionToken completedToken)
{
  {
    std::lock_guard lock(m_recordingMutex);
    require(m_recordingOwner == nullptr,
            "MetalCommandAllocator::reset called while a command buffer is recording");
  }
  if(m_lastSubmission.isValid())
  {
    require(completedToken.isValid(),
            "MetalCommandAllocator::reset requires its completed submission token");
    require(completedToken.queue == m_lastSubmission.queue,
            "MetalCommandAllocator::reset rejected a wrong-queue token");
    require(completedToken.value >= m_lastSubmission.value,
            "MetalCommandAllocator::reset rejected a stale token");
    require(m_queue.isComplete(m_lastSubmission),
            "MetalCommandAllocator::reset called before GPU completion");
  }
  else
  {
    require(!completedToken.isValid() || completedToken.queue == m_queue.identity(),
            "MetalCommandAllocator::reset rejected a wrong-queue token");
  }

  m_allocator->reset();
  recycleSnapshots();
  recycleIndexedIndirectCommandBuffers();
  for(ScratchBuffer& scratch : m_scratchBuffers)
    scratch.offset = 0;
  for(MetalCommandBuffer* commandBuffer : m_commandBuffers)
    commandBuffer->markReusable();
  m_lastSubmission = {};
}

std::unique_ptr<CommandBuffer> MetalCommandAllocator::createCommandBuffer()
{
  auto commandBuffer = std::make_unique<MetalCommandBuffer>(*this, m_device->newCommandBuffer(), m_resourceTable);
  m_commandBuffers.push_back(commandBuffer.get());
  return commandBuffer;
}

MTL4::ArgumentTable* MetalCommandAllocator::allocateArgumentTable(
  uint32_t bufferCount, uint32_t textureCount, uint32_t samplerCount, const char* label)
{
  const auto reusable = std::find_if(
    m_argumentTableSnapshots.begin(), m_argumentTableSnapshots.end(),
    [bufferCount, textureCount, samplerCount](
      const ArgumentTableSnapshot& snapshot) {
      return !snapshot.inUse && snapshot.bufferCount == bufferCount &&
             snapshot.textureCount == textureCount &&
             snapshot.samplerCount == samplerCount;
    });
  if(reusable != m_argumentTableSnapshots.end())
  {
    for(uint32_t index = 0; index < bufferCount; ++index)
      reusable->table->setAddress(0, index);
    for(uint32_t index = 0; index < textureCount; ++index)
      reusable->table->setTexture(MTL::ResourceID{}, index);
    for(uint32_t index = 0; index < samplerCount; ++index)
      reusable->table->setSamplerState(MTL::ResourceID{}, index);
    reusable->inUse = true;
    return reusable->table;
  }

  auto* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
  descriptor->setMaxBufferBindCount(bufferCount);
  descriptor->setMaxTextureBindCount(textureCount);
  descriptor->setMaxSamplerStateBindCount(samplerCount);
  descriptor->setInitializeBindings(true);
  if(label != nullptr)
    descriptor->setLabel(nsString(label));

  NS::Error* error = nullptr;
  MTL4::ArgumentTable* table = m_device->newArgumentTable(descriptor, &error);
  descriptor->release();
  if(table == nullptr)
    throw std::runtime_error(errorMessage("MTLDevice::newArgumentTable", error));
  m_argumentTableSnapshots.push_back(ArgumentTableSnapshot{
    .table = table,
    .bufferCount = bufferCount,
    .textureCount = textureCount,
    .samplerCount = samplerCount,
    .inUse = true,
  });
  incrementHotPathCounter(BackendType::metal, HotPathCounter::tableVersionAllocations);
  return table;
}

MTL::IndirectCommandBuffer*
MetalCommandAllocator::acquireIndexedIndirectCommandBuffer(
  uint32_t maxCommandCount)
{
  require(maxCommandCount != 0,
          "Metal indexed-indirect command buffer capacity must not be zero");
  IndexedIndirectCommandBuffer* reusable = nullptr;
  for(IndexedIndirectCommandBuffer& entry : m_indexedIndirectCommandBuffers)
  {
    if(!entry.inUse && entry.capacity >= maxCommandCount &&
       (reusable == nullptr || entry.capacity < reusable->capacity))
    {
      reusable = &entry;
    }
  }
  if(reusable != nullptr)
  {
    reusable->inUse = true;
    return reusable->commandBuffer;
  }

  auto descriptor = NS::TransferPtr(
    MTL::IndirectCommandBufferDescriptor::alloc()->init());
  descriptor->setCommandTypes(MTL::IndirectCommandTypeDrawIndexed);
  descriptor->setInheritPipelineState(true);
  descriptor->setInheritBuffers(true);
  descriptor->setInheritCullMode(true);
  descriptor->setInheritDepthBias(true);
  descriptor->setInheritDepthClipMode(true);
  descriptor->setInheritDepthStencilState(true);
  descriptor->setInheritFrontFacingWinding(true);
  descriptor->setInheritTriangleFillMode(true);

  auto commandBuffer = NS::TransferPtr(m_device->newIndirectCommandBuffer(
    descriptor.get(), maxCommandCount,
    static_cast<MTL::ResourceOptions>(
      MTL::ResourceStorageModePrivate |
      MTL::ResourceHazardTrackingModeUntracked)));
  if(!commandBuffer)
    throw std::runtime_error("Metal failed to create an indexed indirect command buffer");
  commandBuffer->setLabel(nsString("Metal4 Slang indexed-indirect command buffer"));
  m_indexedIndirectCommandBuffers.push_back(IndexedIndirectCommandBuffer{
    .commandBuffer = commandBuffer.get(),
    .capacity = maxCommandCount,
    .inUse = true,
  });
  commandBuffer.detach();
  incrementHotPathCounter(
    BackendType::metal, HotPathCounter::commandRecordingNativeObjectCreations);
  return m_indexedIndirectCommandBuffers.back().commandBuffer;
}

MetalRootDataAllocation MetalCommandAllocator::allocateRootData(
  std::span<const std::byte> data, uint32_t alignment)
{
  require(!data.empty(), "Metal root-data allocation must not be empty");
  const MetalRootDataAllocation allocation = allocateScratchData(
    data.size_bytes(), alignment);
  std::memcpy(static_cast<std::byte*>(allocation.buffer->contents()) +
                allocation.offset,
              data.data(), data.size_bytes());
  return allocation;
}

MetalRootDataAllocation MetalCommandAllocator::allocateArgumentData(
  uint64_t size, uint32_t alignment)
{
  require(size != 0, "Metal argument-data allocation must not be empty");
  const MetalRootDataAllocation allocation = allocateScratchData(size, alignment);
  std::memset(static_cast<std::byte*>(allocation.buffer->contents()) +
                allocation.offset,
              0, static_cast<size_t>(size));
  return allocation;
}

void MetalCommandAllocator::appendScratchAllocations(
  std::vector<const MTL::Allocation*>& allocations) const
{
  for(const ScratchBuffer& scratch : m_scratchBuffers)
    allocations.push_back(scratch.buffer);
}

void MetalCommandAllocator::acquireRecording(
  MetalCommandBuffer& commandBuffer)
{
  std::lock_guard lock(m_recordingMutex);
  require(m_recordingOwner == nullptr,
          "MetalCommandAllocator supports only one recording command buffer");
  m_recordingOwner = &commandBuffer;
}

void MetalCommandAllocator::releaseRecording(
  MetalCommandBuffer& commandBuffer) noexcept
{
  std::lock_guard lock(m_recordingMutex);
  if(m_recordingOwner == &commandBuffer)
    m_recordingOwner = nullptr;
}

void MetalCommandAllocator::noteSubmitted(
  MetalCommandBuffer& commandBuffer, SubmissionToken token)
{
  require(std::find(m_commandBuffers.begin(), m_commandBuffers.end(), &commandBuffer) !=
            m_commandBuffers.end(),
          "MetalCommandAllocator received an unknown command buffer");
  require(token.queue == m_queue.identity(),
          "MetalCommandAllocator received a wrong-queue submission token");
  if(!m_lastSubmission.isValid() || token.value > m_lastSubmission.value)
    m_lastSubmission = token;
}

void MetalCommandAllocator::releaseCommandBuffer(MetalCommandBuffer& commandBuffer) noexcept
{
  releaseRecording(commandBuffer);
  const auto found = std::find(m_commandBuffers.begin(), m_commandBuffers.end(), &commandBuffer);
  if(found != m_commandBuffers.end())
    m_commandBuffers.erase(found);
}

MetalCommandAllocator::ScratchBuffer& MetalCommandAllocator::acquireScratch(
  uint64_t size, uint32_t alignment)
{
  for(ScratchBuffer& scratch : m_scratchBuffers)
  {
    if(alignUp(scratch.offset, alignment) + size <= scratch.capacity)
      return scratch;
  }

  const uint64_t capacity = std::max(kInitialRootScratchSize, alignUp(size, 4096));
  MTL::Buffer* buffer = m_device->newBuffer(
    capacity, static_cast<MTL::ResourceOptions>(MTL::ResourceStorageModeShared |
                                                MTL::ResourceHazardTrackingModeUntracked));
  require(buffer != nullptr, "Metal failed to allocate root-constant scratch buffer");
  buffer->setLabel(nsString("Metal4 root data scratch"));
  m_scratchBuffers.push_back(ScratchBuffer{buffer, capacity, 0});
  return m_scratchBuffers.back();
}

MetalRootDataAllocation MetalCommandAllocator::allocateScratchData(
  uint64_t size, uint32_t alignment)
{
  const uint32_t safeAlignment = std::max(alignment, 16u);
  ScratchBuffer& scratch = acquireScratch(size, safeAlignment);
  scratch.offset = alignUp(scratch.offset, safeAlignment);
  const MetalRootDataAllocation allocation{
    .buffer = scratch.buffer,
    .offset = scratch.offset,
    .gpuAddress = scratch.buffer->gpuAddress() + scratch.offset,
  };
  scratch.offset += size;
  return allocation;
}

void MetalCommandAllocator::releaseSnapshots() noexcept
{
  for(const ArgumentTableSnapshot& snapshot : m_argumentTableSnapshots)
    snapshot.table->release();
  m_argumentTableSnapshots.clear();
}

void MetalCommandAllocator::recycleSnapshots() noexcept
{
  for(ArgumentTableSnapshot& snapshot : m_argumentTableSnapshots)
    snapshot.inUse = false;
}

void MetalCommandAllocator::releaseIndexedIndirectCommandBuffers() noexcept
{
  for(const IndexedIndirectCommandBuffer& entry :
      m_indexedIndirectCommandBuffers)
  {
    entry.commandBuffer->release();
  }
  m_indexedIndirectCommandBuffers.clear();
}

void MetalCommandAllocator::recycleIndexedIndirectCommandBuffers() noexcept
{
  for(IndexedIndirectCommandBuffer& entry : m_indexedIndirectCommandBuffers)
    entry.inUse = false;
}

}  // namespace demo::rhi::metal
