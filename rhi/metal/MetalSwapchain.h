#pragma once

#include "../RHISwapchain.h"

#include <vector>

namespace CA {
class MetalDrawable;
class MetalLayer;
}

namespace MTL {
class ResidencySet;
}

namespace demo::rhi::metal {

class MetalDevice;
class MetalQueue;
class MetalSurface;

class MetalSwapchain final : public Swapchain
{
public:
  MetalSwapchain() = default;
  ~MetalSwapchain() override;

  void init(MetalDevice& device, MetalQueue& queue, MetalSurface& surface,
            bool vSync, uint32_t maxFramesInFlight = 3);
  void deinit() override;

  void setVSync(bool vSync) override;
  [[nodiscard]] const char* getPresentModeName() const override;
  void requestRebuild() override;
  [[nodiscard]] bool needsRebuild() const override;
  void rebuild() override;
  [[nodiscard]] AcquireResult acquireNextImage() override;
  void encodePresent(CommandBuffer& commandBuffer) override;
  [[nodiscard]] PresentResult present() override;
  [[nodiscard]] TextureHandle currentTexture() const override;
  [[nodiscard]] TextureViewHandle textureView(uint32_t imageIndex) const override;
  [[nodiscard]] Extent2D getExtent() const override;
  [[nodiscard]] uint32_t getMaxFramesInFlight() const override;
  [[nodiscard]] uint32_t getRequestedImageCount() const override;
  [[nodiscard]] TextureFormat getFormat() const override;

private:
  struct ImageResource
  {
    TextureHandle texture{};
    TextureViewHandle view{};
    MTL::ResidencySet* presentResidencySet{nullptr};
    CA::MetalDrawable* pendingDrawable{nullptr};
    SubmissionToken presentSubmission{};
  };

  void createImageResources();
  void destroyImageResources();
  void retireImagePresentation(ImageResource& image);
  void releaseCurrentDrawable() noexcept;

  MetalDevice* m_device{nullptr};
  MetalQueue* m_queue{nullptr};
  MetalSurface* m_surface{nullptr};
  CA::MetalLayer* m_layer{nullptr};
  CA::MetalDrawable* m_currentDrawable{nullptr};
  std::vector<ImageResource> m_images;
  uint32_t m_frameImageIndex{0};
  uint32_t m_maxFramesInFlight{3};
  Extent2D m_extent{};
  bool m_vSync{true};
  bool m_hasAcquiredImage{false};
  bool m_presentEncoded{false};
  bool m_needsRebuild{true};
};

}  // namespace demo::rhi::metal
