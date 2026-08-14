#include "Metal4Availability.h"

#include <Availability.h>
#include <Metal/MTL4CommandAllocator.hpp>
#include <Metal/MTL4CommandBuffer.hpp>
#include <Metal/MTL4CommandQueue.hpp>
#include <Metal/MTL4ComputeCommandEncoder.hpp>
#include <Metal/MTL4RenderCommandEncoder.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLResidencySet.hpp>
#include <Metal/MTLTextureViewPool.hpp>

#include <objc/runtime.h>

namespace demo::rhi::metal {

Metal4Availability queryMetal4Availability() noexcept
{
  MTL::Device* device = MTL::CreateSystemDefaultDevice();
  Metal4Availability result{.deviceAvailable = device != nullptr};
  if(device != nullptr)
    device->release();

#if (defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000) || \
    (defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000)
    using CommandQueue = MTL4::CommandQueue*;
    using CommandAllocator = MTL4::CommandAllocator*;
    using CommandBuffer = MTL4::CommandBuffer*;
    using RenderEncoder = MTL4::RenderCommandEncoder*;
    using ComputeEncoder = MTL4::ComputeCommandEncoder*;
    using ResidencySet = MTL::ResidencySet*;
    using TextureViewPool = MTL::TextureViewPool*;
    (void)sizeof(CommandQueue);
    (void)sizeof(CommandAllocator);
    (void)sizeof(CommandBuffer);
    (void)sizeof(RenderEncoder);
    (void)sizeof(ComputeEncoder);
    (void)sizeof(ResidencySet);
    (void)sizeof(TextureViewPool);
    result.sdkHasCoreApi = true;
    result.runtimeHasCoreApi =
      objc_getProtocol("MTL4CommandQueue") != nullptr &&
      objc_getProtocol("MTL4CommandAllocator") != nullptr &&
      objc_getProtocol("MTL4CommandBuffer") != nullptr &&
      objc_getProtocol("MTL4RenderCommandEncoder") != nullptr &&
      objc_getProtocol("MTL4ComputeCommandEncoder") != nullptr &&
      objc_getProtocol("MTLResidencySet") != nullptr &&
      objc_getProtocol("MTLTextureViewPool") != nullptr;
#endif
  return result;
}

}  // namespace demo::rhi::metal
