#pragma once

#include "RHIArgumentTable.h"
#include "RHIBindlessTypes.h"
#include "RHICapabilities.h"
#include "RHIHandles.h"
#include "RHIPipeline.h"
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

enum class DescriptorHeapType : uint8_t
{
  resource = 0,
  sampler,
};

struct DescriptorHeapDesc
{
  DescriptorHeapType type{DescriptorHeapType::resource};
  uint32_t           descriptorCapacity{0};
  bool               shaderVisible{true};
};

struct DescriptorAllocation
{
  DescriptorHeapHandle heap{};
  ResourceIndex        firstIndex{kInvalidResourceIndex};
  uint32_t             count{0};

  [[nodiscard]] bool isValid() const { return heap.isValid() && firstIndex != kInvalidResourceIndex && count > 0; }
};

class Device
{
public:
  virtual ~Device() = default;

  virtual void init(const DeviceCreateInfo& createInfo) = 0;
  virtual void deinit()                                 = 0;

  virtual uint64_t getBackendInstanceHandle() const       = 0;
  virtual uint64_t getBackendPhysicalDeviceHandle() const = 0;
  virtual uint64_t getBackendDeviceHandle() const         = 0;

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
  // createTextureView builds a backend view from the desc and registers an owned handle.
  // registerExternalTextureView adopts an externally-owned backend view (e.g. swapchain)
  // without taking ownership. destroyTextureView frees owned views. resolveTextureViewBackendHandle
  // returns the backing native handle (as uint64) for descriptor-write / ImGui seams.
  virtual TextureViewHandle createTextureView(const TextureViewCreateDesc& desc)     = 0;
  virtual TextureViewHandle registerExternalTextureView(uint64_t externalView)         = 0;
  virtual void              destroyTextureView(TextureViewHandle handle)             = 0;
  virtual uint64_t          resolveTextureViewBackendHandle(TextureViewHandle handle) const = 0;

  // --- Textures (images) ---
  // createTexture creates an RHI-owned texture. registerExternalTexture adopts an
  // externally-owned backend texture (e.g. swapchain) without taking ownership.
  // destroyTexture frees owned images; destroyImage is kept as the legacy alias
  // while renderer call sites migrate.
  virtual TextureHandle createTexture(const TextureDesc&) { assert(false && "createTexture not implemented"); return {}; }
  virtual void          destroyTexture(TextureHandle handle) { destroyImage(handle); }
  // registerExternalTexture adopts an externally-owned backend texture (e.g. swapchain)
  // without taking ownership. destroyImage frees owned images. resolveTextureBackendHandle
  // returns the backing backend object handle (as uint64) for command/seam paths.
  virtual TextureHandle registerExternalTexture(uint64_t externalImage)           = 0;
  virtual void          destroyImage(TextureHandle handle)                      = 0;
  virtual uint64_t      resolveTextureBackendHandle(TextureHandle handle) const          = 0;

  // ----- Modern GPU interface (Wave 0 contract) ----------------------------
  // Default bodies assert: backends opt in by overriding. Vulkan implements
  // these in Wave 1; D3D12/Metal stay asserting stubs until later milestones.
  // destroy* invalidates the logical handle immediately. For owned resources,
  // physical backend destruction is delayed until the backend retirement point;
  // adopted/external resources are only unregistered from the handle table.

  // --- Buffer (wraps the existing device-address path) ---
  virtual BufferHandle createBuffer(const BufferDesc&) { assert(false && "createBuffer not implemented"); return {}; }
  virtual void         destroyBuffer(BufferHandle) { assert(false && "destroyBuffer not implemented"); }
  // Adopt an externally-owned backend buffer (owned=false): the registry only mirrors it so
  // it can be addressed by handle. updateBufferBinding rebinds the handle to a reallocated
  // backend buffer. destroyBuffer on an owned=false handle only unregisters.
  virtual BufferHandle registerExternalBuffer(uint64_t /*externalBuffer*/) { assert(false && "registerExternalBuffer not implemented"); return {}; }
  virtual void         updateBufferBinding(BufferHandle, uint64_t /*externalBuffer*/) { assert(false && "updateBufferBinding not implemented"); }
  virtual GpuPtr       getBufferGpuAddress(BufferHandle) const { assert(false && "getBufferGpuAddress not implemented"); return {}; }
  virtual void*        mapBuffer(BufferHandle) { assert(false && "mapBuffer not implemented"); return nullptr; }
  virtual void         unmapBuffer(BufferHandle) { assert(false && "unmapBuffer not implemented"); }

  // --- Sampler ---
  virtual SamplerHandle createSampler(const SamplerDesc&) { assert(false && "createSampler not implemented"); return {}; }
  virtual void          destroySampler(SamplerHandle) { assert(false && "destroySampler not implemented"); }
  virtual uint64_t      resolveSamplerBackendHandle(SamplerHandle) const { assert(false && "resolveSamplerBackendHandle not implemented"); return 0; }

  // --- Argument layout / table ---
  virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc&) { assert(false && "createArgumentLayout not implemented"); return {}; }
  virtual void                 destroyArgumentLayout(ArgumentLayoutHandle) { assert(false && "destroyArgumentLayout not implemented"); }
  virtual ArgumentTableHandle  createArgumentTable(ArgumentLayoutHandle) { assert(false && "createArgumentTable not implemented"); return {}; }
  virtual void                 destroyArgumentTable(ArgumentTableHandle) { assert(false && "destroyArgumentTable not implemented"); }
  virtual void                 updateArgumentTable(ArgumentTableHandle, uint32_t /*writeCount*/, const ArgumentWrite*) { assert(false && "updateArgumentTable not implemented"); }

  // --- Pipeline ---
  virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) { assert(false && "createGraphicsPipeline not implemented"); return {}; }
  virtual PipelineHandle createComputePipeline(const ComputePipelineDesc&) { assert(false && "createComputePipeline not implemented"); return {}; }
  virtual void           destroyPipeline(PipelineHandle) { assert(false && "destroyPipeline not implemented"); }

  // --- Query pool ---
  virtual QueryPoolHandle createQueryPool(uint32_t /*queryCount*/) { assert(false && "createQueryPool not implemented"); return {}; }
  virtual void            destroyQueryPool(QueryPoolHandle) { assert(false && "destroyQueryPool not implemented"); }
  virtual uint64_t        getQueryPoolResult(QueryPoolHandle, uint32_t /*queryIndex*/) { assert(false && "getQueryPoolResult not implemented"); return 0; }
  // Non-blocking batch read. Writes queryCount (value, availability) pairs into outPairs
  // (size >= queryCount*2). availability==0 means the result is not yet ready. Returns
  // false if the whole batch could not be read.
  virtual bool            getQueryPoolResultsWithAvailability(QueryPoolHandle, uint32_t /*firstQuery*/, uint32_t /*queryCount*/, uint64_t* /*outPairs*/) { assert(false && "getQueryPoolResultsWithAvailability not implemented"); return false; }

  // --- Future RHI features (capability-gated) ---
  virtual DescriptorHeapHandle allocateDescriptorHeap(const DescriptorHeapDesc&) { assert(false && "allocateDescriptorHeap not implemented"); return {}; }
  virtual void                 freeDescriptorHeap(DescriptorHeapHandle) { assert(false && "freeDescriptorHeap not implemented"); }
  virtual DescriptorAllocation allocateDescriptors(DescriptorHeapHandle, uint32_t /*count*/) { assert(false && "allocateDescriptors not implemented"); return {}; }
  virtual void                 freeDescriptors(const DescriptorAllocation&) { assert(false && "freeDescriptors not implemented"); }

  virtual ResidencySetHandle createResidencySet() { assert(false && "createResidencySet not implemented"); return {}; }
  virtual void               destroyResidencySet(ResidencySetHandle) { assert(false && "destroyResidencySet not implemented"); }

  virtual ShaderLibraryHandle createShaderLibrary(const ShaderLibraryDesc&) { assert(false && "createShaderLibrary not implemented"); return {}; }
  virtual void                destroyShaderLibrary(ShaderLibraryHandle) { assert(false && "destroyShaderLibrary not implemented"); }
  virtual PipelineCompilerHandle createPipelineCompiler(const PipelineCompileOptions&) { assert(false && "createPipelineCompiler not implemented"); return {}; }
  virtual void                   destroyPipelineCompiler(PipelineCompilerHandle) { assert(false && "destroyPipelineCompiler not implemented"); }
};

}  // namespace demo::rhi
