#pragma once

#include "RHICapabilities.h"
#include "RHIHandles.h"
#include "RHIQueue.h"
#include "RHITypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace demo::rhi {

struct ExtensionRequest
{
  const char* name{nullptr};
  bool        required{false};
  void*       featuresStruct{nullptr};
};

struct DeviceCreateInfo
{
  std::vector<ExtensionRequest> deviceExtensions;
  std::vector<const char*>      instanceExtensions;
  std::vector<const char*>      instanceLayers;
  CapabilityRequirements        capabilityRequirements{};
  bool                          enableValidationLayers{true};
};

struct PhysicalDeviceInfo
{
  std::string deviceName;
  uint32_t    apiVersion{0};
  uint32_t    driverVersion{0};
  uint32_t    vendorId{0};
  uint32_t    deviceId{0};
  uint32_t    deviceType{0};
};

struct DeviceFeatureInfo
{
  bool timelineSemaphore{false};
  bool synchronization2{false};
  bool dynamicRendering{false};
  bool maintenance5{false};
  bool maintenance6{false};
};

struct MemoryTypeInfo
{
  uint32_t propertyFlags{0};
  uint32_t heapIndex{0};
};

struct MemoryHeapInfo
{
  uint64_t size{0};
  uint32_t flags{0};
};

struct MemoryProperties
{
  std::vector<MemoryTypeInfo> memoryTypes;
  std::vector<MemoryHeapInfo> memoryHeaps;
};

class Device
{
public:
  virtual ~Device() = default;

  virtual void init(const DeviceCreateInfo& createInfo) = 0;
  virtual void deinit()                                 = 0;

  virtual uint64_t getNativeInstance() const       = 0;
  virtual uint64_t getNativePhysicalDevice() const = 0;
  virtual uint64_t getNativeDevice() const         = 0;

  virtual uint32_t                  getApiVersion() const               = 0;
  virtual const char*               getDeviceName() const               = 0;
  virtual const PhysicalDeviceInfo& getPhysicalDeviceInfo() const       = 0;
  virtual const DeviceFeatureInfo&  getEnabledFeatureInfo() const       = 0;
  virtual CapabilityReport          queryCapabilities() const           = 0;
  virtual bool                      supports(CapabilityTier tier) const = 0;
  virtual const MemoryProperties&   getPhysicalMemoryProperties() const = 0;
  virtual void*                     getFeaturesChainHead() const        = 0;

  virtual QueueInfo getGraphicsQueue() const = 0;
  virtual QueueInfo getComputeQueue() const  = 0;
  virtual QueueInfo getTransferQueue() const = 0;

  virtual bool isInstanceExtensionSupported(const char* name) const = 0;
  virtual bool isDeviceExtensionSupported(const char* name) const   = 0;

  virtual void waitIdle() = 0;

  // --- Texture views ---
  // createTextureView builds a native view from the desc and registers an owned handle.
  // registerExternalTextureView adopts an externally-owned native view (e.g. swapchain)
  // without taking ownership. destroyTextureView frees owned views. resolveTextureViewNative
  // returns the backing native handle (as uint64) for descriptor-write / ImGui seams.
  virtual TextureViewHandle createTextureView(const TextureViewCreateDesc& desc)     = 0;
  virtual TextureViewHandle registerExternalTextureView(uint64_t nativeView)         = 0;
  virtual void              destroyTextureView(TextureViewHandle handle)             = 0;
  virtual uint64_t          resolveTextureViewNative(TextureViewHandle handle) const = 0;

  // --- Textures (images) ---
  // createImage builds a native image (vmaCreateImage) from the desc and registers an
  // owned handle. registerExternalTexture adopts an externally-owned native image (e.g.
  // swapchain) without taking ownership. destroyImage frees owned images. resolveImageNative
  // returns the backing native VkImage (as uint64) for command/seam paths.
  virtual TextureHandle createImage(const TextureCreateDesc& desc)              = 0;
  virtual TextureHandle registerExternalTexture(uint64_t nativeImage)           = 0;
  virtual void          destroyImage(TextureHandle handle)                      = 0;
  virtual uint64_t      resolveImageNative(TextureHandle handle) const          = 0;
};

}  // namespace demo::rhi
