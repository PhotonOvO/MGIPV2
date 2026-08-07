#include "FrameScheduler.h"
#include "../common/ProfilerMarkers.h"

#include <array>
#include <stdexcept>

namespace demo {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

}  // namespace

FrameScheduler::~FrameScheduler()
{
  shutdown();
}

void FrameScheduler::init(rhi::Device& device, uint32_t requestedFrameCount)
{
  VKDEMO_CPU_SCOPE("RHI.FrameScheduler.Init");
  require(m_device == nullptr, "FrameScheduler::init called twice");
  require(requestedFrameCount != 0, "FrameScheduler::init requires frames");

  rhi::Queue* graphicsQueue = device.getQueue(rhi::QueueClass::graphics);
  require(graphicsQueue != nullptr && graphicsQueue->info().isValid(),
          "FrameScheduler::init requires an available graphics queue");

  m_device = &device;
  m_queue = graphicsQueue;
  m_frames.reserve(requestedFrameCount);
  for(uint32_t index = 0; index < requestedFrameCount; ++index)
  {
    FrameSlot slot;
    slot.allocator =
      device.createCommandAllocator(rhi::QueueClass::graphics);
    require(slot.allocator != nullptr,
            "FrameScheduler failed to create a command allocator");
    slot.commandBuffer = slot.allocator->createCommandBuffer();
    require(slot.commandBuffer != nullptr,
            "FrameScheduler failed to create a command buffer");
    m_frames.push_back(std::move(slot));
  }
}

void FrameScheduler::shutdown()
{
  VKDEMO_CPU_SCOPE("RHI.FrameScheduler.Shutdown");
  if(m_queue != nullptr)
  {
    try
    {
      waitForAllFrames();
    }
    catch(...)
    {
    }
  }
  m_recording = false;
  m_frames.clear();
  m_queue = nullptr;
  m_device = nullptr;
  m_currentFrameIndex = 0;
}

rhi::CommandBuffer& FrameScheduler::beginFrame()
{
  VKDEMO_CPU_SCOPE("RHI.Frame.Begin");
  require(m_device != nullptr && m_queue != nullptr && !m_frames.empty(),
          "FrameScheduler::beginFrame requires initialization");
  require(!m_recording,
          "FrameScheduler::beginFrame called while recording");

  FrameSlot& frame = m_frames[m_currentFrameIndex];
  if(frame.submission.isValid())
    m_queue->wait(frame.submission);
  frame.allocator->reset(frame.submission);
  m_device->collectGarbage();
  frame.commandBuffer->begin(*frame.allocator);
  m_recording = true;
  return *frame.commandBuffer;
}

rhi::SubmissionToken FrameScheduler::submitFrame(
  rhi::CommandBuffer& commandBuffer,
  rhi::SubmitWaitPoint waitPoint,
  rhi::SubmitSignalPoint signalPoint)
{
  VKDEMO_CPU_SCOPE("RHI.Frame.Submit");
  require(m_recording,
          "FrameScheduler::submitFrame called outside a frame");
  FrameSlot& frame = m_frames[m_currentFrameIndex];
  require(frame.commandBuffer.get() == &commandBuffer,
          "FrameScheduler::submitFrame received the wrong command buffer");

  commandBuffer.end();

  std::array<rhi::CommandBuffer*, 1> commandBuffers{&commandBuffer};
  std::array<rhi::SubmitWaitPoint, 1> waitPoints{waitPoint};
  std::array<rhi::SubmitSignalPoint, 1> signalPoints{signalPoint};
  const rhi::SubmitBatch batch{
    .commandBuffers = commandBuffers,
    .waitPoints = waitPoint.isValid()
      ? std::span<const rhi::SubmitWaitPoint>{waitPoints}
      : std::span<const rhi::SubmitWaitPoint>{},
    .signalPoints = signalPoint.isValid()
      ? std::span<const rhi::SubmitSignalPoint>{signalPoints}
      : std::span<const rhi::SubmitSignalPoint>{},
    .debugLabel = "FrameScheduler.Graphics",
  };
  frame.submission = m_queue->submit(batch);
  m_recording = false;
  m_currentFrameIndex =
    (m_currentFrameIndex + 1u) % static_cast<uint32_t>(m_frames.size());
  return frame.submission;
}

void FrameScheduler::waitForFrame(uint32_t frameIndex)
{
  VKDEMO_CPU_SCOPE("RHI.Frame.WaitForSubmission");
  require(frameIndex < m_frames.size(),
          "FrameScheduler::waitForFrame index out of range");
  const rhi::SubmissionToken token = m_frames[frameIndex].submission;
  if(token.isValid())
    m_queue->wait(token);
}

void FrameScheduler::waitForAllFrames()
{
  VKDEMO_CPU_SCOPE("RHI.Frame.WaitForCompletion");
  for(uint32_t index = 0; index < m_frames.size(); ++index)
    waitForFrame(index);
  if(m_device != nullptr)
    m_device->collectGarbage();
}

uint32_t FrameScheduler::frameCount() const noexcept
{
  return static_cast<uint32_t>(m_frames.size());
}

uint32_t FrameScheduler::currentFrameIndex() const noexcept
{
  return m_currentFrameIndex;
}

rhi::SubmissionToken FrameScheduler::currentFrameToken() const noexcept
{
  if(m_currentFrameIndex >= m_frames.size())
    return {};
  return m_frames[m_currentFrameIndex].submission;
}

rhi::Queue& FrameScheduler::queue() const
{
  require(m_queue != nullptr, "FrameScheduler::queue requires initialization");
  return *m_queue;
}

}  // namespace demo