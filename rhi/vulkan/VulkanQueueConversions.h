#pragma once

#include "../RHIQueue.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] constexpr VkPipelineStageFlags2 toVkSubmitStage(SubmitStage stage) noexcept
{
  switch(stage)
  {
  case SubmitStage::allCommands: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  case SubmitStage::colorOutput: return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  case SubmitStage::compute: return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  case SubmitStage::transfer: return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
  }
  return VK_PIPELINE_STAGE_2_NONE;
}

}  // namespace demo::rhi::vulkan
