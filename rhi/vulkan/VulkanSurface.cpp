#include "VulkanSurface.h"
#include "internal/VulkanCommon.h"

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#else
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <string>
#include <stdexcept>

namespace demo {
namespace rhi {
namespace vulkan {

void VulkanSurface::initVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, const WindowHandle& window)
{
  m_instance       = instance;
  m_physicalDevice = physicalDevice;

  ASSERT(m_instance != VK_NULL_HANDLE, "VulkanSurface requires a valid VkInstance");
  ASSERT(m_physicalDevice != VK_NULL_HANDLE, "VulkanSurface requires a valid VkPhysicalDevice");
  ASSERT(m_surface == VK_NULL_HANDLE, "VulkanSurface::init called while surface is already initialized");
  ASSERT(window.nativeWindow != nullptr, "VulkanSurface requires a valid native window handle");

#ifdef __ANDROID__
  const VkAndroidSurfaceCreateInfoKHR surfaceInfo{
      .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
      .window = static_cast<ANativeWindow*>(window.nativeWindow),
  };
  VK_CHECK(vkCreateAndroidSurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface));
#else
  SDL_Window* sdlWindow = static_cast<SDL_Window*>(window.nativeWindow);
  if(!SDL_Vulkan_CreateSurface(sdlWindow, m_instance, nullptr, &m_surface))
  {
    throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
  }
#endif
}

void VulkanSurface::deinit()
{
  if(m_surface != VK_NULL_HANDLE)
  {
#ifdef __ANDROID__
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
#else
    SDL_Vulkan_DestroySurface(m_instance, m_surface, nullptr);
#endif
    m_surface = VK_NULL_HANDLE;
  }
}

SurfaceCapabilities VulkanSurface::queryCapabilities() const
{
  ASSERT(m_physicalDevice != VK_NULL_HANDLE, "VulkanSurface requires a valid VkPhysicalDevice");
  ASSERT(m_surface != VK_NULL_HANDLE, "VulkanSurface::queryCapabilities called before init");

  VkSurfaceCapabilitiesKHR vkCapabilities{};
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &vkCapabilities));

  SurfaceCapabilities capabilities{};
  capabilities.minImageCount       = vkCapabilities.minImageCount;
  capabilities.maxImageCount       = vkCapabilities.maxImageCount;
  capabilities.currentExtent       = {vkCapabilities.currentExtent.width, vkCapabilities.currentExtent.height};
  capabilities.minImageExtent      = {vkCapabilities.minImageExtent.width, vkCapabilities.minImageExtent.height};
  capabilities.maxImageExtent      = {vkCapabilities.maxImageExtent.width, vkCapabilities.maxImageExtent.height};
  capabilities.currentTransform    = static_cast<uint32_t>(vkCapabilities.currentTransform);
  capabilities.supportedUsageFlags = static_cast<uint32_t>(vkCapabilities.supportedUsageFlags);
  return capabilities;
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
