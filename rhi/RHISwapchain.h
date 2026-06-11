#pragma once

#include "RHIHandles.h"
#include "RHITypes.h"

namespace demo::rhi {

struct AcquireResult
{
  enum class Status
  {
    success,
    suboptimal,
    outOfDate,
    notReady,  // Image not available yet from a non-blocking acquire.
  };

  TextureHandle texture{};
  uint32_t      imageIndex{0};
  Status        status{Status::success};
};

struct PresentResult
{
  enum class Status
  {
    success,
    suboptimal,
    outOfDate,
  };

  Status status{Status::success};
};

class Swapchain
{
public:
  virtual ~Swapchain() = default;

  virtual AcquireResult acquireNextImage()           = 0;
  virtual PresentResult present()                    = 0;
  // Releases backend swapchain resources; called by the render layer at shutdown.
  virtual void          deinit() {}
  // Present-control knobs; backends without support inherit the no-op defaults.
  virtual void          setVSync(bool /*vSync*/) {}
  virtual void          setFullscreen(bool /*enabled*/, void* /*platformHandle*/) {}
  // Human-readable negotiated present mode (debug/UI display only).
  virtual const char*   getPresentModeName() const { return "Unknown"; }
  virtual void          requestRebuild()             = 0;
  virtual bool          needsRebuild() const         = 0;
  virtual void          rebuild()                    = 0;
  virtual TextureHandle currentTexture() const       = 0;
  virtual Extent2D      getExtent() const            = 0;
  virtual uint32_t      getMaxFramesInFlight() const = 0;
  virtual uint32_t      getRequestedImageCount() const = 0;
  // Returns the negotiated swapchain format as a portable TextureFormat.
  // D3D12/Metal stubs inherit this default and return undefined until the backend overrides.
  virtual TextureFormat getFormat() const { return TextureFormat::undefined; }
  // Native swapchain handles are backend-internal. Cast to VulkanSwapchain for
  // nativeSwapchain() / nativeImage() / nativeImageView() typed accessors.
};

}  // namespace demo::rhi
