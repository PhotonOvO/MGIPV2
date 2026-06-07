#include "VulkanFrameContext.h"
#include "VulkanSwapchain.h"
#include "internal/VulkanCommon.h"

#include <array>
#include <cassert>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace demo {
namespace rhi {
namespace vulkan {

namespace {

void checkVk(VkResult result, const char* message)
{
  assert((result == VK_SUCCESS) && message);
}

bool isRenderDocInjected()
{
#ifdef _WIN32
  return GetModuleHandleA("renderdoc.dll") != nullptr;
#else
  return false;
#endif
}

}  // namespace

VulkanFrameContext::~VulkanFrameContext()
{
  deinit();
}

void VulkanFrameContext::init(void* nativeDevice, uint32_t queueFamilyIndex, uint32_t frameCount)
{
  assert((nativeDevice != nullptr) && "VulkanFrameContext::init requires VkDevice");
  assert((frameCount > 0) && "VulkanFrameContext::init requires non-zero frameCount");
  assert((m_device == VK_NULL_HANDLE) && "VulkanFrameContext::init already initialized");

  m_device            = static_cast<VkDevice>(nativeDevice);
  m_queueFamilyIndex  = queueFamilyIndex;
  m_currentFrameIndex = 0;
  m_frameCounter      = 0;
  vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_graphicsQueue);
  assert((m_graphicsQueue != VK_NULL_HANDLE) && "VulkanFrameContext::init failed resolving graphics queue");

  m_timelineSemaphore = std::make_unique<VulkanTimelineSemaphore>();
  m_timelineSemaphore->init(m_device, 0);

  m_frames.resize(frameCount);
  m_frameData.resize(frameCount);
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    FrameSlot& slot  = m_frames[i];
    slot.commandPool = std::make_unique<VulkanCommandPool>();
    slot.commandPool->init(m_device, QueueClass::graphics, queueFamilyIndex);
    slot.commandBuffer   = slot.commandPool->allocateNativeCommandBuffer();
    slot.lastSignalValue = 0;

    m_frameData[i].lastSignalValue = 0;
    m_frameData[i].userData        = nullptr;
  }
}

void VulkanFrameContext::deinit()
{
  if(m_timelineSemaphore != nullptr)
  {
    processRetirements(m_timelineSemaphore->getCurrentValue());
  }
  m_deferredDestructionQueue.clear();

  for(FrameSlot& slot : m_frames)
  {
    if(slot.commandPool != nullptr)
    {
      if(slot.commandBuffer != VK_NULL_HANDLE)
      {
        slot.commandPool->freeNativeCommandBuffer(slot.commandBuffer);
        slot.commandBuffer = VK_NULL_HANDLE;
      }
      slot.commandPool->deinit();
      slot.commandPool.reset();
    }
    slot.lastSignalValue = 0;
  }
  m_frames.clear();
  m_frameData.clear();

  if(m_timelineSemaphore != nullptr)
  {
    m_timelineSemaphore->deinit();
    m_timelineSemaphore.reset();
  }

  m_device            = VK_NULL_HANDLE;
  m_queueFamilyIndex  = ~0U;
  m_graphicsQueue     = VK_NULL_HANDLE;
  m_swapchain         = nullptr;
  m_currentFrameIndex = 0;
  m_frameCounter      = 0;
}

void VulkanFrameContext::beginFrame()
{
  assert((m_device != VK_NULL_HANDLE) && "VulkanFrameContext::beginFrame requires initialized context");
  assert((m_currentFrameIndex < m_frames.size()) && "VulkanFrameContext::beginFrame invalid frame index");

  // Wait already done externally (waitForFrameCompletion) at end of previous frame
  // Just reset pool and start command buffer recording

  FrameSlot& frame = m_frames[m_currentFrameIndex];
  assert((frame.commandPool != nullptr) && "VulkanFrameContext::beginFrame missing command pool");
  assert((frame.commandBuffer != VK_NULL_HANDLE) && "VulkanFrameContext::beginFrame missing command buffer");

  frame.commandPool->reset();

  const VkCommandBufferBeginInfo beginInfo{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "VulkanFrameContext::beginFrame vkBeginCommandBuffer failed");
}

SubmissionReceipt VulkanFrameContext::endFrame(CommandBuffer* cmdBuffer)
{
  assert((cmdBuffer != nullptr) && "VulkanFrameContext::endFrame requires command buffer");
  assert((m_currentFrameIndex < m_frames.size()) && "VulkanFrameContext::endFrame invalid frame index");

  const SubmissionReceipt receipt = submitCurrentFrame(*cmdBuffer);
  advanceToNextFrame();
  return receipt;
}

void VulkanFrameContext::setSwapchain(Swapchain* swapchain)
{
  m_swapchain = swapchain;
}

SubmissionReceipt VulkanFrameContext::submitCommandBuffers(const SubmissionRequest* requests, uint32_t requestCount)
{
  assert((requests != nullptr) && "VulkanFrameContext::submitCommandBuffers requires request list");
  assert((requestCount > 0) && "VulkanFrameContext::submitCommandBuffers requires non-zero request count");
  assert((requests[0].commandBuffer != nullptr) && "VulkanFrameContext::submitCommandBuffers requires command buffer");
  return submitCurrentFrame(*requests[0].commandBuffer);
}

void VulkanFrameContext::waitForSubmission(SubmissionReceipt receipt)
{
  assert((m_timelineSemaphore != nullptr) && "VulkanFrameContext::waitForSubmission requires timeline semaphore");
  m_timelineSemaphore->wait(receipt.timelineValue);
}

FrameData& VulkanFrameContext::getCurrentFrame()
{
  assert((m_currentFrameIndex < m_frameData.size()) && "VulkanFrameContext::getCurrentFrame invalid frame index");
  return m_frameData[m_currentFrameIndex];
}

void VulkanFrameContext::advanceToNextFrame()
{
  if(m_frames.empty())
  {
    return;
  }
  m_currentFrameIndex = (m_currentFrameIndex + 1) % static_cast<uint32_t>(m_frames.size());
}

void VulkanFrameContext::waitCurrentFrame()
{
  assert((m_timelineSemaphore != nullptr) && "VulkanFrameContext::waitCurrentFrame requires timeline semaphore");
  assert((m_currentFrameIndex < m_frames.size()) && "VulkanFrameContext::waitCurrentFrame invalid frame index");
  m_timelineSemaphore->wait(m_frames[m_currentFrameIndex].lastSignalValue);
}

void VulkanFrameContext::waitForFrame(uint64_t frameIndex)
{
  assert((m_timelineSemaphore != nullptr) && "VulkanFrameContext::waitForFrame requires timeline semaphore");
  if(frameIndex >= m_frames.size())
  {
    return;
  }
  m_timelineSemaphore->wait(m_frames[frameIndex].lastSignalValue);
}

void VulkanFrameContext::waitForFrameCompletion()
{
  assert((m_timelineSemaphore != nullptr) && "VulkanFrameContext::waitForFrameCompletion requires timeline semaphore");
  assert((m_currentFrameIndex < m_frames.size()) && "VulkanFrameContext::waitForFrameCompletion invalid frame index");

  // Wait for the frame slot to be ready (GPU finished)
  m_timelineSemaphore->wait(m_frames[m_currentFrameIndex].lastSignalValue);

  // Process deferred destructions now that we know the frame is complete
  processRetirements(m_timelineSemaphore->getCurrentValue());
}

uint32_t VulkanFrameContext::getFrameCount() const
{
  return static_cast<uint32_t>(m_frames.size());
}

uint32_t VulkanFrameContext::getCurrentFrameIndex() const
{
  return m_currentFrameIndex;
}

uint64_t VulkanFrameContext::getCurrentFrameValue() const
{
  if(m_currentFrameIndex >= m_frameData.size())
  {
    return 0;
  }
  return m_frameData[m_currentFrameIndex].lastSignalValue;
}

void* VulkanFrameContext::getTimelineSemaphore() const
{
  return static_cast<void*>(nativeTimelineSemaphore());
}

void VulkanFrameContext::enqueueRetirement(ResourceHandle resource, uint64_t timelineValue)
{
  m_deferredDestructionQueue.enqueue(ResourceRetirement{resource, timelineValue});
}

uint32_t VulkanFrameContext::processRetirements(uint64_t currentTimelineValue)
{
  return m_deferredDestructionQueue.process(currentTimelineValue);
}

DeferredDestructionQueue& VulkanFrameContext::getDestructionQueue()
{
  return m_deferredDestructionQueue;
}

const DeferredDestructionQueue& VulkanFrameContext::getDestructionQueue() const
{
  return m_deferredDestructionQueue;
}

CommandBuffer* VulkanFrameContext::getCommandBuffer()
{
  const VkCommandBuffer commandBuffer = nativeCommandBuffer(m_currentFrameIndex);
  m_commandBufferFacade.setTarget(commandBuffer, m_resourceTable);
  return &m_commandBufferFacade;
}

VkSemaphore VulkanFrameContext::nativeTimelineSemaphore() const
{
  if(m_timelineSemaphore == nullptr)
  {
    return VK_NULL_HANDLE;
  }
  return m_timelineSemaphore->nativeSemaphore();
}

VkCommandBuffer VulkanFrameContext::nativeCommandBuffer(uint32_t frameIndex) const
{
  if(frameIndex >= m_frames.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_frames[frameIndex].commandBuffer;
}

SubmissionReceipt VulkanFrameContext::submitCurrentFrame(CommandBuffer& commandBuffer)
{
  assert((m_graphicsQueue != VK_NULL_HANDLE) && "VulkanFrameContext::submitCurrentFrame requires graphics queue");
  assert((m_timelineSemaphore != nullptr) && "VulkanFrameContext::submitCurrentFrame requires timeline semaphore");
  assert((m_swapchain != nullptr) && "VulkanFrameContext::submitCurrentFrame requires swapchain");

  auto*      vkSwapchain     = static_cast<VulkanSwapchain*>(m_swapchain);
  const auto waitSemaphore   = vkSwapchain->imageAvailableSemaphoreForCurrentFrame();
  const auto signalSemaphore = vkSwapchain->renderFinishedSemaphoreForCurrentImage();

  FrameSlot& frame = m_frames[m_currentFrameIndex];
  assert((frame.commandBuffer != VK_NULL_HANDLE) && "VulkanFrameContext::submitCurrentFrame missing frame command buffer");

  const VkCommandBuffer nativeCommandBuffer = static_cast<VkCommandBuffer>(commandBuffer.getBackendHandle());
  assert((nativeCommandBuffer == frame.commandBuffer)
         && "VulkanFrameContext::submitCurrentFrame command buffer mismatch");
  checkVk(vkEndCommandBuffer(nativeCommandBuffer), "VulkanFrameContext::submitCurrentFrame vkEndCommandBuffer failed");

  std::array<VkSemaphoreSubmitInfo, 1> waitInfos{};
  uint32_t                             waitCount = 0;
  if(waitSemaphore != VK_NULL_HANDLE)
  {
    waitInfos[0] = VkSemaphoreSubmitInfo{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = waitSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    waitCount = 1;
  }

  std::array<VkSemaphoreSubmitInfo, 2> signalInfos{};
  uint32_t                             signalCount = 0;
  if(signalSemaphore != VK_NULL_HANDLE)
  {
    signalInfos[signalCount++] = VkSemaphoreSubmitInfo{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signalSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
  }

  const uint64_t currentTimelineValue = m_timelineSemaphore->getCurrentValue();
  const uint64_t signalValue =
      (m_frameCounter > currentTimelineValue ? m_frameCounter : currentTimelineValue) + 1;
  m_frameCounter = signalValue;
  signalInfos[signalCount++] = VkSemaphoreSubmitInfo{
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = m_timelineSemaphore->nativeSemaphore(),
      .value     = signalValue,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };

  frame.lastSignalValue                            = signalValue;
  m_frameData[m_currentFrameIndex].lastSignalValue = signalValue;

  const VkCommandBufferSubmitInfo commandBufferInfo{
      .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = nativeCommandBuffer,
  };

  const VkSubmitInfo2 submitInfo{
      .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount   = waitCount,
      .pWaitSemaphoreInfos      = waitCount > 0 ? waitInfos.data() : nullptr,
      .commandBufferInfoCount   = 1,
      .pCommandBufferInfos      = &commandBufferInfo,
      .signalSemaphoreInfoCount = signalCount,
      .pSignalSemaphoreInfos    = signalInfos.data(),
  };

  {
    checkVk(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "VulkanFrameContext::submitCurrentFrame vkQueueSubmit2 failed");
  }
  return SubmissionReceipt{.timelineValue = signalValue};
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
