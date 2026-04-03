#pragma once

#include "../common/Common.h"

namespace utils {

class Swapchain
{
public:
  Swapchain() = default;
  ~Swapchain() { assert(m_swapChain == VK_NULL_HANDLE && "Missing deinit()"); }

  void        requestRebuild() { m_needRebuild = true; }
  bool        needRebuilding() const { return m_needRebuild; }
  VkImage     getImage() const { return m_nextImages[m_frameImageIndex].image; }
  VkImageView getImageView() const { return m_nextImages[m_frameImageIndex].imageView; }
  VkFormat    getImageFormat() const { return m_imageFormat; }
  uint32_t    getMaxFramesInFlight() const { return m_maxFramesInFlight; }
  VkSemaphore getImageAvailableSemaphore() const
  {
    return m_frameResources[m_frameResourceIndex].imageAvailableSemaphore;
  }
  VkSemaphore getRenderFinishedSemaphore() const { return m_frameResources[m_frameImageIndex].renderFinishedSemaphore; }

  // Initialize the swapchain with the provided context and surface, then we can create and re-create it
  void init(VkPhysicalDevice physicalDevice, VkDevice device, const QueueInfo& queue, VkSurfaceKHR surface, VkCommandPool cmdPool)
  {
    m_physicalDevice = physicalDevice;
    m_device         = device;
    m_queue          = queue;
    m_surface        = surface;
    m_cmdPool        = cmdPool;
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    deinitResources();
    *this = {};
  }

  /*--
   * Create the swapchain using the provided context, surface, and vSync option. The actual window size is returned.
   * Queries the GPU capabilities, selects the best surface format and present mode, and creates the swapchain accordingly.
  -*/
  VkExtent2D initResources(bool vSync = true)
  {
    VkExtent2D outWindowSize;

    // Query the physical device's capabilities for the given surface.
    const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                                       .surface = m_surface};
    VkSurfaceCapabilities2KHR             capabilities2{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    vkGetPhysicalDeviceSurfaceCapabilities2KHR(m_physicalDevice, &surfaceInfo2, &capabilities2);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, nullptr);
    std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
    vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, formats.data());

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());

    // Choose the best available surface format and present mode
    const VkSurfaceFormat2KHR surfaceFormat2 = selectSwapSurfaceFormat(formats);
    const VkPresentModeKHR    presentMode    = selectSwapPresentMode(presentModes, vSync);
    // Set the window size according to the surface's current extent
    outWindowSize = capabilities2.surfaceCapabilities.currentExtent;

    // Adjust the number of images in flight within GPU limitations
    uint32_t minImageCount       = capabilities2.surfaceCapabilities.minImageCount;  // Vulkan-defined minimum
    uint32_t preferredImageCount = std::max(3u, minImageCount);  // Prefer 3, but respect minImageCount

    // Handle the maxImageCount case where 0 means "no upper limit"
    uint32_t maxImageCount = (capabilities2.surfaceCapabilities.maxImageCount == 0) ? preferredImageCount :  // No upper limit, use preferred
                                 capabilities2.surfaceCapabilities.maxImageCount;

    // Clamp preferredImageCount to valid range [minImageCount, maxImageCount]
    m_maxFramesInFlight = std::clamp(preferredImageCount, minImageCount, maxImageCount);

    // Store the chosen image format
    m_imageFormat = surfaceFormat2.surfaceFormat.format;

    // Create the swapchain itself
    const VkSwapchainCreateInfoKHR swapchainCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = m_surface,
        .minImageCount    = m_maxFramesInFlight,
        .imageFormat      = surfaceFormat2.surfaceFormat.format,
        .imageColorSpace  = surfaceFormat2.surfaceFormat.colorSpace,
        .imageExtent      = capabilities2.surfaceCapabilities.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = capabilities2.surfaceCapabilities.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = presentMode,
        .clipped          = VK_TRUE,
    };
    VK_CHECK(vkCreateSwapchainKHR(m_device, &swapchainCreateInfo, nullptr, &m_swapChain));
    DBG_VK_NAME(m_swapChain);

    // Retrieve the swapchain images
    {
      uint32_t imageCount = 0;
      vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, nullptr);
      ASSERT(m_maxFramesInFlight <= imageCount, "Wrong swapchain setup");
      m_maxFramesInFlight = imageCount;  // Use the number of images in the swapchain
    }
    std::vector<VkImage> swapImages(m_maxFramesInFlight);
    vkGetSwapchainImagesKHR(m_device, m_swapChain, &m_maxFramesInFlight, swapImages.data());

    // Store the swapchain images and create views for them
    m_nextImages.resize(m_maxFramesInFlight);
    VkImageViewCreateInfo imageViewCreateInfo{
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = m_imageFormat,
        .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    for(uint32_t i = 0; i < m_maxFramesInFlight; i++)
    {
      m_nextImages[i].image = swapImages[i];
      DBG_VK_NAME(m_nextImages[i].image);
      imageViewCreateInfo.image = m_nextImages[i].image;
      VK_CHECK(vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &m_nextImages[i].imageView));
      DBG_VK_NAME(m_nextImages[i].imageView);
    }

    // Initialize frame resources for each frame
    m_frameResources.resize(m_maxFramesInFlight);
    for(size_t i = 0; i < m_maxFramesInFlight; ++i)
    {
      /*--
       * The sync objects are used to synchronize the rendering with the presentation.
       * The image available semaphore is signaled when the image is available to render.
       * The render finished semaphore is signaled when the rendering is finished.
       * The in flight fence is signaled when the frame is in flight.
      -*/
      const VkSemaphoreCreateInfo semaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VK_CHECK(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_frameResources[i].imageAvailableSemaphore));
      DBG_VK_NAME(m_frameResources[i].imageAvailableSemaphore);
      VK_CHECK(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_frameResources[i].renderFinishedSemaphore));
      DBG_VK_NAME(m_frameResources[i].renderFinishedSemaphore);
    }

    // Transition images to present layout
    {
      VkCommandBuffer cmd = utils::beginSingleTimeCommands(m_device, m_cmdPool);
      for(uint32_t i = 0; i < m_maxFramesInFlight; i++)
      {
        cmdTransitionSwapchainLayout(cmd, m_nextImages[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
      }
      utils::endSingleTimeCommands(cmd, m_device, m_cmdPool, m_queue.queue);
    }

    return outWindowSize;
  }

  /*--
   * Recreate the swapchain, typically after a window resize or when it becomes invalid.
   * This waits for all rendering to be finished before destroying the old swapchain and creating a new one.
  -*/
  VkExtent2D reinitResources(bool vSync = true)
  {
    // Wait for all frames to finish rendering before recreating the swapchain
    vkQueueWaitIdle(m_queue.queue);

    m_frameResourceIndex = 0;
    m_needRebuild        = false;
    deinitResources();
    return initResources(vSync);
  }

  /*--
   * Destroy the swapchain and its associated resources.
   * This function is also called when the swapchain needs to be recreated.
  -*/
  void deinitResources()
  {
    vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
    for(auto& frameRes : m_frameResources)
    {
      vkDestroySemaphore(m_device, frameRes.imageAvailableSemaphore, nullptr);
      vkDestroySemaphore(m_device, frameRes.renderFinishedSemaphore, nullptr);
    }
    for(auto& image : m_nextImages)
    {
      vkDestroyImageView(m_device, image.imageView, nullptr);
    }
  }

  /*--
   * Prepares the command buffer for recording rendering commands.
   * This function handles synchronization with the previous frame and acquires the next image from the swapchain.
   * The command buffer is reset, ready for new rendering commands.
  -*/
  VkResult acquireNextImage(VkDevice device)
  {
    ASSERT(m_needRebuild == false, "Swapbuffer need to call reinitResources()");

    auto& frame = m_frameResources[m_frameResourceIndex];

    // Acquire the next image from the swapchain
    const VkResult result = vkAcquireNextImageKHR(device, m_swapChain, std::numeric_limits<uint64_t>::max(),
                                                  frame.imageAvailableSemaphore, VK_NULL_HANDLE, &m_frameImageIndex);
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("AcquireNextImage: \t frameRes=%u imageIndex=%u", m_frameResourceIndex, m_frameImageIndex);
#endif
    // Handle special case if the swapchain is out of date (e.g., window resize)
    if(result == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_needRebuild = true;  // Swapchain must be rebuilt on the next frame
    }
    else
    {
      ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "Couldn't aquire swapchain image");
    }
    return result;
  }

  /*--
   * Presents the rendered image to the screen.
   * The semaphore ensures that the image is presented only after rendering is complete.
   * Advances to the next frame in the cycle.
  -*/
  void presentFrame(VkQueue queue)
  {
    auto& frame = m_frameResources[m_frameImageIndex];

    // Setup the presentation info, linking the swapchain and the image index
    const VkPresentInfoKHR presentInfo{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,                               // Wait for rendering to finish
        .pWaitSemaphores    = &frame.renderFinishedSemaphore,  // Synchronize presentation
        .swapchainCount     = 1,                               // Swapchain to present the image
        .pSwapchains        = &m_swapChain,                    // Pointer to the swapchain
        .pImageIndices      = &m_frameImageIndex,              // Index of the image to present
    };

    // Present the image and handle potential resizing issues
    const VkResult result = vkQueuePresentKHR(queue, &presentInfo);
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("PresentFrame: \t\t frameRes=%u imageIndex=%u", m_frameResourceIndex, m_frameImageIndex);
#endif
    // If the swapchain is out of date (e.g., window resized), it needs to be rebuilt
    if(result == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_needRebuild = true;
    }
    else
    {
      ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "Couldn't present swapchain image");
    }

    // Advance to the next frame in the swapchain
    m_frameResourceIndex = (m_frameResourceIndex + 1) % m_maxFramesInFlight;
  }

private:
  // Represents an image within the swapchain that can be rendered to.
  struct Image
  {
    VkImage     image{};      // Image to render to
    VkImageView imageView{};  // Image view to access the image
  };
  /*--
   * Resources associated with each frame being processed.
   * Each frame has its own set of resources, mainly synchronization primitives
  -*/
  struct FrameResources
  {
    VkSemaphore imageAvailableSemaphore{};  // Signals when the image is ready for rendering
    VkSemaphore renderFinishedSemaphore{};  // Signals when rendering is finished
  };

  // We choose the format that is the most common, and that is supported by* the physical device.
  VkSurfaceFormat2KHR selectSwapSurfaceFormat(const std::vector<VkSurfaceFormat2KHR>& availableFormats) const
  {
    // If there's only one available format and it's undefined, return a default format.
    if(availableFormats.size() == 1 && availableFormats[0].surfaceFormat.format == VK_FORMAT_UNDEFINED)
    {
      VkSurfaceFormat2KHR result{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                                 .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
      return result;
    }

    const auto preferredFormats = std::to_array<VkSurfaceFormat2KHR>({
        {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
        {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
    });

    // Check available formats against the preferred formats.
    for(const auto& preferredFormat : preferredFormats)
    {
      for(const auto& availableFormat : availableFormats)
      {
        if(availableFormat.surfaceFormat.format == preferredFormat.surfaceFormat.format
           && availableFormat.surfaceFormat.colorSpace == preferredFormat.surfaceFormat.colorSpace)
        {
          return availableFormat;  // Return the first matching preferred format.
        }
      }
    }

    // If none of the preferred formats are available, return the first available format.
    return availableFormats[0];
  }

  /*--
   * The present mode is chosen based on the vSync option
   * The FIFO mode is the most common, and is used when vSync is enabled.
   * The MAILBOX mode is used when vSync is disabled, and is the best mode for triple buffering.
   * The IMMEDIATE mode is used when vSync is disabled, and is the best mode for low latency.
  -*/
  VkPresentModeKHR selectSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vSync = true)
  {
    if(vSync)
    {
      return VK_PRESENT_MODE_FIFO_KHR;
    }

    bool mailboxSupported = false, immediateSupported = false;

    for(VkPresentModeKHR mode : availablePresentModes)
    {
      if(mode == VK_PRESENT_MODE_MAILBOX_KHR)
        mailboxSupported = true;
      if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        immediateSupported = true;
    }

    if(mailboxSupported)
    {
      return VK_PRESENT_MODE_MAILBOX_KHR;
    }

    if(immediateSupported)
    {
      return VK_PRESENT_MODE_IMMEDIATE_KHR;  // Best mode for low latency
    }

    return VK_PRESENT_MODE_FIFO_KHR;  // Fallback to FIFO if neither MAILBOX nor IMMEDIATE is available
  }

private:
  VkPhysicalDevice m_physicalDevice{};  // The physical device (GPU)
  VkDevice         m_device{};          // The logical device (interface to the physical device)
  QueueInfo        m_queue{};           // The queue used to submit command buffers to the GPU
  VkSwapchainKHR   m_swapChain{};       // The swapchain
  VkFormat         m_imageFormat{};     // The format of the swapchain images
  VkSurfaceKHR     m_surface{};         // The surface to present images to
  VkCommandPool    m_cmdPool{};         // The command pool for the swapchain

  std::vector<Image>          m_nextImages;
  std::vector<FrameResources> m_frameResources;
  uint32_t                    m_frameResourceIndex = 0;
  uint32_t                    m_frameImageIndex    = 0;
  bool                        m_needRebuild        = false;

  uint32_t m_maxFramesInFlight = 3;  // Best for pretty much all cases
};

}
