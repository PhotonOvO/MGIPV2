#include "MetalSwapchain.h"

#include "MetalCommon.h"
#include "MetalCommandBuffer.h"
#include "MetalDevice.h"
#include "MetalQueue.h"
#include "MetalResourceTable.h"
#include "MetalSurface.h"

#include <Metal/MTLDevice.hpp>
#include <Metal/MTLResidencySet.hpp>
#include <Metal/MTLTexture.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>
#include <QuartzCore/CAMetalLayer.hpp>

#include <stdexcept>

namespace demo::rhi::metal {

MetalSwapchain::~MetalSwapchain()
{
  deinit();
}

void MetalSwapchain::init(
  MetalDevice& device, MetalQueue& queue, MetalSurface& surface,
  bool vSync, uint32_t maxFramesInFlight)
{
  require(m_device == nullptr, "MetalSwapchain::init called more than once");
  require(surface.layer() != nullptr, "MetalSwapchain requires an initialized surface");
  require(queue.info().capabilities.supportsPresent,
          "MetalSwapchain requires a present-capable Metal 4 queue");
  require(maxFramesInFlight >= 2 && maxFramesInFlight <= 3,
          "MetalSwapchain supports two or three frames in flight");
  m_device = &device;
  m_queue = &queue;
  m_surface = &surface;
  m_layer = surface.layer();
  m_maxFramesInFlight = maxFramesInFlight;
  m_images.resize(maxFramesInFlight);
  setVSync(vSync);
  m_needsRebuild = true;
}

void MetalSwapchain::deinit()
{
  if(m_device == nullptr)
    return;
  if(m_queue != nullptr)
  {
    try { m_queue->waitIdle(); } catch(...) {}
  }
  releaseCurrentDrawable();
  destroyImageResources();
  m_images.clear();
  m_device = nullptr;
  m_queue = nullptr;
  m_surface = nullptr;
  m_layer = nullptr;
  m_extent = {};
  m_frameImageIndex = 0;
  m_hasAcquiredImage = false;
  m_presentEncoded = false;
  m_needsRebuild = true;
}

void MetalSwapchain::setVSync(bool vSync)
{
  m_vSync = vSync;
  if(m_layer != nullptr)
    m_layer->setDisplaySyncEnabled(vSync);
}

const char* MetalSwapchain::getPresentModeName() const
{
  return m_vSync ? "DisplaySync" : "Immediate";
}

void MetalSwapchain::requestRebuild()
{
  m_needsRebuild = true;
}

bool MetalSwapchain::needsRebuild() const
{
  return m_needsRebuild;
}

void MetalSwapchain::rebuild()
{
  require(m_surface != nullptr && m_layer != nullptr,
          "MetalSwapchain::rebuild requires initialization");
  if(m_hasAcquiredImage)
    throw std::runtime_error("MetalSwapchain cannot rebuild with an acquired drawable");
  m_queue->waitIdle();
  destroyImageResources();
  m_surface->updateDrawableSize();
  m_extent = m_surface->drawableExtent();
  m_layer->setMaximumDrawableCount(m_maxFramesInFlight);
  m_layer->setDisplaySyncEnabled(m_vSync);
  m_frameImageIndex = 0;
  m_needsRebuild = m_extent.width == 0 || m_extent.height == 0;
  if(!m_needsRebuild)
  {
    try
    {
      createImageResources();
    }
    catch(...)
    {
      destroyImageResources();
      m_needsRebuild = true;
      throw;
    }
  }
}

AcquireResult MetalSwapchain::acquireNextImage()
{
  if(m_device == nullptr || m_layer == nullptr || m_queue == nullptr)
    throw std::runtime_error("MetalSwapchain::acquireNextImage requires initialization");
  if(m_hasAcquiredImage)
    return AcquireResult{.status = AcquireResult::Status::notReady};
  if(m_needsRebuild || m_extent.width == 0 || m_extent.height == 0)
    return AcquireResult{.status = AcquireResult::Status::outOfDate};

  const uint32_t imageIndex = m_frameImageIndex;
  ImageResource& image = m_images[imageIndex];
  if(!image.texture.isValid() || !image.view.isValid() ||
     image.presentResidencySet == nullptr)
  {
    throw std::runtime_error("MetalSwapchain has no presentation composition image");
  }
  retireImagePresentation(image);

  CA::MetalDrawable* drawable = m_layer->nextDrawable();
  if(drawable == nullptr)
    return AcquireResult{.status = AcquireResult::Status::notReady};
  drawable->retain();

  MTL::Texture* texture = drawable->texture();
  if(texture == nullptr || texture->width() != m_extent.width ||
     texture->height() != m_extent.height ||
     texture->pixelFormat() != MTL::PixelFormatBGRA8Unorm)
  {
    drawable->release();
    m_needsRebuild = true;
    return AcquireResult{.status = AcquireResult::Status::outOfDate};
  }

  try
  {
    m_queue->waitDrawable(reinterpret_cast<MTL::Drawable*>(drawable));
  }
  catch(...)
  {
    drawable->release();
    throw;
  }

  m_currentDrawable = drawable;
  m_hasAcquiredImage = true;
  m_presentEncoded = false;
  return AcquireResult{
    .texture = image.texture,
    .imageIndex = imageIndex,
    .status = AcquireResult::Status::success,
  };
}

void MetalSwapchain::encodePresent(CommandBuffer& commandBuffer)
{
  if(!m_hasAcquiredImage || m_currentDrawable == nullptr)
    return;
  auto* metalCommandBuffer = dynamic_cast<MetalCommandBuffer*>(&commandBuffer);
  if(metalCommandBuffer == nullptr)
    throw std::invalid_argument("MetalSwapchain received a foreign command buffer");

  ImageResource& image = m_images[m_frameImageIndex];
  MTL::Texture* drawableTexture = m_currentDrawable->texture();
  MTL::ResidencySet* drawableResidencySet = m_layer->residencySet();
  require(drawableTexture != nullptr && image.presentResidencySet != nullptr &&
            drawableResidencySet != nullptr,
          "MetalSwapchain requires live presentation textures and residency sets");
  metalCommandBuffer->encodeDrawablePresent(
    image.texture, drawableTexture, image.presentResidencySet,
    drawableResidencySet,
    reinterpret_cast<MTL::Drawable*>(m_currentDrawable));
  m_presentEncoded = true;
}

PresentResult MetalSwapchain::present()
{
  if(!m_hasAcquiredImage || m_currentDrawable == nullptr)
    return PresentResult{.status = PresentResult::Status::outOfDate};
  if(!m_presentEncoded)
    throw std::runtime_error("MetalSwapchain::present requires an encoded drawable copy");

  ImageResource& image = m_images[m_frameImageIndex];
  image.presentSubmission = m_queue->lastSubmittedToken();
  require(image.presentSubmission.isValid(),
          "MetalSwapchain::present requires a submitted frame");
  reinterpret_cast<MTL::Drawable*>(m_currentDrawable)->present();
  image.pendingDrawable = m_currentDrawable;
  m_currentDrawable = nullptr;
  m_hasAcquiredImage = false;
  m_presentEncoded = false;
  m_frameImageIndex = (m_frameImageIndex + 1u) % m_maxFramesInFlight;
  return PresentResult{.status = PresentResult::Status::success};
}

TextureHandle MetalSwapchain::currentTexture() const
{
  if(!m_hasAcquiredImage || m_frameImageIndex >= m_images.size())
    return {};
  return m_images[m_frameImageIndex].texture;
}

TextureViewHandle MetalSwapchain::textureView(uint32_t imageIndex) const
{
  return imageIndex < m_images.size() ? m_images[imageIndex].view
                                      : TextureViewHandle{};
}

Extent2D MetalSwapchain::getExtent() const
{
  return m_extent;
}

uint32_t MetalSwapchain::getMaxFramesInFlight() const
{
  return m_maxFramesInFlight;
}

uint32_t MetalSwapchain::getRequestedImageCount() const
{
  return m_maxFramesInFlight;
}

TextureFormat MetalSwapchain::getFormat() const
{
  return TextureFormat::bgra8Unorm;
}

void MetalSwapchain::createImageResources()
{
  const TextureDesc textureDesc{
    .dimension = TextureDimension::e2D,
    .format = TextureFormat::bgra8Unorm,
    .usage = TextureUsageFlags::colorAttachment |
             TextureUsageFlags::transferSrc |
             TextureUsageFlags::transferDst,
    .extent = {m_extent.width, m_extent.height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 present composition",
  };
  for(ImageResource& image : m_images)
  {
    image.texture = m_device->createTexture(textureDesc);
    image.view = m_device->createTextureView(TextureViewCreateDesc{
      .image = image.texture,
      .format = TextureFormat::bgra8Unorm,
      .viewType = ImageViewType::e2D,
      .aspect = TextureAspect::color,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
      .debugName = "Metal4 present composition view",
    });
    const MetalTextureRecord* composition =
      m_device->resourceTable().tryGetTexture(image.texture);
    require(composition != nullptr && composition->nativeTexture != nullptr,
            "MetalSwapchain failed to resolve presentation composition texture");
    auto* residencyDesc = MTL::ResidencySetDescriptor::alloc()->init();
    residencyDesc->setInitialCapacity(1);
    residencyDesc->setLabel(nsString("Metal4 presentation residency"));
    NS::Error* residencyError = nullptr;
    image.presentResidencySet =
      m_device->nativeDevice()->newResidencySet(residencyDesc, &residencyError);
    residencyDesc->release();
    if(image.presentResidencySet == nullptr)
      throw std::runtime_error(errorMessage(
        "MetalSwapchain presentation residency set", residencyError));
    image.presentResidencySet->addAllocation(composition->nativeTexture);
    image.presentResidencySet->commit();
  }
}

void MetalSwapchain::destroyImageResources()
{
  if(m_device == nullptr)
    return;
  for(ImageResource& image : m_images)
  {
    if(image.pendingDrawable != nullptr)
      image.pendingDrawable->release();
    if(image.presentResidencySet != nullptr)
    {
      image.presentResidencySet->endResidency();
      image.presentResidencySet->release();
    }
    if(image.view.isValid())
      m_device->destroyTextureView(image.view);
    if(image.texture.isValid())
      m_device->destroyTexture(image.texture);
    image = {};
  }
}

void MetalSwapchain::retireImagePresentation(ImageResource& image)
{
  if(image.presentSubmission.isValid())
    m_queue->wait(image.presentSubmission);
  if(image.pendingDrawable != nullptr)
  {
    image.pendingDrawable->release();
    image.pendingDrawable = nullptr;
  }
  image.presentSubmission = {};
}

void MetalSwapchain::releaseCurrentDrawable() noexcept
{
  if(m_currentDrawable != nullptr)
    m_currentDrawable->release();
  m_currentDrawable = nullptr;
  m_presentEncoded = false;
}

}  // namespace demo::rhi::metal
