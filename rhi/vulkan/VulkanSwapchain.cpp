#include "VulkanSwapchain.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#include "volk.h"

namespace demo::rhi::vulkan {
namespace {

uint64_t toNativeU64(uintptr_t value)
{
  return static_cast<uint64_t>(value);
}

void ensure(bool condition, const char* message)
{
  if(!condition)
  {
    throw std::runtime_error(message);
  }
}

void checkVk(VkResult result, const char* message)
{
  if(result != VK_SUCCESS)
  {
    throw std::runtime_error(message);
  }
}

}  // namespace

void VulkanSwapchain::init(void* nativePhysicalDevice, void* nativeDevice, void* nativeQueue, void* nativeSurface, void* nativeCmdPool, bool vSync)
{
  m_physicalDevice = static_cast<VkPhysicalDevice>(nativePhysicalDevice);
  m_device         = static_cast<VkDevice>(nativeDevice);
  m_queue          = static_cast<VkQueue>(nativeQueue);
  m_surface        = static_cast<VkSurfaceKHR>(nativeSurface);
  m_cmdPool        = static_cast<VkCommandPool>(nativeCmdPool);
  m_vSync          = vSync;

  ensure(m_physicalDevice != VK_NULL_HANDLE, "VulkanSwapchain::init requires VkPhysicalDevice");
  ensure(m_device != VK_NULL_HANDLE, "VulkanSwapchain::init requires VkDevice");
  ensure(m_queue != VK_NULL_HANDLE, "VulkanSwapchain::init requires VkQueue");
  ensure(m_surface != VK_NULL_HANDLE, "VulkanSwapchain::init requires VkSurfaceKHR");
  ensure(m_cmdPool != VK_NULL_HANDLE, "VulkanSwapchain::init requires VkCommandPool");
}

void VulkanSwapchain::deinit()
{
  destroyResources();
  m_physicalDevice = VK_NULL_HANDLE;
  m_device         = VK_NULL_HANDLE;
  m_queue          = VK_NULL_HANDLE;
  m_surface        = VK_NULL_HANDLE;
  m_cmdPool        = VK_NULL_HANDLE;
  m_imageFormat    = VK_FORMAT_UNDEFINED;
  m_extent         = {};
  m_needsRebuild   = false;
}

void VulkanSwapchain::requestRebuild()
{
  m_needsRebuild = true;
}

bool VulkanSwapchain::needsRebuild() const
{
  return m_needsRebuild;
}

void VulkanSwapchain::rebuild()
{
  ensure(m_queue != VK_NULL_HANDLE, "VulkanSwapchain::rebuild requires VkQueue");
  vkQueueWaitIdle(m_queue);
  m_frameResourceIndex = 0;
  m_frameImageIndex    = 0;
  m_needsRebuild       = false;
  destroyResources();
  createResources(m_vSync);
}

AcquireResult VulkanSwapchain::acquireNextImage()
{
  ensure(m_device != VK_NULL_HANDLE, "VulkanSwapchain::acquireNextImage requires VkDevice");
  ensure(m_swapchain != VK_NULL_HANDLE, "VulkanSwapchain::acquireNextImage requires initialized swapchain");
  ensure(!m_frameResources.empty(), "VulkanSwapchain::acquireNextImage requires frame resources");

  auto&          frame  = m_frameResources[m_frameResourceIndex];
  const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, (std::numeric_limits<uint64_t>::max)(),
                                                frame.imageAvailableSemaphore, VK_NULL_HANDLE, &m_frameImageIndex);
  if(result == VK_ERROR_OUT_OF_DATE_KHR)
  {
    m_needsRebuild = true;
    return AcquireResult{.texture = {}, .imageIndex = 0, .status = AcquireResult::Status::outOfDate};
  }

  ensure(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "VulkanSwapchain::acquireNextImage failed");
  if(result == VK_SUBOPTIMAL_KHR)
  {
    m_needsRebuild = true;
  }
  ensure(m_frameImageIndex < m_images.size(), "VulkanSwapchain::acquireNextImage invalid image index");
  return AcquireResult{.texture    = m_images[m_frameImageIndex].texture,
                       .imageIndex = m_frameImageIndex,
                       .status = result == VK_SUBOPTIMAL_KHR ? AcquireResult::Status::suboptimal : AcquireResult::Status::success};
}

PresentResult VulkanSwapchain::present()
{
  ensure(m_queue != VK_NULL_HANDLE, "VulkanSwapchain::present requires VkQueue");
  ensure(m_swapchain != VK_NULL_HANDLE, "VulkanSwapchain::present requires initialized swapchain");
  ensure(m_frameResourceIndex < m_frameResources.size(), "VulkanSwapchain::present invalid frame resource index");
  ensure(m_frameImageIndex < m_images.size(), "VulkanSwapchain::present invalid swapchain image index");

  auto&                  frame = m_frameResources[m_frameResourceIndex];
  const VkPresentInfoKHR presentInfo{
      .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores    = &frame.renderFinishedSemaphore,
      .swapchainCount     = 1,
      .pSwapchains        = &m_swapchain,
      .pImageIndices      = &m_frameImageIndex,
  };

  const VkResult result = vkQueuePresentKHR(m_queue, &presentInfo);
  PresentResult  presentResult{};
  if(result == VK_ERROR_OUT_OF_DATE_KHR)
  {
    m_needsRebuild       = true;
    presentResult.status = PresentResult::Status::outOfDate;
  }
  else
  {
    ensure(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "VulkanSwapchain::present failed");
    if(result == VK_SUBOPTIMAL_KHR)
    {
      m_needsRebuild = true;
    }
    presentResult.status = result == VK_SUBOPTIMAL_KHR ? PresentResult::Status::suboptimal : PresentResult::Status::success;
  }

  if(m_maxFramesInFlight > 0)
  {
    m_frameResourceIndex = (m_frameResourceIndex + 1) % m_maxFramesInFlight;
  }

  return presentResult;
}

TextureHandle VulkanSwapchain::currentTexture() const
{
  if(m_images.empty() || m_frameImageIndex >= m_images.size())
  {
    return {};
  }
  return m_images[m_frameImageIndex].texture;
}

Extent2D VulkanSwapchain::getExtent() const
{
  return m_extent;
}

uint32_t VulkanSwapchain::getMaxFramesInFlight() const
{
  return m_maxFramesInFlight;
}

uint64_t VulkanSwapchain::getNativeSwapchain() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_swapchain));
}

uint64_t VulkanSwapchain::getNativeImageView(uint32_t imageIndex) const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(nativeImageView(imageIndex)));
}

uint64_t VulkanSwapchain::getNativeImage(uint32_t imageIndex) const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(nativeImage(imageIndex)));
}

VkImageView VulkanSwapchain::nativeImageView(uint32_t imageIndex) const
{
  if(imageIndex >= m_images.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_images[imageIndex].imageView;
}

VkImage VulkanSwapchain::nativeImage(uint32_t imageIndex) const
{
  if(imageIndex >= m_images.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_images[imageIndex].image;
}

VkSemaphore VulkanSwapchain::imageAvailableSemaphoreForCurrentFrame() const
{
  if(m_frameResources.empty() || m_frameResourceIndex >= m_frameResources.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_frameResources[m_frameResourceIndex].imageAvailableSemaphore;
}

VkSemaphore VulkanSwapchain::renderFinishedSemaphoreForCurrentFrame() const
{
  if(m_frameResources.empty() || m_frameResourceIndex >= m_frameResources.size())
  {
    return VK_NULL_HANDLE;
  }
  return m_frameResources[m_frameResourceIndex].renderFinishedSemaphore;
}

Extent2D VulkanSwapchain::createResources(bool vSync)
{
  ensure(m_physicalDevice != VK_NULL_HANDLE, "VulkanSwapchain::createResources requires VkPhysicalDevice");
  ensure(m_device != VK_NULL_HANDLE, "VulkanSwapchain::createResources requires VkDevice");
  ensure(m_surface != VK_NULL_HANDLE, "VulkanSwapchain::createResources requires VkSurfaceKHR");

  const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                                     .surface = m_surface};
  VkSurfaceCapabilities2KHR             capabilities2{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
  checkVk(vkGetPhysicalDeviceSurfaceCapabilities2KHR(m_physicalDevice, &surfaceInfo2, &capabilities2),
          "VulkanSwapchain::createResources failed querying surface capabilities");

  uint32_t formatCount = 0;
  checkVk(vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, nullptr),
          "VulkanSwapchain::createResources failed querying surface format count");
  ensure(formatCount > 0, "VulkanSwapchain::createResources requires at least one surface format");

  std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
  checkVk(vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, formats.data()),
          "VulkanSwapchain::createResources failed querying surface formats");

  uint32_t presentModeCount = 0;
  checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr),
          "VulkanSwapchain::createResources failed querying present mode count");
  ensure(presentModeCount > 0, "VulkanSwapchain::createResources requires at least one present mode");

  std::vector<VkPresentModeKHR> presentModes(presentModeCount);
  checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data()),
          "VulkanSwapchain::createResources failed querying present modes");

  const VkSurfaceFormat2KHR surfaceFormat2 = selectSwapSurfaceFormat(formats);
  const VkPresentModeKHR    presentMode    = selectSwapPresentMode(presentModes, vSync);

  const VkExtent2D vkExtent = capabilities2.surfaceCapabilities.currentExtent;
  m_extent                  = {vkExtent.width, vkExtent.height};

  const uint32_t minImageCount       = capabilities2.surfaceCapabilities.minImageCount;
  const uint32_t preferredImageCount = (std::max)(3u, minImageCount);
  const uint32_t maxImageCount =
      capabilities2.surfaceCapabilities.maxImageCount == 0 ? preferredImageCount : capabilities2.surfaceCapabilities.maxImageCount;
  m_maxFramesInFlight = (std::clamp)(preferredImageCount, minImageCount, maxImageCount);
  m_imageFormat       = surfaceFormat2.surfaceFormat.format;

  const VkSwapchainCreateInfoKHR swapchainCreateInfo{
      .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface          = m_surface,
      .minImageCount    = m_maxFramesInFlight,
      .imageFormat      = surfaceFormat2.surfaceFormat.format,
      .imageColorSpace  = surfaceFormat2.surfaceFormat.colorSpace,
      .imageExtent      = vkExtent,
      .imageArrayLayers = 1,
      .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform     = capabilities2.surfaceCapabilities.currentTransform,
      .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode      = presentMode,
      .clipped          = VK_TRUE,
  };
  checkVk(vkCreateSwapchainKHR(m_device, &swapchainCreateInfo, nullptr, &m_swapchain),
          "VulkanSwapchain::createResources failed creating swapchain");

  uint32_t imageCount = 0;
  checkVk(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr),
          "VulkanSwapchain::createResources failed querying swapchain image count");
  ensure(imageCount > 0, "VulkanSwapchain::createResources requires at least one swapchain image");

  std::vector<VkImage> swapImages(imageCount);
  checkVk(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, swapImages.data()),
          "VulkanSwapchain::createResources failed querying swapchain images");

  m_maxFramesInFlight = imageCount;
  m_images.resize(imageCount);

  VkImageViewCreateInfo imageViewCreateInfo{
      .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format   = m_imageFormat,
      .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY},
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  for(uint32_t i = 0; i < imageCount; ++i)
  {
    m_images[i].image         = swapImages[i];
    m_images[i].texture       = TextureHandle{.index = i + 1, .generation = 1};
    imageViewCreateInfo.image = m_images[i].image;
    checkVk(vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &m_images[i].imageView),
            "VulkanSwapchain::createResources failed creating image view");
  }

  m_frameResources.resize(imageCount);
  const VkSemaphoreCreateInfo semaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for(auto& frame : m_frameResources)
  {
    checkVk(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &frame.imageAvailableSemaphore),
            "VulkanSwapchain::createResources failed creating image-available semaphore");
    checkVk(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &frame.renderFinishedSemaphore),
            "VulkanSwapchain::createResources failed creating render-finished semaphore");
  }

  return m_extent;
}

void VulkanSwapchain::destroyResources()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_images.clear();
    m_frameResources.clear();
    m_swapchain = VK_NULL_HANDLE;
    return;
  }

  for(auto& frame : m_frameResources)
  {
    if(frame.imageAvailableSemaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(m_device, frame.imageAvailableSemaphore, nullptr);
      frame.imageAvailableSemaphore = VK_NULL_HANDLE;
    }
    if(frame.renderFinishedSemaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(m_device, frame.renderFinishedSemaphore, nullptr);
      frame.renderFinishedSemaphore = VK_NULL_HANDLE;
    }
  }

  for(auto& image : m_images)
  {
    if(image.imageView != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, image.imageView, nullptr);
      image.imageView = VK_NULL_HANDLE;
    }
    image.image   = VK_NULL_HANDLE;
    image.texture = {};
  }

  if(m_swapchain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
  }

  m_images.clear();
  m_frameResources.clear();
}

VkSurfaceFormat2KHR VulkanSwapchain::selectSwapSurfaceFormat(const std::vector<VkSurfaceFormat2KHR>& availableFormats) const
{
  if(availableFormats.size() == 1 && availableFormats[0].surfaceFormat.format == VK_FORMAT_UNDEFINED)
  {
    return VkSurfaceFormat2KHR{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                               .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  }

  const auto preferredFormats = std::to_array<VkSurfaceFormat2KHR>({
      {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
      {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
  });

  for(const auto& preferred : preferredFormats)
  {
    for(const auto& available : availableFormats)
    {
      if(available.surfaceFormat.format == preferred.surfaceFormat.format
         && available.surfaceFormat.colorSpace == preferred.surfaceFormat.colorSpace)
      {
        return available;
      }
    }
  }

  return availableFormats[0];
}

VkPresentModeKHR VulkanSwapchain::selectSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vSync) const
{
  if(vSync)
  {
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  bool mailboxSupported   = false;
  bool immediateSupported = false;
  for(VkPresentModeKHR mode : availablePresentModes)
  {
    mailboxSupported   = mailboxSupported || (mode == VK_PRESENT_MODE_MAILBOX_KHR);
    immediateSupported = immediateSupported || (mode == VK_PRESENT_MODE_IMMEDIATE_KHR);
  }

  if(mailboxSupported)
  {
    return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  if(immediateSupported)
  {
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

}  // namespace demo::rhi::vulkan
