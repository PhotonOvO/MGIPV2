#include "D3D12Swapchain.h"

#include "D3D12Device.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace demo::rhi::d3d12 {
namespace {

void checkHresult(HRESULT result, const char* operation)
{
  if(FAILED(result))
  {
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x" << std::hex << static_cast<uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

template<typename T>
void releaseNative(void*& object) noexcept
{
  if(object != nullptr)
  {
    static_cast<T*>(object)->Release();
    object = nullptr;
  }
}

}  // namespace

void D3D12Swapchain::init(D3D12Device* owner, void* nativeFactory, void* nativeDevice,
                          void* nativeCommandQueue, void* nativeWindow, bool vSync,
                          uint32_t requestedImageCount)
{
  if(m_owner != nullptr)
    throw std::runtime_error("D3D12Swapchain::init called twice");
  if(owner == nullptr || nativeFactory == nullptr || nativeDevice == nullptr ||
     nativeCommandQueue == nullptr || nativeWindow == nullptr)
  {
    throw std::runtime_error("D3D12Swapchain::init requires device, factory, queue, and HWND");
  }

  m_owner = owner;
  m_factory = nativeFactory;
  m_d3d12Device = nativeDevice;
  m_commandQueue = nativeCommandQueue;
  m_hwnd = nativeWindow;
  m_vSync = vSync;
  m_requestedImageCount = std::clamp(requestedImageCount, 2u, 16u);

  BOOL allowTearing = FALSE;
  IDXGIFactory5* factory5 = nullptr;
  if(SUCCEEDED(static_cast<IDXGIFactory4*>(m_factory)->QueryInterface(IID_PPV_ARGS(&factory5))))
  {
    if(FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                            &allowTearing, sizeof(allowTearing))))
    {
      allowTearing = FALSE;
    }
    factory5->Release();
  }
  m_tearingSupported = allowTearing == TRUE;
  m_needsRebuild = true;
}

void D3D12Swapchain::deinit()
{
  if(m_commandQueue != nullptr)
  {
    try
    {
      waitQueueIdle();
    }
    catch(...)
    {
    }
  }

  releaseBackBuffers();
  releaseNative<ID3D12DescriptorHeap>(m_rtvDescriptorHeap);
  releaseNative<IDXGISwapChain3>(m_swapchain);

  m_owner = nullptr;
  m_factory = nullptr;
  m_d3d12Device = nullptr;
  m_commandQueue = nullptr;
  m_hwnd = nullptr;
  m_extent = {};
  m_frameImageIndex = 0;
  m_rtvDescriptorSize = 0;
  m_hasAcquiredImage = false;
  m_needsRebuild = false;
}

void D3D12Swapchain::setVSync(bool vSync)
{
  m_vSync = vSync;
}

void D3D12Swapchain::setFullscreen(bool enabled, void*)
{
  if(m_swapchain == nullptr)
    return;

  checkHresult(static_cast<IDXGISwapChain3*>(m_swapchain)->SetFullscreenState(enabled ? TRUE : FALSE, nullptr),
               "IDXGISwapChain::SetFullscreenState");
  requestRebuild();
}

const char* D3D12Swapchain::getPresentModeName() const
{
  if(m_vSync)
    return "FIFO";
  return m_tearingSupported ? "Immediate (tearing)" : "Immediate";
}

void D3D12Swapchain::requestRebuild()
{
  m_needsRebuild = true;
}

bool D3D12Swapchain::needsRebuild() const
{
  if(m_needsRebuild)
    return true;
  const Extent2D client = queryClientExtent();
  return client.width != m_extent.width || client.height != m_extent.height;
}

void D3D12Swapchain::rebuild()
{
  if(m_owner == nullptr)
    throw std::runtime_error("D3D12Swapchain::rebuild called before init");

  const Extent2D nextExtent = queryClientExtent();
  if(nextExtent.width == 0 || nextExtent.height == 0)
  {
    m_needsRebuild = true;
    return;
  }

  waitQueueIdle();
  m_hasAcquiredImage = false;

  if(m_swapchain == nullptr)
  {
    createSwapchain(nextExtent);
  }
  else
  {
    releaseBackBuffers();
    const UINT flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    checkHresult(static_cast<IDXGISwapChain3*>(m_swapchain)->ResizeBuffers(
                   m_requestedImageCount, nextExtent.width, nextExtent.height,
                   DXGI_FORMAT_B8G8R8A8_UNORM, flags),
                 "IDXGISwapChain::ResizeBuffers");
  }

  m_extent = nextExtent;
  createBackBuffers();
  m_frameImageIndex = static_cast<IDXGISwapChain3*>(m_swapchain)->GetCurrentBackBufferIndex();
  m_needsRebuild = false;
}

AcquireResult D3D12Swapchain::acquireNextImage()
{
  if(m_swapchain == nullptr || m_images.empty())
    return {.status = AcquireResult::Status::outOfDate};

  if(needsRebuild())
  {
    m_needsRebuild = true;
    return {.status = AcquireResult::Status::outOfDate};
  }

  m_frameImageIndex = static_cast<IDXGISwapChain3*>(m_swapchain)->GetCurrentBackBufferIndex();
  if(m_frameImageIndex >= m_images.size())
    throw std::runtime_error("D3D12Swapchain returned an invalid back-buffer index");

  m_hasAcquiredImage = true;
  return {
    .texture = m_images[m_frameImageIndex].texture,
    .imageIndex = m_frameImageIndex,
    .status = AcquireResult::Status::success,
  };
}

PresentResult D3D12Swapchain::present()
{
  if(m_swapchain == nullptr || !m_hasAcquiredImage)
    return {};

  UINT flags = 0;
  if(!m_vSync && m_tearingSupported)
    flags |= DXGI_PRESENT_ALLOW_TEARING;

  const HRESULT result = static_cast<IDXGISwapChain3*>(m_swapchain)->Present(m_vSync ? 1u : 0u, flags);
  m_hasAcquiredImage = false;

  if(result == DXGI_STATUS_OCCLUDED)
    return {};
  if(result == DXGI_ERROR_INVALID_CALL || result == DXGI_ERROR_MODE_CHANGE_IN_PROGRESS)
  {
    m_needsRebuild = true;
    return {.status = PresentResult::Status::outOfDate};
  }
  checkHresult(result, "IDXGISwapChain::Present");
  return {};
}

TextureHandle D3D12Swapchain::currentTexture() const
{
  if(!m_hasAcquiredImage || m_frameImageIndex >= m_images.size())
    return {};
  return m_images[m_frameImageIndex].texture;
}

TextureViewHandle D3D12Swapchain::textureView(uint32_t imageIndex) const
{
  if(imageIndex >= m_images.size())
    return {};
  return m_images[imageIndex].view;
}

Extent2D D3D12Swapchain::getExtent() const
{
  return m_extent;
}

uint32_t D3D12Swapchain::getMaxFramesInFlight() const
{
  return static_cast<uint32_t>(m_images.empty() ? m_requestedImageCount : m_images.size());
}

uint32_t D3D12Swapchain::getRequestedImageCount() const
{
  return m_requestedImageCount;
}

TextureFormat D3D12Swapchain::getFormat() const
{
  return TextureFormat::bgra8Unorm;
}

uint64_t D3D12Swapchain::renderTargetDescriptor(uint32_t imageIndex) const
{
  if(imageIndex >= m_images.size())
    return 0;
  return m_images[imageIndex].rtvDescriptor;
}

Extent2D D3D12Swapchain::queryClientExtent() const
{
  const HWND hwnd = static_cast<HWND>(m_hwnd);
  if(hwnd == nullptr || !IsWindow(hwnd))
    return {};

  RECT rect{};
  if(!GetClientRect(hwnd, &rect))
    return {};
  return {
    static_cast<uint32_t>(std::max<LONG>(0, rect.right - rect.left)),
    static_cast<uint32_t>(std::max<LONG>(0, rect.bottom - rect.top)),
  };
}

void D3D12Swapchain::createSwapchain(Extent2D extent)
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    .NumDescriptors = m_requestedImageCount,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    .NodeMask = 0,
  };
  ID3D12DescriptorHeap* heap = nullptr;
  checkHresult(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)),
               "ID3D12Device::CreateDescriptorHeap(RTV)");
  m_rtvDescriptorHeap = heap;
  m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  const UINT swapchainFlags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  const DXGI_SWAP_CHAIN_DESC1 desc{
    .Width = extent.width,
    .Height = extent.height,
    .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
    .Stereo = FALSE,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
    .BufferCount = m_requestedImageCount,
    .Scaling = DXGI_SCALING_STRETCH,
    .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
    .Flags = swapchainFlags,
  };

  IDXGISwapChain1* swapchain1 = nullptr;
  checkHresult(static_cast<IDXGIFactory4*>(m_factory)->CreateSwapChainForHwnd(
                 static_cast<ID3D12CommandQueue*>(m_commandQueue), static_cast<HWND>(m_hwnd),
                 &desc, nullptr, nullptr, &swapchain1),
               "IDXGIFactory::CreateSwapChainForHwnd");
  static_cast<IDXGIFactory4*>(m_factory)->MakeWindowAssociation(
    static_cast<HWND>(m_hwnd), DXGI_MWA_NO_ALT_ENTER);

  IDXGISwapChain3* swapchain3 = nullptr;
  const HRESULT queryResult = swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain3));
  swapchain1->Release();
  checkHresult(queryResult, "IDXGISwapChain::QueryInterface(IDXGISwapChain3)");
  m_swapchain = swapchain3;
}

void D3D12Swapchain::createBackBuffers()
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  auto* heap = static_cast<ID3D12DescriptorHeap*>(m_rtvDescriptorHeap);
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor = heap->GetCPUDescriptorHandleForHeapStart();

  m_images.reserve(m_requestedImageCount);
  try
  {
    for(uint32_t index = 0; index < m_requestedImageCount; ++index)
    {
      ID3D12Resource* resource = nullptr;
      checkHresult(static_cast<IDXGISwapChain3*>(m_swapchain)->GetBuffer(index, IID_PPV_ARGS(&resource)),
                   "IDXGISwapChain::GetBuffer");

      device->CreateRenderTargetView(resource, nullptr, descriptor);
      const TextureHandle texture = m_owner->adoptSwapchainTexture(resource);
      const TextureViewHandle view = m_owner->adoptSwapchainTextureView(static_cast<uint64_t>(descriptor.ptr));
      m_images.push_back({resource, static_cast<uint64_t>(descriptor.ptr), texture, view});

      descriptor.ptr += m_rtvDescriptorSize;
    }
  }
  catch(...)
  {
    releaseBackBuffers();
    throw;
  }
}

void D3D12Swapchain::releaseBackBuffers()
{
  for(ImageResource& image : m_images)
  {
    if(m_owner != nullptr)
    {
      m_owner->destroyTextureView(image.view);
      m_owner->destroyTexture(image.texture);
    }
    if(image.resource != nullptr)
      static_cast<ID3D12Resource*>(image.resource)->Release();
  }
  m_images.clear();
}

void D3D12Swapchain::waitQueueIdle() const
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  auto* queue = static_cast<ID3D12CommandQueue*>(m_commandQueue);
  if(device == nullptr || queue == nullptr)
    return;

  ID3D12Fence* fence = nullptr;
  checkHresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
               "ID3D12Device::CreateFence(swapchain idle)");
  HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if(eventHandle == nullptr)
  {
    fence->Release();
    throw std::runtime_error("CreateEventW failed for D3D12 swapchain fence");
  }

  const HRESULT signalResult = queue->Signal(fence, 1);
  if(SUCCEEDED(signalResult) && fence->GetCompletedValue() < 1)
  {
    checkHresult(fence->SetEventOnCompletion(1, eventHandle),
                 "ID3D12Fence::SetEventOnCompletion(swapchain idle)");
    WaitForSingleObject(eventHandle, INFINITE);
  }
  CloseHandle(eventHandle);
  fence->Release();
  checkHresult(signalResult, "ID3D12CommandQueue::Signal(swapchain idle)");
}

}  // namespace demo::rhi::d3d12
