#pragma once

#include "../RHISwapchain.h"

#include <vector>

namespace demo::rhi::d3d12 {

class D3D12Swapchain final : public demo::rhi::Swapchain
{
public:
  D3D12Swapchain() = default;

  void init(void* nativeDevice, void* nativeCommandQueue, void* nativeWindow, uint32_t maxFramesInFlight);
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

  // D3D12-native accessors (for backend interop)
  // NOTES: Returns opaque handles to D3D12 objects
  void* swapchain() const { return m_swapchain; }  // IDXGISwapChain3

private:
  struct ImageResource
  {
    void*         resource{nullptr};       // ID3D12Resource
    void*         rtvDescriptor{nullptr};  // D3D12_CPU_DESCRIPTOR_HANDLE
    TextureHandle handle{};
  };

  struct FrameResource
  {
    void*    fence{nullptr};       // ID3D12Fence
    void*    fenceEvent{nullptr};  // HANDLE
    uint64_t fenceValue{0};
  };

  Extent2D createResources();
  void     destroyResources();

  // NOTES: DXGI Swapchain vs RHI Swapchain mapping
  // D3D12 uses IDXGISwapChain3 for presentation:
  //
  // Initialization:
  // 1. Get DXGI factory from device: ID3D12Device::GetAdapter() → IDXGIAdapter → IDXGIFactory
  // 2. Create DXGI_SWAP_CHAIN_DESC1:
  //    - Width/Height: 0 (auto from window size)
  //    - Format: DXGI_FORMAT_R8G8B8A8_UNORM
  //    - BufferCount: maxFramesInFlight (usually 2 or 3)
  //    - SampleDesc: 1,0 (no MSAA for swapchain)
  //    - SwapEffect: DXGI_SWAP_EFFECT_FLIP_DISCARD (flip model for low latency)
  //    - BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT
  //    - AlphaMode: DXGI_ALPHA_MODE_IGNORE
  // 3. Create swapchain: CreateSwapChainForHwnd(commandQueue, hwnd, &desc1, nullptr, nullptr, &swapchain)
  // 4. Query IDXGISwapChain3 interface for GetCurrentBackBufferIndex()
  // 5. Create render target views for each backbuffer
  //
  // Acquisition (acquireNextImage):
  // - D3D12 doesn't have explicit acquire like Vulkan
  // - Use swapchain->GetCurrentBackBufferIndex() to get current buffer index
  // - No semaphore needed (flip model handles synchronization implicitly)
  //
  // Presentation (present):
  // - swapchain->Present(syncInterval, flags)
  // - syncInterval: 0 for vsync off, 1 for vsync on
  // - flags: 0 or DXGI_PRESENT_DO_NOT_WAIT
  // - Wait for fence before reusing backbuffer

  void* m_d3d12Device{nullptr};        // ID3D12Device
  void* m_commandQueue{nullptr};       // ID3D12CommandQueue
  void* m_swapchain{nullptr};          // IDXGISwapChain3
  void* m_rtvDescriptorHeap{nullptr};  // ID3D12DescriptorHeap (RTV heap)
  void* m_hwnd{nullptr};               // HWND (window handle)

  std::vector<ImageResource> m_images;
  std::vector<FrameResource> m_frameResources;

  uint32_t m_frameResourceIndex{0};
  uint32_t m_frameImageIndex{0};
  uint32_t m_maxFramesInFlight{3};
  uint32_t m_rtvDescriptorSize{0};  // D3D12_DESCRIPTOR_HEAP_TYPE_RTV size
  Extent2D m_extent{};
  bool     m_vSync{true};
  bool     m_needsRebuild{false};
};

}  // namespace demo::rhi::d3d12