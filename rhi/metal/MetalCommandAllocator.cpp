#include "MetalCommandAllocator.h"

#include "MetalCommandBuffer.h"
#include "MetalCommon.h"
#include "MetalQueue.h"

#include "../RHIDebugCounters.h"

#include <Metal/MTL4ArgumentTable.hpp>
#include <Metal/MTL4CommandAllocator.hpp>
#include <Metal/MTLBuffer.hpp>
#include <Metal/MTLDevice.hpp>
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
  MTL::ComputePipelineState* blitPipeline)
  : m_queue(queue), m_device(device), m_blitPipeline(blitPipeline),
    m_resourceTable(resourceTable)
{
  require(m_device != nullptr && m_blitPipeline != nullptr &&
            m_resourceTable != nullptr,
          "MetalCommandAllocator requires an initialized device and resource table");
  m_allocator = m_device->newCommandAllocator();
  require(m_allocator != nullptr, "Metal failed to create MTL4::CommandAllocator");
}

MetalCommandAllocator::~MetalCommandAllocator()
{
  if(!m_commandBuffers.empty())
    std::terminate();
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
  for(ScratchBuffer& scratch : m_scratchBuffers)
    scratch.buffer->release();
  m_scratchBuffers.clear();
  if(m_allocator != nullptr)
    m_allocator->release();
}

QueueClass MetalCommandAllocator::queueClass() const noexcept
{
  return m_queue.info().queueClass;
}

void MetalCommandAllocator::reset(SubmissionToken completedToken)
{
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
  releaseSnapshots();
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
  m_argumentTableSnapshots.push_back(table);
  incrementHotPathCounter(BackendType::metal, HotPathCounter::tableVersionAllocations);
  return table;
}

MetalRootDataAllocation MetalCommandAllocator::allocateRootData(
  std::span<const std::byte> data, uint32_t alignment)
{
  require(!data.empty(), "Metal root-data allocation must not be empty");
  ScratchBuffer& scratch = acquireScratch(data.size_bytes(), std::max(alignment, 16u));
  scratch.offset = alignUp(scratch.offset, std::max(alignment, 16u));
  std::memcpy(static_cast<std::byte*>(scratch.buffer->contents()) + scratch.offset,
              data.data(), data.size_bytes());
  const MetalRootDataAllocation allocation{
    .buffer = scratch.buffer,
    .offset = scratch.offset,
    .gpuAddress = scratch.buffer->gpuAddress() + scratch.offset,
  };
  scratch.offset += data.size_bytes();
  return allocation;
}

void MetalCommandAllocator::addScratchAllocations(
  MTL::ResidencySet& residencySet) const
{
  for(const ScratchBuffer& scratch : m_scratchBuffers)
    residencySet.addAllocation(scratch.buffer);
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

void MetalCommandAllocator::releaseSnapshots() noexcept
{
  for(MTL4::ArgumentTable* table : m_argumentTableSnapshots)
    table->release();
  m_argumentTableSnapshots.clear();
}

}  // namespace demo::rhi::metal
