#pragma once

#include "../RHISwapchain.h"

#include <vector>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

class VulkanSwapchain final : public demo::rhi::Swapchain
{
public:
  VulkanSwapchain() = default;

  void init(void* nativePhysicalDevice, void* nativeDevice, void* nativeQueue, void* nativeSurface, void* nativeCmdPool, bool vSync);
  void deinit();
  void setVSync(bool vSync);
  [[nodiscard]] bool             getVSync() const { return m_vSync; }
  [[nodiscard]] VkPresentModeKHR getPresentMode() const { return m_presentMode; }
  void                             set_fullscreen(bool fullscreen, void* platform_handle = nullptr);

  void          requestRebuild() override;
  bool          needsRebuild() const override;
  void          rebuild() override;
  AcquireResult acquireNextImage() override;
  PresentResult present() override;
  TextureHandle currentTexture() const override;
  Extent2D      getExtent() const override;
  uint32_t      getMaxFramesInFlight() const override;
  uint32_t      getRequestedImageCount() const { return m_requestedImageCount; }

  uint64_t                     getNativeSwapchain() const override;
  uint64_t                     getNativeImageView(uint32_t imageIndex) const override;
  uint64_t                     getNativeImage(uint32_t imageIndex) const override;
  [[nodiscard]] VkSwapchainKHR nativeSwapchain() const { return m_swapchain; }
  [[nodiscard]] VkImageView    nativeImageView(uint32_t imageIndex) const;
  [[nodiscard]] VkImage        nativeImage(uint32_t imageIndex) const;
  [[nodiscard]] VkSemaphore    imageAvailableSemaphoreForCurrentFrame() const;
  [[nodiscard]] VkSemaphore    renderFinishedSemaphoreForCurrentImage() const;

private:
  struct ImageResource
  {
    VkImage       image{VK_NULL_HANDLE};
    VkImageView   imageView{VK_NULL_HANDLE};
    VkSemaphore   renderFinishedSemaphore{VK_NULL_HANDLE};
    TextureHandle texture{};
  };

  struct FrameResource
  {
    VkSemaphore imageAvailableSemaphore{VK_NULL_HANDLE};
  };

  Extent2D createResources(bool vSync);
  void     destroyResources();

  VkSurfaceFormat2KHR selectSwapSurfaceFormat(const std::vector<VkSurfaceFormat2KHR>& availableFormats) const;
  VkPresentModeKHR selectSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vSync) const;

  VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
  VkDevice         m_device{VK_NULL_HANDLE};
  VkQueue          m_queue{VK_NULL_HANDLE};
  VkSurfaceKHR     m_surface{VK_NULL_HANDLE};
  VkCommandPool    m_cmdPool{VK_NULL_HANDLE};
  VkSwapchainKHR   m_swapchain{VK_NULL_HANDLE};
  VkFormat         m_imageFormat{VK_FORMAT_UNDEFINED};

  std::vector<ImageResource> m_images;
  std::vector<FrameResource> m_frameResources;

  uint32_t m_frameResourceIndex{0};
  uint32_t m_frameImageIndex{0};
  bool     m_hasAcquiredImage{false};
  uint32_t m_maxFramesInFlight{3};
  uint32_t m_requestedImageCount{3};
  Extent2D m_extent{};
  bool     m_vSync{true};
  bool     m_needsRebuild{false};
  VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_FIFO_KHR};
  bool     m_fullscreen{false};
  void*    m_platform_handle{nullptr};
};

}  // namespace demo::rhi::vulkan
