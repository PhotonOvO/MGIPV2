#include "D3D12Pipelines.h"

namespace demo::rhi::d3d12 {

void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo)
{
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Create D3D12_GRAPHICS_PIPELINE_STATE_DESC:
  //    - pRootSignature: backend-lowered from PipelineBindingSchemaDesc
  //      (descriptor tables for argument slots, root constants for constant
  //      slots, root descriptors/GPU VAs for GpuPtr slots, explicit dynamic
  //      buffer bindings)
  //    - VS, PS: shader byte code
  //    - BlendState: D3D12_BLEND_DESC
  //    - RasterizerState: D3D12_RASTERIZER_DESC
  //    - DepthStencilState: D3D12_DEPTH_STENCIL_DESC
  //    - InputLayout: D3D12_INPUT_ELEMENT_DESC array
  //    - PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
  //    - NumRenderTargets: 1
  //    - RTVFormats: DXGI_FORMAT_R8G8B8A8_UNORM
  // 2. Create PSO: device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))
  // 3. Return pso handle
  return nullptr;
}

void* createComputePipeline(void* nativeDevice, const ComputePipelineCreateInfo& createInfo)
{
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Create D3D12_COMPUTE_PIPELINE_STATE_DESC:
  //    - pRootSignature: backend-lowered from PipelineBindingSchemaDesc
  //    - CS: shader byte code
  // 2. Create PSO: device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso))
  // 3. Return pso handle
  return nullptr;
}

}  // namespace demo::rhi::d3d12
