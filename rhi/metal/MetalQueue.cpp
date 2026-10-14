#include "MetalQueue.h"

#include "MetalCommandBuffer.h"

#include "../RHIDebugCounters.h"

#include <Metal/MTL4CommandQueue.hpp>
#include <Metal/MTLEvent.hpp>

#include <limits>
#include <stdexcept>
#include <vector>

namespace demo::rhi::metal {

MetalQueue::~MetalQueue()
{
  deinit();
}

void MetalQueue::init(MTL4::CommandQueue* queue, MTL::SharedEvent* completionEvent,
                      QueueIdentity identity, QueueInfo info)
{
  if(m_queue != nullptr || queue == nullptr || completionEvent == nullptr || !identity.isValid())
    throw std::runtime_error("MetalQueue::init received invalid state or native objects");
  m_queue = queue;
  m_completionEvent = completionEvent;
  m_identity = identity;
  m_info = info;
}

void MetalQueue::deinit()
{
  if(m_queue == nullptr)
    return;
  try
  {
    waitIdle();
  }
  catch(...)
  {
  }
  m_completionEvent->release();
  m_queue->release();
  m_completionEvent = nullptr;
  m_queue = nullptr;
  m_identity = {};
  m_info = {};
  m_queues = {};
  m_lastSubmittedValue = 0;
}

void MetalQueue::setQueueRegistry(std::array<MetalQueue*, 3> queues) noexcept
{
  m_queues = queues;
}

QueueIdentity MetalQueue::identity() const noexcept
{
  return m_identity;
}

QueueInfo MetalQueue::info() const noexcept
{
  return m_info;
}

SubmissionToken MetalQueue::submit(const SubmitBatch& batch)
{
  if(m_queue == nullptr)
    throw std::runtime_error("MetalQueue::submit called before initialization");
  if(batch.commandBuffers.empty())
    throw std::invalid_argument("MetalQueue::submit requires at least one command buffer");
  if(!batch.signalPoints.empty())
    throw std::runtime_error("Metal 4 external QueueSyncHandle signals are not implemented");

  for(const SubmitWaitPoint& waitPoint : batch.waitPoints)
  {
    if(!waitPoint.isValid())
      throw std::invalid_argument("MetalQueue::submit received an invalid wait point");
    if(waitPoint.external.isValid())
      throw std::runtime_error("Metal 4 external QueueSyncHandle waits are not implemented");
    MetalQueue& producer = resolveQueue(waitPoint.submission.queue);
    if(&producer != this || waitPoint.submission.value > completedValue())
      m_queue->wait(producer.completionEvent(), waitPoint.submission.value);
  }

  std::vector<const MTL4::CommandBuffer*> nativeBuffers;
  nativeBuffers.reserve(batch.commandBuffers.size());
  MTL::Drawable* drawable = nullptr;
  for(CommandBuffer* commandBuffer : batch.commandBuffers)
  {
    auto* metalBuffer = dynamic_cast<MetalCommandBuffer*>(commandBuffer);
    if(metalBuffer == nullptr)
      throw std::invalid_argument("MetalQueue::submit received a command buffer from another backend");
    metalBuffer->validateForSubmit(*this);
    nativeBuffers.push_back(metalBuffer->nativeHandle());
    if(MTL::Drawable* pendingDrawable = metalBuffer->drawableForPresent();
       pendingDrawable != nullptr)
    {
      if(drawable != nullptr)
        throw std::invalid_argument("Metal submission contains multiple drawable presents");
      if(!m_info.capabilities.supportsPresent)
        throw std::runtime_error("Metal drawable present requires a present-capable queue");
      drawable = pendingDrawable;
    }
  }

  const uint64_t submissionValue = ++m_lastSubmittedValue;
  m_queue->commit(nativeBuffers.data(), nativeBuffers.size());
  if(drawable != nullptr)
    m_queue->signalDrawable(drawable);
  m_queue->signalEvent(m_completionEvent, submissionValue);

  const SubmissionToken token{m_identity, submissionValue};
  for(CommandBuffer* commandBuffer : batch.commandBuffers)
    static_cast<MetalCommandBuffer*>(commandBuffer)->markSubmitted(token);

  incrementHotPathCounter(BackendType::metal, HotPathCounter::queueSubmits);
  incrementHotPathCounter(BackendType::metal, HotPathCounter::submittedCommandBuffers,
                          batch.commandBuffers.size());
  return token;
}

bool MetalQueue::isComplete(SubmissionToken token) const
{
  if(!token.isValid())
    return true;
  const MetalQueue& queue = resolveQueue(token.queue);
  return queue.completedValue() >= token.value;
}

void MetalQueue::wait(SubmissionToken token)
{
  if(!token.isValid())
    return;
  MetalQueue& queue = resolveQueue(token.queue);
  if(queue.completedValue() >= token.value)
    return;
  if(!queue.m_completionEvent->waitUntilSignaledValue(
       token.value, std::numeric_limits<uint64_t>::max()))
  {
    throw std::runtime_error("Metal shared-event wait timed out");
  }
}

void MetalQueue::waitIdle()
{
  if(m_lastSubmittedValue != 0)
    wait(SubmissionToken{m_identity, m_lastSubmittedValue});
}

SubmissionToken MetalQueue::lastSubmittedToken() const noexcept
{
  return m_lastSubmittedValue == 0
    ? SubmissionToken{}
    : SubmissionToken{m_identity, m_lastSubmittedValue};
}

void MetalQueue::waitDrawable(MTL::Drawable* drawable)
{
  if(m_queue == nullptr || drawable == nullptr)
    throw std::invalid_argument("MetalQueue::waitDrawable requires a queue and drawable");
  m_queue->wait(drawable);
}

uint64_t MetalQueue::completedValue() const noexcept
{
  return m_completionEvent != nullptr ? m_completionEvent->signaledValue() : 0;
}

MetalQueue& MetalQueue::resolveQueue(QueueIdentity identity) const
{
  for(MetalQueue* queue : m_queues)
  {
    if(queue != nullptr && queue->identity() == identity)
      return *queue;
  }
  throw std::invalid_argument("Metal submission token belongs to an unknown queue");
}

}  // namespace demo::rhi::metal
