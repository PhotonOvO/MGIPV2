#include "MetalSurface.h"

#include "MetalCommon.h"

#include <QuartzCore/CAMetalLayer.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace demo::rhi::metal {

MetalSurface::~MetalSurface()
{
  deinit();
}

void MetalSurface::init(MTL::Device* device, const WindowHandle& window)
{
  require(device != nullptr, "MetalSurface requires a Metal device");
  require(window.nativeWindow != nullptr, "MetalSurface requires an SDL window");
  require(m_layer == nullptr, "MetalSurface::init called more than once");

  SDL_Window* sdlWindow = static_cast<SDL_Window*>(window.nativeWindow);
  SDL_MetalView metalView = SDL_Metal_CreateView(sdlWindow);
  require(metalView != nullptr, "MetalSurface failed to create an SDL Metal view");
  CA::MetalLayer* layer = reinterpret_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(metalView));
  if(layer == nullptr)
  {
    SDL_Metal_DestroyView(metalView);
    throw std::runtime_error("MetalSurface failed to resolve SDL's CAMetalLayer");
  }
  layer->retain();
  layer->setDevice(device);
  layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  // The final Metal 4 compute copy writes the drawable; render encoders only
  // target the swapchain's offscreen composition textures.
  layer->setFramebufferOnly(false);
  layer->setMaximumDrawableCount(3);
  // A timed-out acquire skips presentation after CAMetalLayer has already
  // replaced its backing during live resize, which exposes an empty layer for
  // that display refresh. Keep acquire blocking so every rendered frame has a
  // drawable to present; iconified/zero-sized windows are filtered by the app
  // and swapchain before nextDrawable() is called.
  layer->setAllowsNextDrawableTimeout(false);

  m_device = device;
  m_layer = layer;
  m_window = sdlWindow;
  m_metalView = metalView;
  updateDrawableSize();
}

void MetalSurface::deinit()
{
  if(m_layer == nullptr)
    return;
  m_layer->release();
  if(m_metalView != nullptr)
    SDL_Metal_DestroyView(m_metalView);
  m_layer = nullptr;
  m_device = nullptr;
  m_window = nullptr;
  m_metalView = nullptr;
  m_extent = {};
}

void MetalSurface::updateDrawableSize()
{
  require(m_layer != nullptr && m_window != nullptr,
          "MetalSurface::updateDrawableSize requires initialization");
  int pixelWidth = 0;
  int pixelHeight = 0;
  require(SDL_GetWindowSizeInPixels(m_window, &pixelWidth, &pixelHeight),
          "MetalSurface failed to query SDL drawable size");
  const uint32_t width = static_cast<uint32_t>(std::max(pixelWidth, 0));
  const uint32_t height = static_cast<uint32_t>(std::max(pixelHeight, 0));
  m_layer->setDrawableSize(CGSizeMake(width, height));
  m_extent = {width, height};
}

SurfaceCapabilities MetalSurface::queryCapabilities() const
{
  require(m_layer != nullptr, "MetalSurface::queryCapabilities requires initialization");
  return SurfaceCapabilities{
    .minImageCount = 2,
    .maxImageCount = 3,
    .currentExtent = m_extent,
    .minImageExtent = {1, 1},
    .maxImageExtent = {16384, 16384},
    .currentTransform = 0,
    .supportedUsageFlags = static_cast<uint32_t>(
      TextureUsageFlags::colorAttachment | TextureUsageFlags::transferSrc |
      TextureUsageFlags::transferDst),
  };
}

}  // namespace demo::rhi::metal
