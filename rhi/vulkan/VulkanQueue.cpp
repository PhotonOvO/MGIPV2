#include "VulkanQueue.h"

#include "VulkanCommandBuffer.h"
#include "VulkanQueueConversions.h"
#include "internal/VulkanCommon.h"
#include "../RHIDebugCounters.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace demo::rhi::vulkan {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

void checkVkResult(VkResult result, const char* operation)
{
  if(result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed");
}

}  // namespace

QueueSyncHandle VulkanQueueSyncRegistry::registerSemaphore(VkSemaphore semaphore)
{
  require(semaphore != VK_NULL_HANDLE,
          "VulkanQueueSyncRegistry::registerSemaphore requires a semaphore");
  return m_semaphores.emplace(VulkanQueueSyncRecord{semaphore});
}

void VulkanQueueSyncRegistry::unregisterSemaphore(QueueSyncHandle handle)
{
  if(!handle.isNull() && !m_semaphores.destroy(handle))
    throw std::runtime_error("VulkanQueueSyncRegistry rejected a stale semaphore handle");
}

VkSemaphore VulkanQueueSyncRegistry::resolve(QueueSyncHandle handle) const
{
  const VulkanQueueSyncRecord* record = m_semaphores.tryGet(handle);
  return record != nullptr ? record->semaphore : VK_NULL_HANDLE;
}

VulkanQueue::~VulkanQueue()
{
  deinit();
}

void VulkanQueue::init(VkDevice device, VkQueue queue, uint32_t familyIndex,
                       uint32_t queueIndex, QueueIdentity identity, QueueInfo info,
                       VulkanQueueSyncRegistry* syncRegistry)
{
  require(device != VK_NULL_HANDLE, "VulkanQueue::init requires VkDevice");
  require(queue != VK_NULL_HANDLE, "VulkanQueue::init requires VkQueue");
  require(identity.isValid(), "VulkanQueue::init requires a valid identity");
  require(info.isValid(), "VulkanQueue::init requires an available queue");
  require(syncRegistry != nullptr, "VulkanQueue::init requires a sync registry");
  require(m_device == VK_NULL_HANDLE, "VulkanQueue::init called twice");

  m_device = device;
  m_queue = queue;
  m_familyIndex = familyIndex;
  m_queueIndex = queueIndex;
  m_identity = identity;
  m_info = info;
  m_syncRegistry = syncRegistry;
  m_lastSubmittedValue = 0;
  createTimeline(0);
}

void VulkanQueue::deinit()
{
  if(m_device != VK_NULL_HANDLE && m_timeline != VK_NULL_HANDLE)
    vkDestroySemaphore(m_device, m_timeline, nullptr);
  m_timeline = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
  m_queue = VK_NULL_HANDLE;
  m_familyIndex = ~0u;
  m_queueIndex = 0;
  m_identity = {};
  m_info = {};
  m_syncRegistry = nullptr;
  m_queues = {};
  m_lastSubmittedValue = 0;
}

void VulkanQueue::setQueueRegistry(std::array<VulkanQueue*, 3> queues) noexcept
{
  m_queues = queues;
}

QueueIdentity VulkanQueue::identity() const noexcept
{
  return m_identity;
}

QueueInfo VulkanQueue::info() const noexcept
{
  return m_info;
}

SubmissionToken VulkanQueue::submit(const SubmitBatch& batch)
{
  require(m_queue != VK_NULL_HANDLE, "VulkanQueue::submit requires an initialized queue");
  require(!batch.commandBuffers.empty(), "VulkanQueue::submit requires command buffers");

  if(m_lastSubmittedValue >= std::numeric_limits<uint64_t>::max() - 64u)
    recreateExhaustedTimeline();

  constexpr size_t kMaxCommandBuffers = 64;
  constexpr size_t kMaxWaitPoints = 64;
  constexpr size_t kMaxSignalPoints = 64;
  require(batch.commandBuffers.size() <= kMaxCommandBuffers,
          "VulkanQueue::submit command-buffer capacity exceeded");
  require(batch.waitPoints.size() <= kMaxWaitPoints,
          "VulkanQueue::submit wait-point capacity exceeded");
  require(batch.signalPoints.size() <= kMaxSignalPoints,
          "VulkanQueue::submit signal-point capacity exceeded");

  std::array<VkCommandBufferSubmitInfo, kMaxCommandBuffers> commandInfos{};
  std::array<VulkanCommandBuffer*, kMaxCommandBuffers> commandBuffers{};
  uint32_t commandCount = 0;
  for(CommandBuffer* commandBuffer : batch.commandBuffers)
  {
    auto* vkCommandBuffer = dynamic_cast<VulkanCommandBuffer*>(commandBuffer);
    require(vkCommandBuffer != nullptr,
            "VulkanQueue::submit received a command buffer from another backend");
    require(vkCommandBuffer->state() == CommandBufferState::executable,
            "VulkanQueue::submit requires executable command buffers");
    require(vkCommandBuffer->queueClass() == m_info.queueClass,
            "VulkanQueue::submit command buffer queue class mismatch");
    vkCommandBuffer->validateForSubmit();
    commandInfos[commandCount] = VkCommandBufferSubmitInfo{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = vkCommandBuffer->nativeHandle(),
    };
    commandBuffers[commandCount] = vkCommandBuffer;
    ++commandCount;
  }

  std::array<VkSemaphoreSubmitInfo, kMaxWaitPoints> waitInfos{};
  uint32_t waitCount = 0;
  for(const SubmitWaitPoint& waitPoint : batch.waitPoints)
  {
    require(waitPoint.isValid(), "VulkanQueue::submit received an invalid wait point");
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0;
    if(waitPoint.submission.isValid())
    {
      VulkanQueue& source = resolveQueue(waitPoint.submission.queue);
      require(waitPoint.submission.value <= source.m_lastSubmittedValue,
              "VulkanQueue::submit wait token has not been submitted");
      semaphore = source.m_timeline;
      value = waitPoint.submission.value;
    }
    else
    {
      semaphore = m_syncRegistry->resolve(waitPoint.external.sync);
      value = waitPoint.external.value;
      require(semaphore != VK_NULL_HANDLE,
              "VulkanQueue::submit rejected a stale external wait point");
    }
    waitInfos[waitCount++] = VkSemaphoreSubmitInfo{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = semaphore,
      .value = value,
      .stageMask = toVkSubmitStage(waitPoint.stage),
    };
  }

  std::array<VkSemaphoreSubmitInfo, kMaxSignalPoints + 1> signalInfos{};
  uint32_t signalCount = 0;
  for(const SubmitSignalPoint& signalPoint : batch.signalPoints)
  {
    require(signalPoint.isValid(), "VulkanQueue::submit received an invalid signal point");
    const VkSemaphore semaphore = m_syncRegistry->resolve(signalPoint.external.sync);
    require(semaphore != VK_NULL_HANDLE,
            "VulkanQueue::submit rejected a stale external signal point");
    signalInfos[signalCount++] = VkSemaphoreSubmitInfo{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = semaphore,
      .value = signalPoint.external.value,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
  }

  const uint64_t signalValue = m_lastSubmittedValue + 1u;
  signalInfos[signalCount++] = VkSemaphoreSubmitInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    .semaphore = m_timeline,
    .value = signalValue,
    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkSubmitInfo2 submitInfo{
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
    .waitSemaphoreInfoCount = waitCount,
    .pWaitSemaphoreInfos = waitCount != 0 ? waitInfos.data() : nullptr,
    .commandBufferInfoCount = commandCount,
    .pCommandBufferInfos = commandInfos.data(),
    .signalSemaphoreInfoCount = signalCount,
    .pSignalSemaphoreInfos = signalInfos.data(),
  };
  checkVkResult(vkQueueSubmit2(m_queue, 1, &submitInfo, VK_NULL_HANDLE),
                "vkQueueSubmit2");

  m_lastSubmittedValue = signalValue;
  incrementHotPathCounter(BackendType::vulkan, HotPathCounter::queueSubmits);
  incrementHotPathCounter(
    BackendType::vulkan, HotPathCounter::submittedCommandBuffers, commandCount);
  const SubmissionToken token{m_identity, signalValue};
  for(uint32_t index = 0; index < commandCount; ++index)
    commandBuffers[index]->markSubmitted(token);
  return token;
}

bool VulkanQueue::isComplete(SubmissionToken token) const
{
  require(token.isValid(), "VulkanQueue::isComplete requires a valid token");
  require(token.queue == m_identity,
          "VulkanQueue::isComplete rejected a stale or wrong-queue token");
  require(token.value <= m_lastSubmittedValue,
          "VulkanQueue::isComplete token has not been submitted");

  uint64_t completed = 0;
  checkVkResult(vkGetSemaphoreCounterValue(m_device, m_timeline, &completed),
                "vkGetSemaphoreCounterValue");
  return completed >= token.value;
}

void VulkanQueue::wait(SubmissionToken token)
{
  require(token.isValid(), "VulkanQueue::wait requires a valid token");
  require(token.queue == m_identity,
          "VulkanQueue::wait rejected a stale or wrong-queue token");
  require(token.value <= m_lastSubmittedValue,
          "VulkanQueue::wait token has not been submitted");

  const VkSemaphoreWaitInfo waitInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
    .semaphoreCount = 1,
    .pSemaphores = &m_timeline,
    .pValues = &token.value,
  };
  checkVkResult(vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX),
                "vkWaitSemaphores");
}

void VulkanQueue::waitIdle()
{
  require(m_queue != VK_NULL_HANDLE, "VulkanQueue::waitIdle requires an initialized queue");
  checkVkResult(vkQueueWaitIdle(m_queue), "vkQueueWaitIdle");
}

uint64_t VulkanQueue::completedValue() const
{
  uint64_t completed = 0;
  checkVkResult(vkGetSemaphoreCounterValue(m_device, m_timeline, &completed),
                "vkGetSemaphoreCounterValue");
  return completed;
}

SubmissionToken VulkanQueue::lastSubmittedToken() const noexcept
{
  return m_lastSubmittedValue != 0
    ? SubmissionToken{m_identity, m_lastSubmittedValue}
    : SubmissionToken{};
}

VulkanQueue& VulkanQueue::resolveQueue(QueueIdentity identity) const
{
  for(VulkanQueue* queue : m_queues)
  {
    if(queue != nullptr && queue->m_identity == identity)
      return *queue;
  }
  throw std::runtime_error("VulkanQueue rejected a stale or unknown queue identity");
}

void VulkanQueue::createTimeline(uint64_t initialValue)
{
  const VkSemaphoreTypeCreateInfo typeInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    .initialValue = initialValue,
  };
  const VkSemaphoreCreateInfo createInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = &typeInfo,
  };
  checkVkResult(vkCreateSemaphore(m_device, &createInfo, nullptr, &m_timeline),
                "vkCreateSemaphore(timeline)");
}

void VulkanQueue::recreateExhaustedTimeline()
{
  waitIdle();
  vkDestroySemaphore(m_device, m_timeline, nullptr);
  m_timeline = VK_NULL_HANDLE;
  ++m_identity.generation;
  if(m_identity.generation == 0)
    m_identity.generation = 1;
  m_lastSubmittedValue = 0;
  createTimeline(0);
}

}  // namespace demo::rhi::vulkan