#include "MetalDevice.h"

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
  assert(false && "Metal implementation not yet available");
}

void MetalDevice::deinit()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Release Metal queues (ARC handles automatically)
  // 2. Release Metal device (ARC handles automatically)
  // 3. Clear capability state
}

uint64_t MetalDevice::getNativeInstance() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have an instance concept like Vulkan
  // Return 0 or opaque handle if needed for interop
  return 0;
}

uint64_t MetalDevice::getNativePhysicalDevice() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't separate physical/logical devices
  // Return same as getNativeDevice() or 0
  return 0;
}

uint64_t MetalDevice::getNativeDevice() const
{
  // TODO: Metal implementation
  // NOTES: Cast id<MTLDevice> to uint64_t
  // Use __bridge_retained or similar for ARC interop
  return 0;
}

uint32_t MetalDevice::getApiVersion() const
{
  // TODO: Metal implementation
  // NOTES: Map Metal version to Vulkan-style API version
  // Metal 3+ on macOS 14+ → ~1.4 equivalent
  // Return 0 or mapped version
  return 0;
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
  // - device.name → deviceName
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

void* MetalDevice::getFeaturesChainHead() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have feature chains like Vulkan
  // Return nullptr
  return nullptr;
}

QueueInfo MetalDevice::getGraphicsQueue() const
{
  // TODO: Metal implementation
  // NOTES: Return graphics queue info
  // Metal queues are command queues, not separate families
  return m_graphicsQueue.toRhi();
}

QueueInfo MetalDevice::getComputeQueue() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have separate compute queues
  // Return same as graphics or shared queue
  return m_computeQueue.toRhi();
}

QueueInfo MetalDevice::getTransferQueue() const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have separate transfer queues
  // Return same as graphics (use blit encoder for transfers)
  return m_transferQueue.toRhi();
}

bool MetalDevice::isInstanceExtensionSupported(const char* name) const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have instance extensions like Vulkan
  // Return false or check Metal framework availability
  (void)name;
  return false;
}

bool MetalDevice::isDeviceExtensionSupported(const char* name) const
{
  // TODO: Metal implementation
  // NOTES: Metal doesn't have device extensions like Vulkan
  // Return false or check Metal feature sets
  (void)name;
  return false;
}

void MetalDevice::waitIdle()
{
  // TODO: Metal implementation
  // NOTES: Use MTLCommandBuffer addCompletedHandler to wait
  // Or use MTLSharedEvent for synchronization
  // Metal doesn't have explicit device-wide idle like vkDeviceWaitIdle
}

}  // namespace demo::rhi::metal