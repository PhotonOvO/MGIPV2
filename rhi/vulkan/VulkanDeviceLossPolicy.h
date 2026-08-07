#pragma once

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] constexpr bool permitsEmergencyRetirementDrain(
  VkResult idleResult) noexcept
{
  return idleResult == VK_SUCCESS || idleResult == VK_ERROR_DEVICE_LOST;
}

}  // namespace demo::rhi::vulkan