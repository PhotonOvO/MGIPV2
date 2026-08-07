#pragma once

#include "../RHITypes.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] constexpr VkShaderStageFlagBits toVkShaderStage(ShaderStage stage) noexcept
{
  switch(stage)
  {
  case ShaderStage::vertex: return VK_SHADER_STAGE_VERTEX_BIT;
  case ShaderStage::fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
  case ShaderStage::compute: return VK_SHADER_STAGE_COMPUTE_BIT;
  case ShaderStage::geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
  case ShaderStage::tessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  case ShaderStage::tessEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  case ShaderStage::none:
  case ShaderStage::allGraphics:
  case ShaderStage::all:
    return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
  }
  return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
}

[[nodiscard]] constexpr VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages) noexcept
{
  VkShaderStageFlags flags = 0;
  const uint32_t mask = static_cast<uint32_t>(stages);
  if((mask & static_cast<uint32_t>(ShaderStage::vertex)) != 0)
    flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if((mask & static_cast<uint32_t>(ShaderStage::fragment)) != 0)
    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if((mask & static_cast<uint32_t>(ShaderStage::compute)) != 0)
    flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  if((mask & static_cast<uint32_t>(ShaderStage::geometry)) != 0)
    flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  if((mask & static_cast<uint32_t>(ShaderStage::tessControl)) != 0)
    flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if((mask & static_cast<uint32_t>(ShaderStage::tessEval)) != 0)
    flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  return flags;
}

}  // namespace demo::rhi::vulkan
