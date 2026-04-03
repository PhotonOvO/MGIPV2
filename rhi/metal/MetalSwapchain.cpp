#include "MetalSwapchain.h"

#include <cassert>

namespace demo::rhi::metal {

void MetalSwapchain::init(void* nativeDevice, void* nativeQueue, void* nativeSurface, void* nativeLayer, uint32_t maxFramesInFlight)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Store metalDevice, metalQueue, metalLayer (type: id<MTLDevice>, id<MTLCommandQueue>, id<CAMetalLayer>)
  // 2. Configure metalLayer properties:
  //    - layer.device = metalDevice
  //    - layer.pixelFormat = MTLPixelFormatBGRA8Unorm (or MTLPixelFormatBGRA8Unorm_sRGB)
  //    - layer.framebufferOnly = YES (optimization, prevents CPU access)
  //    - layer.drawableSize = CGSize{width, height}
  // 3. Initialize frame resources (if needed for tracking)
  // 4. Set m_maxFramesInFlight (Metal handles this internally via drawable pool)
  //
  // Example Metal API pattern:
  // CAMetalLayer* layer = (CAMetalLayer*)nativeLayer;
  // layer.device = (id<MTLDevice>)nativeDevice;
  // layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // layer.framebufferOnly = YES;
  (void)nativeDevice;
  (void)nativeQueue;
  (void)nativeSurface;
  (void)nativeLayer;
  (void)maxFramesInFlight;
  assert(false && "Metal implementation not yet available");
}

void MetalSwapchain::deinit()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Clear current drawable (ARC handles release)
  // 2. Clear metalLayer reference (don't destroy, owned by view/window)
  // 3. Clear image resources (ARC handles release)
}

void MetalSwapchain::requestRebuild()
{
  // TODO: Metal implementation
  // NOTES:
  // Set m_needsRebuild = true
  // Triggered on window resize or format change
  // Metal handles resize by updating drawableSize on layer
  m_needsRebuild = true;
}

bool MetalSwapchain::needsRebuild() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return m_needsRebuild
  // Metal automatically handles some resize cases, but may need manual update
  return m_needsRebuild;
}

void MetalSwapchain::rebuild()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Update metalLayer.drawableSize to new window size
  // 2. Clear old drawable references
  // 3. Update m_extent to match new size
  //
  // Example Metal API pattern:
  // CGSize size = CGSizeMake(newWidth, newHeight);
  // metalLayer.drawableSize = size;
  // m_extent.width = newWidth;
  // m_extent.height = newHeight;
}

AcquireResult MetalSwapchain::acquireNextImage()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Call [metalLayer nextDrawable] to get current drawable
  // 2. Store in m_currentDrawable
  // 3. Extract texture from drawable: drawable.texture
  // 4. Create TextureHandle for the texture
  // 5. Return AcquireResult with texture handle and image index
  //
  // Example Metal API pattern:
  // id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
  // if (!drawable) { return AcquireResult{.status = AcquireResult::Status::outOfDate}; }
  // m_currentDrawable = drawable;
  // MTLTexture* texture = drawable.texture;
  // TextureHandle handle = createTextureHandle(texture);
  // return AcquireResult{.texture = handle, .status = AcquireResult::Status::success};
  //
  // NOTES: Metal doesn't have suboptimal/out-of-date states like Vulkan
  // Return success if drawable is valid, outOfDate otherwise
  AcquireResult result;
  result.status = AcquireResult::Status::outOfDate;
  return result;
}

PresentResult MetalSwapchain::present()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Present current drawable using command buffer
  // 2. Call [commandBuffer presentDrawable:m_currentDrawable]
  // 3. Commit command buffer: [commandBuffer commit]
  // 4. Clear m_currentDrawable reference
  //
  // NOTES: Presentation is done from the command buffer, not from swapchain directly
  // This method should be called after encoding completes
  // The command buffer will be obtained from the command list context
  //
  // Example Metal API pattern:
  // [commandBuffer presentDrawable:m_currentDrawable];
  // [commandBuffer commit];
  // m_currentDrawable = nil;
  PresentResult result;
  result.status = PresentResult::Status::success;
  return result;
}

TextureHandle MetalSwapchain::currentTexture() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return texture handle for m_currentDrawable.texture
  // Or 0 if no current drawable
  TextureHandle handle;
  return handle;
}

Extent2D MetalSwapchain::getExtent() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return m_extent (width/height from metalLayer.drawableSize)
  // Or query layer.drawableSize dynamically
  return m_extent;
}

uint32_t MetalSwapchain::getMaxFramesInFlight() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return m_maxFramesInFlight
  // Metal manages drawable pool internally, typically 3 frames
  return m_maxFramesInFlight;
}

uint64_t MetalSwapchain::getNativeSwapchain() const
{
  // TODO: Metal implementation
  // NOTES:
  // Metal doesn't have a swapchain object
  // Return opaque handle to CAMetalLayer or 0
  // Use __bridge_retained for ARC interop
  return 0;
}

uint64_t MetalSwapchain::getNativeImageView(uint32_t imageIndex) const
{
  // TODO: Metal implementation
  // NOTES:
  // Metal textures are directly used, no separate image views
  // Return texture handle or 0
  (void)imageIndex;
  return 0;
}

uint64_t MetalSwapchain::getNativeImage(uint32_t imageIndex) const
{
  // TODO: Metal implementation
  // NOTES:
  // Return opaque handle to MTLTexture at imageIndex
  // Use __bridge_retained for ARC interop
  (void)imageIndex;
  return 0;
}

void* MetalSwapchain::currentDrawable() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return m_currentDrawable (id<CAMetalDrawable>)
  return m_currentDrawable;
}

}  // namespace demo::rhi::metal