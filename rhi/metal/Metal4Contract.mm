#include "Metal4Contract.h"

#import <AvailabilityMacros.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace demo::rhi::metal {

Metal4NativeContractStatus queryMetal4NativeContract() noexcept
{
  @autoreleasepool
  {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    Metal4NativeContractStatus result{.deviceAvailable = device != nil};

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    struct NativeTypeMap
    {
      id<MTL4CommandQueue> queue{nil};
      id<MTL4CommandAllocator> allocator{nil};
      id<MTL4CommandBuffer> commandBuffer{nil};
      id<MTL4RenderCommandEncoder> renderEncoder{nil};
      id<MTL4ComputeCommandEncoder> computeEncoder{nil};
      id<MTLResidencySet> residencySet{nil};
      id<MTLTextureViewPool> textureViewPool{nil};
      MTLGPUAddress gpuAddress{};
    };
    (void)sizeof(NativeTypeMap);
    result.sdkTypesCompiled = true;

    if(@available(macOS 26.0, *))
    {
      result.runtimeTypesAvailable =
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