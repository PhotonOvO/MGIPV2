#pragma once

#include "internal/VulkanCommon.h"
#include "../RHIPipeline.h"

#include <vector>

namespace demo::rhi::vulkan {

struct GraphicsPipelineCreateInfo
{
  const GraphicsPipelineDesc* desc{nullptr};
  VkPipelineLayout            layout{VK_NULL_HANDLE};
};

struct ComputePipelineCreateInfo
{
  const ComputePipelineDesc*  desc{nullptr};
  VkPipelineLayout            layout{VK_NULL_HANDLE};
  VkPipelineCreateFlags2 pipelineFlags{0};
};

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineCreateInfo& createInfo);
VkPipeline createComputePipeline(VkDevice device, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::vulkan
