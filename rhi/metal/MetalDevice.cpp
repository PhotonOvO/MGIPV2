#include "MetalDevice.h"
#include <stdexcept>

#include <cassert>

namespace demo::rhi::metal {

MetalDevice::~MetalDevice()
{
  deinit();
}

void MetalDevice::init(const DeviceCreateInfo& createInfo)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Create device using MTLCreateSystemDefaultDevice()
  // 2. Validate device supports Metal 3+ (macOS 14+, iOS 17+)
  // 3. Query Metal feature set using device.supportsFamily()
  // 4. Map Metal capabilities to RHI CapabilityTier
  // 5. Initialize queues:
  //    - Graphics: device.newCommandQueue()
  //    - Compute: May share with graphics (Metal doesn't have separate queues)
  //    - Transfer: May share with graphics (use MTLBlitCommandEncoder)
  //
  // Example Metal API pattern:
  // id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  // if (!device) { /* error handling */ }
  // id<MTLCommandQueue> queue = [device newCommandQueue];
  (void)createInfo;
  throw std::runtime_error("Metal backend is contract-shaped and requires a macOS Metal 4 implementation");
}

void MetalDevice::deinit()
{
  // Deinit remains safe after a failed or unavailable Metal initialization.

  if (!m_initialized)
    return;
  // TODO: Metal implementation
  // NOTES:
  // 1. Release Metal queues (ARC handles automatically)
  // 2. Release Metal device (ARC handles automatically)
  // 3. Clear capability state
}

BackendInfo MetalDevice::getBackendInfo() const
{
  return BackendInfo{
    .type = BackendType::metal,
    .apiName = "Metal",
    .version = BackendVersion{.major = 4},
  };
}

const char* MetalDevice::getDeviceName() const
{
  // TODO: Metal implementation
  // NOTES: Use [device.name UTF8String]
  return nullptr;
}

const PhysicalDeviceInfo& MetalDevice::getPhysicalDeviceInfo() const
{
  // TODO: Metal implementation
  // NOTES: Populate from Metal device properties
  // - device.name â†’ deviceName
  // - Map GPU family to deviceType
  // - vendorId/deviceId not applicable (use 0)
  return m_physicalDeviceInfo;
}

const DeviceFeatureInfo& MetalDevice::getEnabledFeatureInfo() const
{
  // TODO: Metal implementation
  // NOTES: Map Metal features to RHI feature flags
  // - timelineSemaphore: false (use MTLCommandBuffer completion)
  // - synchronization2: false (Metal uses implicit sync)
  // - dynamicRendering: true (native support)
  // - maintenance5/maintenance6: N/A (Vulkan-specific)
  return m_featureInfo;
}

CapabilityReport MetalDevice::queryCapabilities() const
{
  // TODO: Metal implementation
  // NOTES: Query Metal capabilities and map to RHI tiers
  // Use device.supportsFamily() to check GPU family
  // Use device.supportsFeatureSet() to check feature sets
  return m_capabilities;
}

bool MetalDevice::supports(CapabilityTier tier) const
{
  // TODO: Metal implementation
  // NOTES: Compare requested tier with Metal GPU family
  // - Tier 3: Apple GPU family 3+
  // - Tier 2: Apple GPU family 2+
  // - Tier 1: Any Apple GPU
  (void)tier;
  return false;
}

const MemoryProperties& MetalDevice::getPhysicalMemoryProperties() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't expose memory heaps like Vulkan
  // Return empty structure or Unified memory model
  return m_memoryProperties;
}

Queue* MetalDevice::getQueue(QueueClass)
{
  return nullptr;
}

std::unique_ptr<CommandAllocator> MetalDevice::createCommandAllocator(QueueClass)
{
  throw std::runtime_error("Metal command allocators require the macOS Metal 4 backend");
}
void MetalDevice::waitIdle()
{
  throw std::runtime_error("Metal waitIdle requires the macOS Metal 4 backend");
  // TODO: Metal implementation
  // NOTES: Use MTLCommandBuffer addCompletedHandler to wait
  // Or use MTLSharedEvent for synchronization
  // Metal doesn't have explicit device-wide idle like vkDeviceWaitIdle
}

}  // namespace demo::rhi::metal