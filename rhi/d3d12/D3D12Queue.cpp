#include "D3D12Queue.h"

#include "D3D12CommandBuffer.h"
#include "../RHIDebugCounters.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace demo::rhi::d3d12 {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

void checkHresult(HRESULT result, const char* operation)
{
  if(FAILED(result))
  {
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

}  // namespace

D3D12Queue::~D3D12Queue()
{
  deinit();
}

void D3D12Queue::init(void* nativeDevice, void* nativeQueue, uint32_t nativeType,
                      QueueIdentity identity, QueueInfo info)
{
  require(m_device == nullptr, "D3D12Queue::init called twice");
  require(nativeDevice != nullptr && nativeQueue != nullptr,
          "D3D12Queue::init requires a device and queue");
  require(identity.isValid() && info.isValid(),
          "D3D12Queue::init requires a valid public identity");

  m_device = nativeDevice;
  m_queue = nativeQueue;
  m_nativeType = nativeType;
  m_identity = identity;
  m_info = info;

  ID3D12Fence* fence = nullptr;
  checkHresult(static_cast<ID3D12Device*>(m_device)->CreateFence(
                 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
               "ID3D12Device::CreateFence(queue timeline)");
  m_fence = fence;
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if(m_fenceEvent == nullptr)
  {
    deinit();
    throw std::runtime_error("CreateEventW failed for D3D12 queue timeline");
  }
}

void D3D12Queue::deinit()
{
  if(m_fenceEvent != nullptr)
  {
    CloseHandle(static_cast<HANDLE>(m_fenceEvent));
    m_fenceEvent = nullptr;
  }
  if(m_fence != nullptr)
  {
    static_cast<ID3D12Fence*>(m_fence)->Release();
    m_fence = nullptr;
  }
  m_device = nullptr;
  m_queue = nullptr;
  m_nativeType = 0;
  m_identity = {};
  m_info = {};
  m_queues = {};
  m_lastSubmittedValue = 0;
}

void D3D12Queue::setQueueRegistry(std::array<D3D12Queue*, 3> queues) noexcept
{
  m_queues = queues;
}

QueueIdentity D3D12Queue::identity() const noexcept
{
  return m_identity;
}

QueueInfo D3D12Queue::info() const noexcept
{
  return m_info;
}

SubmissionToken D3D12Queue::submit(const SubmitBatch& batch)
{
  require(m_queue != nullptr && m_fence != nullptr,
          "D3D12Queue::submit requires an initialized queue");
  require(!batch.commandBuffers.empty(),
          "D3D12Queue::submit requires command buffers");
  if(m_lastSubmittedValue >= std::numeric_limits<uint64_t>::max() - 64u)
    recreateExhaustedTimeline();

  constexpr size_t kMaxCommandBuffers = 64;
  require(batch.commandBuffers.size() <= kMaxCommandBuffers,
          "D3D12Queue::submit command-buffer capacity exceeded");
  require(batch.signalPoints.empty(),
          "D3D12Queue does not accept backend-external signal points");

  std::array<ID3D12CommandList*, kMaxCommandBuffers> nativeLists{};
  std::array<D3D12CommandBuffer*, kMaxCommandBuffers> commandBuffers{};
  uint32_t commandCount = 0;
  for(CommandBuffer* commandBuffer : batch.commandBuffers)
  {
    auto* d3dCommandBuffer = dynamic_cast<D3D12CommandBuffer*>(commandBuffer);
    require(d3dCommandBuffer != nullptr,
            "D3D12Queue::submit received a foreign command buffer");
    require(d3dCommandBuffer->state() == CommandBufferState::executable,
            "D3D12Queue::submit requires executable command buffers");
    require(d3dCommandBuffer->queueClass() == m_info.queueClass,
            "D3D12Queue::submit command buffer queue class mismatch");
    d3dCommandBuffer->validateForSubmit();
    nativeLists[commandCount] =
      static_cast<ID3D12GraphicsCommandList*>(d3dCommandBuffer->nativeHandle());
    commandBuffers[commandCount] = d3dCommandBuffer;
    ++commandCount;
  }

  for(const SubmitWaitPoint& waitPoint : batch.waitPoints)
  {
    require(waitPoint.isValid(),
            "D3D12Queue::submit received an invalid wait point");
    require(waitPoint.submission.isValid(),
            "D3D12Queue does not accept backend-external wait points");
    D3D12Queue& source = resolveQueue(waitPoint.submission.queue);
    require(waitPoint.submission.value <= source.m_lastSubmittedValue,
            "D3D12Queue wait token has not been submitted");
  }

  auto* queue = static_cast<ID3D12CommandQueue*>(m_queue);
  for(const SubmitWaitPoint& waitPoint : batch.waitPoints)
  {
    D3D12Queue& source = resolveQueue(waitPoint.submission.queue);
    checkHresult(queue->Wait(static_cast<ID3D12Fence*>(source.m_fence),
                            waitPoint.submission.value),
                 "ID3D12CommandQueue::Wait");
  }

  queue->ExecuteCommandLists(commandCount, nativeLists.data());
  const uint64_t signalValue = m_lastSubmittedValue + 1u;
  checkHresult(queue->Signal(static_cast<ID3D12Fence*>(m_fence), signalValue),
               "ID3D12CommandQueue::Signal(queue timeline)");
  m_lastSubmittedValue = signalValue;
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::queueSubmits);
  incrementHotPathCounter(
    BackendType::d3d12, HotPathCounter::submittedCommandBuffers, commandCount);

  const SubmissionToken token{m_identity, signalValue};
  for(uint32_t index = 0; index < commandCount; ++index)
    commandBuffers[index]->markSubmitted(token);
  return token;
}

bool D3D12Queue::isComplete(SubmissionToken token) const
{
  require(token.isValid(), "D3D12Queue::isComplete requires a valid token");
  require(token.queue == m_identity,
          "D3D12Queue::isComplete rejected a stale or wrong-queue token");
  require(token.value <= m_lastSubmittedValue,
          "D3D12Queue::isComplete token has not been submitted");
  return static_cast<ID3D12Fence*>(m_fence)->GetCompletedValue() >= token.value;
}

void D3D12Queue::wait(SubmissionToken token)
{
  require(token.isValid(), "D3D12Queue::wait requires a valid token");
  require(token.queue == m_identity,
          "D3D12Queue::wait rejected a stale or wrong-queue token");
  require(token.value <= m_lastSubmittedValue,
          "D3D12Queue::wait token has not been submitted");
  if(isComplete(token))
    return;

  checkHresult(static_cast<ID3D12Fence*>(m_fence)->SetEventOnCompletion(
                 token.value, static_cast<HANDLE>(m_fenceEvent)),
               "ID3D12Fence::SetEventOnCompletion");
  if(WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE) != WAIT_OBJECT_0)
    throw std::runtime_error("WaitForSingleObject failed for D3D12 queue timeline");
}

void D3D12Queue::waitIdle()
{
  const SubmissionToken token = lastSubmittedToken();
  if(token.isValid())
    wait(token);
}

uint64_t D3D12Queue::completedValue() const noexcept
{
  return m_fence != nullptr
    ? static_cast<ID3D12Fence*>(m_fence)->GetCompletedValue()
    : 0;
}

SubmissionToken D3D12Queue::lastSubmittedToken() const noexcept
{
  return m_lastSubmittedValue != 0
    ? SubmissionToken{m_identity, m_lastSubmittedValue}
    : SubmissionToken{};
}

D3D12Queue& D3D12Queue::resolveQueue(QueueIdentity identity) const
{
  for(D3D12Queue* queue : m_queues)
  {
    if(queue != nullptr && queue->m_identity == identity)
      return *queue;
  }
  throw std::runtime_error("D3D12Queue rejected a stale or unknown queue identity");
}

void D3D12Queue::recreateExhaustedTimeline()
{
  waitIdle();
  auto* oldFence = static_cast<ID3D12Fence*>(m_fence);
  ID3D12Fence* newFence = nullptr;
  checkHresult(static_cast<ID3D12Device*>(m_device)->CreateFence(
                 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&newFence)),
               "ID3D12Device::CreateFence(recovered queue timeline)");
  m_fence = newFence;
  if(oldFence != nullptr)
    oldFence->Release();
  ++m_identity.generation;
  if(m_identity.generation == 0)
    m_identity.generation = 1;
  m_lastSubmittedValue = 0;
}
}  // namespace demo::rhi::d3d12
