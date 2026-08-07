#include "Metal4Availability.h"

#import <AvailabilityMacros.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace demo::rhi::metal {

Metal4Availability queryMetal4Availability() noexcept
{
  @autoreleasepool
  {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    Metal4Availability result{.deviceAvailable = device != nil};

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    using CommandQueue = id<MTL4CommandQueue>;
    using CommandAllocator = id<MTL4CommandAllocator>;
    using CommandBuffer = id<MTL4CommandBuffer>;
    using RenderEncoder = id<MTL4RenderCommandEncoder>;
    using ComputeEncoder = id<MTL4ComputeCommandEncoder>;
    using ResidencySet = id<MTLResidencySet>;
    using TextureViewPool = id<MTLTextureViewPool>;
    (void)sizeof(CommandQueue);
    (void)sizeof(CommandAllocator);
    (void)sizeof(CommandBuffer);
    (void)sizeof(RenderEncoder);
    (void)sizeof(ComputeEncoder);
    (void)sizeof(ResidencySet);
    (void)sizeof(TextureViewPool);
    result.sdkHasCoreApi = true;

    if(@available(macOS 26.0, *))
    {
      result.runtimeHasCoreApi =
        NSProtocolFromString(@"MTL4CommandQueue") != nil &&
        NSProtocolFromString(@"MTL4CommandAllocator") != nil &&
        NSProtocolFromString(@"MTL4CommandBuffer") != nil &&
        NSProtocolFromString(@"MTL4RenderCommandEncoder") != nil &&
        NSProtocolFromString(@"MTL4ComputeCommandEncoder") != nil &&
        NSProtocolFromString(@"MTLResidencySet") != nil &&
        NSProtocolFromString(@"MTLTextureViewPool") != nil;
    }
#endif
    return result;
  }
}

}  // namespace demo::rhi::metal
