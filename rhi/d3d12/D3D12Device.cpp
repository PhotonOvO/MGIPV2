#include "D3D12Device.h"
#include "../RHIBoundary.h"

namespace demo::rhi::d3d12 {

D3D12Device::~D3D12Device()
{
  deinit();
}

void D3D12Device::init(const DeviceCreateInfo& createInfo)
{
  (void)createInfo;
  RHI_UNIMPLEMENTED("D3D12Device::init");
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Create DXGIFactory using CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG if validation enabled)
  // 2. Enumerate adapters using IDXGIFactory4::EnumAdapters1()
  // 3. Select adapter based on feature level (prefer D3D_FEATURE_LEVEL_12_2+)
  // 4. Create device using D3D12CreateDevice(adapter, featureLevel, IID_PPV_ARGS(&device))
  // 5. Enable debug layer if createInfo.enableValidationLayers
  // 6. Create command queues for graphics, compute, transfer
  // 7. Create descriptor heaps for bindless:
  //    - CBV_SRV_UAV heap (shader-visible, large size for bindless)
  //    - Sampler heap (shader-visible, large size for bindless)
  // 8. Query D3D12_FEATURE_DATA_* for capabilities
  // 9. Map D3D12 features to RHI CapabilityTier
}

void D3D12Device::deinit()
{
  // deinit must be safe to call (RAII cleanup never aborts). Since init() aborts via
  // RHI_UNIMPLEMENTED, m_initialized can never be true here — so this is a clean no-op.
  if (!m_initialized)
    return;
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Wait for GPU idle using ID3D12Device::Signal + ID3D12Fence::Wait
  // 2. Release command queues
  // 3. Release descriptor heaps
  // 4. Release device (Release())
  // 5. Release adapter and factory
}

uint32_t D3D12Device::getApiVersion() const
{
  // TODO: D3D12 implementation
  // NOTES: Return D3D_FEATURE_LEVEL as version
  // D3D_FEATURE_LEVEL_12_2 = 0xc200 (mapped to API version)
  return m_apiVersion;
}

const char* D3D12Device::getDeviceName() const
{
  // TODO: D3D12 implementation
  // NOTES: Query DXGI_ADAPTER_DESC1.Description
  return m_physicalDeviceInfo.deviceName.c_str();
}

const PhysicalDeviceInfo& D3D12Device::getPhysicalDeviceInfo() const
{
  // TODO: D3D12 implementation
  // NOTES: Query DXGI_ADAPTER_DESC1 for device info
  return m_physicalDeviceInfo;
}

const DeviceFeatureInfo& D3D12Device::getEnabledFeatureInfo() const
{
  // TODO: D3D12 implementation
  // NOTES: Return queried D3D12 features
  return m_featureInfo;
}

CapabilityReport D3D12Device::queryCapabilities() const
{
  // TODO: D3D12 implementation
  // NOTES: Query D3D12_FEATURE_DATA_* structures:
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS (basic features)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS1 (tier 1 resource binding)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS2 (wave ops)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS3 (copy queue timestamps)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS4 (native 16-bit)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS5 (raytracing tier 1.1)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS6 (variable rate shading)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS7 (mesh shaders)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS8 (shaders)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS9 (mesh shader amplification)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS10 (mesh shaders)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS11 (work graphs)
  // - D3D12_FEATURE_DATA_D3D12_OPTIONS12 (resource binding tier 4)
  return m_capabilities;
}

bool D3D12Device::supports(CapabilityTier tier) const
{
  // TODO: D3D12 implementation
  // NOTES: Map D3D12 features to tiers:
  // - Tier 3: D3D_FEATURE_LEVEL_12_2 + Shader Model 6.7 + mesh shaders + work graphs
  // - Tier 2: D3D_FEATURE_LEVEL_12_1 + Shader Model 6.6 + ray tracing tier 1.1
  // - Tier 1: D3D_FEATURE_LEVEL_12_0 + Shader Model 6.5 + basic features
  return false;
}

const MemoryProperties& D3D12Device::getPhysicalMemoryProperties() const
{
  // TODO: D3D12 implementation
  // NOTES: Query IDXGIAdapter3::QueryVideoMemoryInfo for heap info
  // D3D12 has unified memory model, but can query VRAM vs system RAM
  return m_memoryProperties;
}

QueueInfo D3D12Device::getGraphicsQueue() const
{
  // TODO: D3D12 implementation
  // NOTES: Return queue info from m_graphicsQueue
  return m_graphicsQueue.toRhi();
}

QueueInfo D3D12Device::getComputeQueue() const
{
  // TODO: D3D12 implementation
  // NOTES: Return queue info from m_computeQueue
  return m_computeQueue.toRhi();
}

QueueInfo D3D12Device::getTransferQueue() const
{
  // TODO: D3D12 implementation
  // NOTES: Return queue info from m_transferQueue
  return m_transferQueue.toRhi();
}

void D3D12Device::waitIdle()
{
  RHI_UNIMPLEMENTED("D3D12Device::waitIdle");
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Create ID3D12Fence with initial value 0
  // 2. Signal fence on graphics queue: queue->Signal(fence, 1)
  // 3. Create event handle: CreateEvent(nullptr, FALSE, FALSE, nullptr)
  // 4. Wait for fence: fence->SetEventOnCompletion(1, event)
  // 5. Wait for event: WaitForSingleObject(event, INFINITE)
  // 6. Close event handle
}

}  // namespace demo::rhi::d3d12