#include "MetalPipelines.h"

#include <cassert>

namespace demo::rhi::metal {

void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Compile vertex shader: device.newLibraryWithSource
  // 2. Compile fragment shader: device.newLibraryWithSource
  // 3. Extract functions: library.newFunctionWithName
  // 4. Create MTLRenderPipelineDescriptor:
  //    - Set vertexFunction and fragmentFunction
  //    - Configure color attachment pixel formats
  //    - Configure depth/stencil attachment formats
  //    - Set vertex descriptor (input layout)
  //    - Set rasterSampleCount for MSAA
  // 5. Create MTLRenderPipelineState: device.newRenderPipelineStateWithDescriptor
  //
  // Argument Buffer Setup:
  // - Shader declares argument buffers with [[buffer(N)]]
  // - Public PipelineArgumentSlotDesc slots map to backend argument buffers/tables
  // - RootBindingDesc constants and pointers map to encoder bytes or buffers
  // - No explicit Vulkan-style pipeline layout or push-constant byte offset
  //
  // Returns: id<MTLRenderPipelineState> (as void*)
  (void)nativeDevice;
  (void)createInfo;
  assert(false && "Metal implementation not yet available");
  return nullptr;
}

void* createComputePipeline(void* nativeDevice, const ComputePipelineCreateInfo& createInfo)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Compile compute shader: device.newLibraryWithSource
  // 2. Extract compute function: library.newFunctionWithName
  // 3. Create MTLComputePipelineState: device.newComputePipelineStateWithFunction
  //
  // NOTES: Compute pipelines are simpler than graphics (no render state)
  // Argument buffer/root binding lowering is same as graphics
  //
  // Returns: id<MTLComputePipelineState> (as void*)
  (void)nativeDevice;
  (void)createInfo;
  assert(false && "Metal implementation not yet available");
  return nullptr;
}

}  // namespace demo::rhi::metal
