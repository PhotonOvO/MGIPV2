#include "Metal4Contract.h"

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

Metal4NativeContractStatus queryMetal4NativeContract() noexcept
{
  MTL::Device* device = MTL::CreateSystemDefaultDevice();
  Metal4NativeContractStatus result{.deviceAvailable = device != nullptr};
  if(device != nullptr)
    device->release();

#if (defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000) || \
    (defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000)
    struct NativeTypeMap
    {
      MTL4::CommandQueue* queue{nullptr};
      MTL4::CommandAllocator* allocator{nullptr};
      MTL4::CommandBuffer* commandBuffer{nullptr};
      MTL4::RenderCommandEncoder* renderEncoder{nullptr};
      MTL4::ComputeCommandEncoder* computeEncoder{nullptr};
      MTL::ResidencySet* residencySet{nullptr};
      MTL::TextureViewPool* textureViewPool{nullptr};
      MTL::GPUAddress gpuAddress{};
    };
    (void)sizeof(NativeTypeMap);
    result.sdkTypesCompiled = true;
    result.runtimeTypesAvailable =
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
