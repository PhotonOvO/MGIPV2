#pragma once

#include "../RHIDevice.h"

#include <vector>

namespace demo::rhi::metal {

class MetalDevice final : public demo::rhi::Device
{
public:
  MetalDevice() = default;
  ~MetalDevice() override;

  void init(const DeviceCreateInfo& createInfo) override;
  void deinit() override;

  uint64_t getNativeInstance() const override;
  uint64_t getNativePhysicalDevice() const override;
  uint64_t getNativeDevice() const override;

  uint32_t                  getApiVersion() const override;
  const char*               getDeviceName() const override;
  const PhysicalDeviceInfo& getPhysicalDeviceInfo() const override;
  const DeviceFeatureInfo&  getEnabledFeatureInfo() const override;
  CapabilityReport          queryCapabilities() const override;
  bool                      supports(CapabilityTier tier) const override;
  const MemoryProperties&   getPhysicalMemoryProperties() const override;
  void*                     getFeaturesChainHead() const override;

  QueueInfo getGraphicsQueue() const override;
  QueueInfo getComputeQueue() const override;
  QueueInfo getTransferQueue() const override;

  bool isInstanceExtensionSupported(const char* name) const override;
  bool isDeviceExtensionSupported(const char* name) const override;

  void waitIdle() override;

  // Metal-native accessor (for backend interop)
  // NOTES: Returns opaque handle to MTLDevice
  // Implementation should cast from id<MTLDevice> to uint64_t
  void* getMetalDevice() const { return m_metalDevice; }

private:
  struct NativeQueueInfo
  {
    void*     queue{nullptr};  // id<MTLCommandQueue>
    uint32_t  familyIndex{~0u};
    uint32_t  queueIndex{0};
    uint32_t  queueCount{0};
    QueueInfo toRhi() const;
  };

  // NOTES: Metal device initialization strategy
  // 1. Create device using MTLCreateSystemDefaultDevice()
  // 2. Query Metal feature set (MTLFeatureSet) to determine capabilities
  // 3. Map Metal Feature Set to RHI CapabilityTier:
  //    - Tier 3 (Full): Apple GPU on macOS 14+, Metal 3+
  //    - Tier 2 (Enhanced): Apple GPU on macOS 13+, iOS 17+
  //    - Tier 1 (Basic): Other Apple GPUs or older OS
  // 4. Feature detection:
  //    - supportsFamily(MTLGPUFamilyApple3+) for tier 3
  //    - Argument buffers: supportsArgumentBuffers
  //    - Dynamic libraries: supportsDynamicLibraries
  //    - Indirect drawing: supportsIndirectTessellationAndGeometry
  void initMetalDevice();
  void selectMetalQueue();
  void detectMetalCapabilities();
  void validateMetalCapabilities();

  // NOTES: Metal Feature Set → RHI Capability Mapping
  // Timeline Semaphore: Not natively supported
  //   - Use MTLCommandBuffer completion handlers instead
  //   - Track completion with MTLSharedEvent (when available)
  // Synchronization2: N/A (Metal uses implicit synchronization)
  // Dynamic Rendering: Native (MTLRenderPassDescriptor)
  // Bindless: Argument buffers (MTLArgumentEncoder)
  void mapMetalFeatureSetToCapabilities();

  DeviceCreateInfo m_createInfo{};

  void* m_metalDevice{nullptr};  // id<MTLDevice>

  NativeQueueInfo m_graphicsQueue{};
  NativeQueueInfo m_computeQueue{};
  NativeQueueInfo m_transferQueue{};  // May share with graphics

  uint32_t           m_apiVersion{0};
  PhysicalDeviceInfo m_physicalDeviceInfo{};
  DeviceFeatureInfo  m_featureInfo{};
  CapabilityReport   m_capabilities{};
  MemoryProperties   m_memoryProperties{};

  // NOTES: Metal-specific capability storage
  // Metal Feature Set: MTLFeatureSet or MTLGPUFamily
  // Used to determine supported features and performance tier
  uint32_t m_metalFeatureSet{0};
  bool     m_supportsArgumentBuffers{false};
  bool     m_supportsDynamicLibraries{false};
  bool     m_supportsIndirectDrawing{false};
  bool     m_supportsMeshShaders{false};  // mesh shaders not in Metal

  bool m_initialized{false};
};

}  // namespace demo::rhi::metal