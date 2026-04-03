#pragma once

#include "RHITypes.h"

#include <cstdint>

namespace demo {
namespace rhi {

struct WindowHandle
{
  void* nativeWindow{nullptr};
};

struct SurfaceCapabilities
{
  uint32_t minImageCount{0};
  uint32_t maxImageCount{0};
  Extent2D currentExtent{};
  Extent2D minImageExtent{};
  Extent2D maxImageExtent{};
  uint32_t currentTransform{0};
  uint32_t supportedUsageFlags{0};
};

class Surface
{
public:
  virtual ~Surface() = default;

  virtual void                init(void* nativeInstance, void* nativePhysicalDevice, const WindowHandle& window) = 0;
  virtual void                deinit()                                                                           = 0;
  virtual SurfaceCapabilities queryCapabilities() const                                                          = 0;
  virtual uint64_t            getNativeHandle() const                                                            = 0;
};

}  // namespace rhi
}  // namespace demo
