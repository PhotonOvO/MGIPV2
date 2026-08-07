#pragma once

#include "../RHISwapchain.h"

#include <cstdint>
#include <vector>

namespace demo::rhi::d3d12 {

class D3D12Device;

class D3D12Swapchain final : public demo::rhi::Swapchain
{
public:
  D3D12Swapchain() = default;
  ~D3D12Swapchain() override { deinit(); }

  void init(D3D12Device* owner, void* nativeFactory, void* nativeDevice,
            void* nativeCommandQueue, void* nativeWindow, bool vSync,
            uint32_t requestedImageCount = 3);
  void deinit() override;

  void        setVSync(bool vSync) override;
  void        setFullscreen(bool enabled, void* platformHandle) override;
  const char* getPresentModeName() const override;

  void          requestRebuild() override;
  bool          needsRebuild() const override;
  void          rebuild() override;
  AcquireResult acquireNextImage() override;
  PresentResult present() override;
  TextureHandle currentTexture() const override;
  TextureViewHandle textureView(uint32_t imageIndex) const override;
  Extent2D      getExtent() const override;
  uint32_t      getMaxFramesInFlight() const override;
  uint32_t      getRequestedImageCount() const override;
  TextureFormat getFormat() const override;

  [[nodiscard]] void* swapchain() const { return m_swapchain; }
  [[nodiscard]] uint64_t renderTargetDescriptor(uint32_t imageIndex) const;

private:
  struct ImageResource
  {
    void*             resource{nullptr};
    uint64_t          rtvDescriptor{0};
    TextureHandle     texture{};
    TextureViewHandle view{};
  };

  Extent2D queryClientExtent() const;
  void     createSwapchain(Extent2D extent);
  void     createBackBuffers();
  void     releaseBackBuffers();
  void     waitQueueIdle() const;

  D3D12Device* m_owner{nullptr};
  void* m_factory{nullptr};
  void* m_d3d12Device{nullptr};
  void* m_commandQueue{nullptr};
  void* m_swapchain{nullptr};
  void* m_rtvDescriptorHeap{nullptr};
  void* m_hwnd{nullptr};

  std::vector<ImageResource> m_images;

  uint32_t m_frameImageIndex{0};
  uint32_t m_requestedImageCount{3};
  uint32_t m_rtvDescriptorSize{0};
  Extent2D m_extent{};
  bool     m_vSync{true};
  bool     m_tearingSupported{false};
  bool     m_hasAcquiredImage{false};
  bool     m_needsRebuild{false};
};

}  // namespace demo::rhi::d3d12
