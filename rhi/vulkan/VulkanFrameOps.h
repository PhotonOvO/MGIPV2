#pragma once

#include "../../common/Common.h"

namespace demo::rhi::vulkan {

void waitTimeline(VkDevice device, VkSemaphore timelineSemaphore, uint64_t waitValue);
void resetAndBeginCommandBuffer(VkDevice device, VkCommandPool commandPool, VkCommandBuffer commandBuffer);
void endAndSubmitFrame(VkQueue              queue,
                       VkCommandBuffer      commandBuffer,
                       VkSemaphore          waitSemaphore,
                       VkPipelineStageFlags waitStage,
                       VkSemaphore          signalSemaphore,
                       VkPipelineStageFlags signalStage,
                       VkSemaphore          timelineSemaphore,
                       uint64_t             signalValue);
void waitQueueIdle(VkQueue queue);

}  // namespace demo::rhi::vulkan
