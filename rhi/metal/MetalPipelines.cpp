#include "MetalPipelines.h"

#include <cassert>

namespace demo::rhi::metal {

MetalPipelineLayout::~MetalPipelineLayout()
{
  deinit();
}

void MetalPipelineLayout::init(void* nativeDevice, const PipelineLayoutDesc& desc)
{
  // TODO: Metal implementation
  // NOTES:
  // Metal doesn't have explicit pipeline layout objects
  // This method stores metadata for pipeline creation:
  // 1. Store device reference
  // 2. Store descriptor
  // 3. Track bind table layouts (indices for argument buffers)
  // 4. Track push constant ranges (for setVertexBytes/setFragmentBytes)
  m_desc = desc;
  (void)nativeDevice;
  m_initialized = true;
}

void MetalPipelineLayout::deinit()
{
  // TODO: Metal implementation
  // NOTES:
  // Clear metadata only (no native objects to release)
  m_argumentBufferIndices.clear();
  m_initialized = false;
}

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
  // - Bind tables will be set as regular buffers during encoding
  // - No explicit pipeline layout binding
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
  // Argument buffer setup is same as graphics
  //
  // Returns: id<MTLComputePipelineState> (as void*)
  (void)nativeDevice;
  (void)createInfo;
  assert(false && "Metal implementation not yet available");
  return nullptr;
}

}  // namespace demo::rhi::metal