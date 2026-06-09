#pragma once

#include "internal/VulkanCommon.h"
#include "../RHISurface.h"

namespace demo {
namespace rhi {
namespace vulkan {

class VulkanSurface final : public demo::rhi::Surface
{
public:
  VulkanSurface() = default;

  void                init(void* nativeInstance, void* nativePhysicalDevice, const WindowHandle& window) override;
  void                deinit() override;
  SurfaceCapabilities queryCapabilities() const override;

  // Typed typed accessor for backend-internal callers (e.g. swapchain init via cast).
  // Upper-layer code must not call this directly — cast to VulkanSurface first.
  [[nodiscard]] VkSurfaceKHR backendHandle() const { return m_surface; }

private:
  VkInstance       m_instance{VK_NULL_HANDLE};
  VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
  VkSurfaceKHR     m_surface{VK_NULL_HANDLE};
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
