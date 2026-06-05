#pragma once

#include "RHIArgumentTable.h"
#include "RHICapabilities.h"
#include "RHIHandles.h"
#include "RHIQueue.h"
#include "RHITypes.h"

#include <cassert>
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

  // ----- Modern GPU interface (Wave 0 contract) ----------------------------
  // Default bodies assert: backends opt in by overriding. Vulkan implements
  // these in Wave 1; D3D12/Metal stay asserting stubs until later milestones.
  // destroy* must route through FrameContext's deferred-destruction queue.

  // --- Buffer (wraps the existing device-address path) ---
  virtual BufferHandle createBuffer(const BufferDesc&) { assert(false && "createBuffer not implemented"); return {}; }
  virtual void         destroyBuffer(BufferHandle) { assert(false && "destroyBuffer not implemented"); }
  virtual GpuPtr       getBufferGpuAddress(BufferHandle) const { assert(false && "getBufferGpuAddress not implemented"); return {}; }
  virtual void*        mapBuffer(BufferHandle) { assert(false && "mapBuffer not implemented"); return nullptr; }
  virtual void         unmapBuffer(BufferHandle) { assert(false && "unmapBuffer not implemented"); }

  // --- Sampler ---
  virtual SamplerHandle createSampler(const SamplerDesc&) { assert(false && "createSampler not implemented"); return {}; }
  virtual void          destroySampler(SamplerHandle) { assert(false && "destroySampler not implemented"); }

  // --- Argument layout / table ---
  virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc&) { assert(false && "createArgumentLayout not implemented"); return {}; }
  virtual void                 destroyArgumentLayout(ArgumentLayoutHandle) { assert(false && "destroyArgumentLayout not implemented"); }
  virtual ArgumentTableHandle  createArgumentTable(ArgumentLayoutHandle) { assert(false && "createArgumentTable not implemented"); return {}; }
  virtual void                 destroyArgumentTable(ArgumentTableHandle) { assert(false && "destroyArgumentTable not implemented"); }
  virtual void                 updateArgumentTable(ArgumentTableHandle, uint32_t /*writeCount*/, const ArgumentWrite*) { assert(false && "updateArgumentTable not implemented"); }

  // --- Query pool ---
  virtual QueryPoolHandle createQueryPool(uint32_t /*queryCount*/) { assert(false && "createQueryPool not implemented"); return {}; }
  virtual void            destroyQueryPool(QueryPoolHandle) { assert(false && "destroyQueryPool not implemented"); }
  virtual uint64_t        getQueryPoolResult(QueryPoolHandle, uint32_t /*queryIndex*/) { assert(false && "getQueryPoolResult not implemented"); return 0; }
  // Non-blocking batch read. Writes queryCount (value, availability) pairs into outPairs
  // (size >= queryCount*2). availability==0 means the result is not yet ready. Returns
  // false if the whole batch could not be read.
  virtual bool            getQueryPoolResultsWithAvailability(QueryPoolHandle, uint32_t /*firstQuery*/, uint32_t /*queryCount*/, uint64_t* /*outPairs*/) { assert(false && "getQueryPoolResultsWithAvailability not implemented"); return false; }
};

}  // namespace demo::rhi
