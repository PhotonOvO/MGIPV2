#pragma once

#include "internal/VulkanCommon.h"
#include "../RHIPipeline.h"

#include <vector>

namespace demo::rhi::vulkan {

struct GraphicsPipelineCreateInfo
{
  const GraphicsPipelineDesc* desc{nullptr};
  VkPipelineLayout            layout{VK_NULL_HANDLE};
  const VkShaderModule*        shaderModules{nullptr};
  uint32_t                     shaderModuleCount{0};
};

struct ComputePipelineCreateInfo
{
  const ComputePipelineDesc*  desc{nullptr};
  VkPipelineLayout            layout{VK_NULL_HANDLE};
  const VkShaderModule*        shaderModules{nullptr};
  uint32_t                     shaderModuleCount{0};
  VkPipelineCreateFlags2 pipelineFlags{0};
  VkShaderModule         shaderModule{VK_NULL_HANDLE};
};

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineCreateInfo& createInfo);
VkPipeline createComputePipeline(VkDevice device, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::vulkan
