#include "MetalSurface.h"

#include "MetalCommon.h"

#include <QuartzCore/CAMetalLayer.hpp>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdexcept>

struct GLFWwindow;
extern "C" NSWindow* glfwGetCocoaWindow(GLFWwindow*) __attribute__((weak_import));

namespace demo::rhi::metal {

MetalSurface::~MetalSurface()
{
  deinit();
}

void MetalSurface::init(MTL::Device* device, const WindowHandle& window)
{
  require(device != nullptr, "MetalSurface requires a Metal device");
  require(window.nativeWindow != nullptr, "MetalSurface requires a GLFW window");
  require(m_layer == nullptr, "MetalSurface::init called more than once");
  require([NSThread isMainThread], "MetalSurface must be initialized on the main thread");
  require(glfwGetCocoaWindow != nullptr,
          "MetalSurface requires GLFW with Cocoa native-window support");

  NSWindow* cocoaWindow = glfwGetCocoaWindow(
    static_cast<GLFWwindow*>(window.nativeWindow));
  NSView* contentView = cocoaWindow.contentView;
  require(cocoaWindow != nil && contentView != nil,
          "MetalSurface failed to resolve the Cocoa content view");

  CA::MetalLayer* layer = CA::MetalLayer::layer();
  require(layer != nullptr, "MetalSurface failed to create CAMetalLayer");
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

  contentView.wantsLayer = YES;
  contentView.layer = reinterpret_cast<CAMetalLayer*>(layer);
  reinterpret_cast<CAMetalLayer*>(layer).frame = contentView.bounds;

  m_device = device;
  m_layer = layer;
  m_nativeView = contentView;
  updateDrawableSize();
}

void MetalSurface::deinit()
{
  if(m_layer == nullptr)
    return;
  if([NSThread isMainThread] && m_nativeView != nullptr)
  {
    NSView* view = static_cast<NSView*>(m_nativeView);
    if(view.layer == reinterpret_cast<CAMetalLayer*>(m_layer))
      view.layer = nil;
  }
  m_layer->release();
  m_layer = nullptr;
  m_device = nullptr;
  m_nativeView = nullptr;
  m_extent = {};
}

void MetalSurface::updateDrawableSize()
{
  require(m_layer != nullptr && m_nativeView != nullptr,
          "MetalSurface::updateDrawableSize requires initialization");
  require([NSThread isMainThread],
          "MetalSurface drawable size must be updated on the main thread");
  NSView* view = static_cast<NSView*>(m_nativeView);
  const NSRect backingBounds = [view convertRectToBacking:view.bounds];
  const uint32_t width = static_cast<uint32_t>(
    std::max<CGFloat>(0.0, backingBounds.size.width));
  const uint32_t height = static_cast<uint32_t>(
    std::max<CGFloat>(0.0, backingBounds.size.height));
  reinterpret_cast<CAMetalLayer*>(m_layer).frame = view.bounds;
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
