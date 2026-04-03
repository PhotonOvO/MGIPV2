#include "VulkanSurface.h"
#include "../../common/Common.h"

namespace demo {
namespace rhi {
namespace vulkan {

void VulkanSurface::init(void* nativeInstance, void* nativePhysicalDevice, const WindowHandle& window)
{
  m_instance       = static_cast<VkInstance>(nativeInstance);
  m_physicalDevice = static_cast<VkPhysicalDevice>(nativePhysicalDevice);

  ASSERT(m_instance != VK_NULL_HANDLE, "VulkanSurface requires a valid VkInstance");
  ASSERT(m_physicalDevice != VK_NULL_HANDLE, "VulkanSurface requires a valid VkPhysicalDevice");
  ASSERT(m_surface == VK_NULL_HANDLE, "VulkanSurface::init called while surface is already initialized");
  ASSERT(window.nativeWindow != nullptr, "VulkanSurface requires a valid native window handle");

  GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(window.nativeWindow);
  VK_CHECK(glfwCreateWindowSurface(m_instance, glfwWindow, nullptr, &m_surface));
}

void VulkanSurface::deinit()
{
  if(m_surface != VK_NULL_HANDLE)
  {
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
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

uint64_t VulkanSurface::getNativeHandle() const
{
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_surface));
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
