#include "VulkanFrameOps.h"

namespace demo::rhi::vulkan {

void waitTimeline(VkDevice device, VkSemaphore timelineSemaphore, uint64_t waitValue)
{
  const VkSemaphoreWaitInfo waitInfo = {
      .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1,
      .pSemaphores    = &timelineSemaphore,
      .pValues        = &waitValue,
  };
  VK_CHECK(vkWaitSemaphores(device, &waitInfo, std::numeric_limits<uint64_t>::max()));
}

void resetAndBeginCommandBuffer(VkDevice device, VkCommandPool commandPool, VkCommandBuffer commandBuffer)
{
  VK_CHECK(vkResetCommandPool(device, commandPool, 0));
  const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
}

void endAndSubmitFrame(VkQueue              queue,
                       VkCommandBuffer      commandBuffer,
                       VkSemaphore          waitSemaphore,
                       VkPipelineStageFlags waitStage,
                       VkSemaphore          signalSemaphore,
                       VkPipelineStageFlags signalStage,
                       VkSemaphore          timelineSemaphore,
                       uint64_t             signalValue)
{
  VK_CHECK(vkEndCommandBuffer(commandBuffer));

  std::array<VkSemaphoreSubmitInfo, 1> waitSemaphores{{{
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = waitSemaphore,
      .stageMask = waitStage,
  }}};

  std::array<VkSemaphoreSubmitInfo, 2> signalSemaphores{{
      {
          .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = signalSemaphore,
          .stageMask = signalStage,
      },
      {
          .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = timelineSemaphore,
          .value     = signalValue,
          .stageMask = signalStage,
      },
  }};

  const std::array<VkCommandBufferSubmitInfo, 1> commandBuffers{{{
      .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = commandBuffer,
  }}};

  const std::array<VkSubmitInfo2, 1> submitInfo{{{
      .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount   = uint32_t(waitSemaphores.size()),
      .pWaitSemaphoreInfos      = waitSemaphores.data(),
      .commandBufferInfoCount   = uint32_t(commandBuffers.size()),
      .pCommandBufferInfos      = commandBuffers.data(),
      .signalSemaphoreInfoCount = uint32_t(signalSemaphores.size()),
      .pSignalSemaphoreInfos    = signalSemaphores.data(),
  }}};

  VK_CHECK(vkQueueSubmit2(queue, uint32_t(submitInfo.size()), submitInfo.data(), nullptr));
}

void waitQueueIdle(VkQueue queue)
{
  vkQueueWaitIdle(queue);
}

}  // namespace demo::rhi::vulkan
