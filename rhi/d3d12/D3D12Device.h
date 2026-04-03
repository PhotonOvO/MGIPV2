#pragma once

#include "../RHIDevice.h"

#include <vector>

namespace demo::rhi::d3d12 {

class D3D12Device final : public demo::rhi::Device
{
public:
  D3D12Device() = default;
  ~D3D12Device() override;

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

  // D3D12-native accessor (for backend interop)
  // NOTES: Returns opaque handle to ID3D12Device
  void* getD3D12Device() const { return m_d3d12Device; }

private:
  struct NativeQueueInfo
  {
    void*     commandQueue{nullptr};  // ID3D12CommandQueue
    uint32_t  nodeMask{0};
    uint32_t  queueIndex{0};
    QueueInfo toRhi() const;
  };

  // NOTES: D3D12 device initialization strategy
  // 1. Create DXGIFactory using CreateDXGIFactory2()
  // 2. Enumerate adapters using EnumAdapters1()
  // 3. Select adapter based on feature level and capabilities
  // 4. Create device using D3D12CreateDevice()
  // 5. Query D3D12_FEATURE_DATA_* structures for capabilities
  // 6. Map D3D12 features to RHI CapabilityTier:
  //    - Tier 3 (Full): D3D_FEATURE_LEVEL_12_2+, Shader Model 6.7+
  //    - Tier 2 (Enhanced): D3D_FEATURE_LEVEL_12_1+, Shader Model 6.6+
  //    - Tier 1 (Basic): D3D_FEATURE_LEVEL_12_0+, Shader Model 6.5+
  // 7. Create descriptor heaps for bindless:
  //    - CBV_SRV_UAV heap (shader-visible)
  //    - Sampler heap (shader-visible)
  void initD3D12Device();
  void selectD3D12Adapter();
  void initD3D12Queues();
  void initDescriptorHeaps();
  void detectD3D12Capabilities();
  void validateD3D12Capabilities();

  // NOTES: D3D12 Feature → RHI Capability Mapping
  // Timeline Semaphore: ID3D12Fence (SetEventOnCompletion, GetCompletedValue)
  // Synchronization2: D3D12_BARRIER_GROUP (Win11 22H2+) or manual barriers
  // Dynamic Rendering: Native (no render passes like Vulkan)
  // Bindless: Descriptor heaps with root signatures (GPU descriptor indexing)
  void mapD3D12FeaturesToCapabilities();

  DeviceCreateInfo m_createInfo{};

  void* m_dxgiFactory{nullptr};  // IDXGIFactory4
  void* m_dxgiAdapter{nullptr};  // IDXGIAdapter4
  void* m_d3d12Device{nullptr};  // ID3D12Device

  NativeQueueInfo m_graphicsQueue{};
  NativeQueueInfo m_computeQueue{};
  NativeQueueInfo m_transferQueue{};

  uint32_t           m_apiVersion{0};
  PhysicalDeviceInfo m_physicalDeviceInfo{};
  DeviceFeatureInfo  m_featureInfo{};
  CapabilityReport   m_capabilities{};
  MemoryProperties   m_memoryProperties{};

  // NOTES: D3D12-specific capability storage
  // Feature Level: D3D_FEATURE_LEVEL_* (determines API support)
  // Shader Model: D3D_SHADER_MODEL_* (determines shader capabilities)
  // Used to determine supported features and performance tier
  uint32_t m_d3dFeatureLevel{0};
  uint32_t m_shaderModel{0};
  bool     m_supportsMeshShaders{false};
  bool     m_supportsRayTracing{false};
  bool     m_supportsWorkGraphs{false};
  bool     m_supportsGpuUploadHeaps{false};

  // NOTES: D3D12 Descriptor Heaps for Bindless
  // These heaps are created during init and used for bindless resource access
  // - CBV_SRV_UAV heap: For constant buffers, shader resource views, unordered access views
  // - Sampler heap: For samplers
  // - Shader-visible: Can be bound directly in shaders via root signature
  void*    m_cbvSrvUavHeap{nullptr};  // ID3D12DescriptorHeap
  void*    m_samplerHeap{nullptr};    // ID3D12DescriptorHeap
  uint32_t m_descriptorSize{0};       // D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV size

  bool m_initialized{false};
};

}  // namespace demo::rhi::d3d12