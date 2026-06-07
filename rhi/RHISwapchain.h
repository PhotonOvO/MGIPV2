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
  virtual void          requestRebuild()             = 0;
  virtual bool          needsRebuild() const         = 0;
  virtual void          rebuild()                    = 0;
  virtual TextureHandle currentTexture() const       = 0;
  virtual Extent2D      getExtent() const            = 0;
  virtual uint32_t      getMaxFramesInFlight() const = 0;
  virtual uint32_t      getRequestedImageCount() const = 0;

  virtual uint64_t getBackendSwapchainHandle() const { return 0; }
  virtual uint64_t getBackendImageViewHandle(uint32_t) const { return 0; }
  virtual uint64_t getBackendImageHandle(uint32_t) const { return 0; }
};

}  // namespace demo::rhi
