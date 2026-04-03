#pragma once

#include "../RHISwapchain.h"

#include <vector>

namespace demo::rhi::metal {

class MetalSwapchain final : public demo::rhi::Swapchain
{
public:
  MetalSwapchain() = default;

  void init(void* nativeDevice, void* nativeQueue, void* nativeSurface, void* nativeLayer, uint32_t maxFramesInFlight);
  void deinit();

  void          requestRebuild() override;
  bool          needsRebuild() const override;
  void          rebuild() override;
  AcquireResult acquireNextImage() override;
  PresentResult present() override;
  TextureHandle currentTexture() const override;
  Extent2D      getExtent() const override;
  uint32_t      getMaxFramesInFlight() const override;

  uint64_t getNativeSwapchain() const override;
  uint64_t getNativeImageView(uint32_t imageIndex) const override;
  uint64_t getNativeImage(uint32_t imageIndex) const override;

  // Metal-native accessors (for backend interop)
  // NOTES: Returns opaque handles to Metal objects
  void* metalLayer() const { return m_metalLayer; }  // id<CAMetalLayer>
  void* currentDrawable() const;                     // id<CAMetalDrawable>

private:
  struct ImageResource
  {
    void*         texture{nullptr};  // id<MTLTexture>
    TextureHandle handle{};
  };

  struct FrameResource
  {
    // NOTES: Metal uses command buffer completion handlers instead of semaphores
    // No binary semaphores needed like Vulkan
    // Track completion via MTLCommandBuffer addCompletedHandler
  };

  Extent2D createResources();
  void     destroyResources();

  // NOTES: CAMetalLayer vs Swapchain mapping
  // Metal doesn't have a swapchain object like Vulkan
  // Instead, CAMetalLayer manages drawable acquisition
  // - CAMetalLayer is configured with pixelFormat, framebufferOnly, etc.
  // - nextDrawable returns a CAMetalDrawable with current texture
  // - Drawable must be presented after rendering completes
  //
  // Initialization:
  // 1. Get CAMetalLayer from view/window (nativeLayer parameter)
  // 2. Set layer.device = metalDevice
  // 3. Set layer.pixelFormat (MTLPixelFormatBGRA8Unorm for sRGB)
  // 4. Set layer.framebufferOnly = YES (optimization)
  // 5. Configure drawableSize to match window size
  //
  // Acquisition:
  // id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
  // MTLTexture* texture = drawable.texture;
  //
  // Presentation:
  // [commandBuffer presentDrawable:drawable];
  // [commandBuffer commit];

  void* m_metalDevice{nullptr};      // id<MTLDevice>
  void* m_metalQueue{nullptr};       // id<MTLCommandQueue>
  void* m_metalLayer{nullptr};       // id<CAMetalLayer>
  void* m_currentDrawable{nullptr};  // id<CAMetalDrawable> (current frame)

  std::vector<ImageResource> m_images;
  std::vector<FrameResource> m_frameResources;

  uint32_t m_frameResourceIndex{0};
  uint32_t m_frameImageIndex{0};
  uint32_t m_maxFramesInFlight{3};
  Extent2D m_extent{};
  bool     m_needsRebuild{false};
};

}  // namespace demo::rhi::metal