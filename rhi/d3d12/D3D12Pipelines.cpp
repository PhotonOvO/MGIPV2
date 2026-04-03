#include "D3D12Pipelines.h"

namespace demo::rhi::d3d12 {

D3D12PipelineLayout::~D3D12PipelineLayout()
{
  deinit();
}

void D3D12PipelineLayout::init(void* nativeDevice, const PipelineLayoutDesc& desc)
{
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Store device handle and desc
  // 2. Calculate push constant size from desc
  // 3. Create D3D12_ROOT_PARAMETER array:
  //    - Root constant for push constants (if any)
  //    - Descriptor table for each bind table
  // 4. Create D3D12_DESCRIPTOR_RANGE for each descriptor table
  // 5. Create D3D12_ROOT_SIGNATURE_DESC
  // 6. Serialize: D3D12SerializeRootSignature(..., blob, error)
  // 7. Create: device->CreateRootSignature(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))
}

void D3D12PipelineLayout::deinit()
{
  // TODO: D3D12 implementation
  // NOTES:
  // Release root signature: m_rootSignature->Release()
}

void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo)
{
  // TODO: D3D12 implementation
  // NOTES:
  // 1. Create D3D12_GRAPHICS_PIPELINE_STATE_DESC:
  //    - pRootSignature: from pipeline layout
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
  //    - pRootSignature: from pipeline layout
  //    - CS: shader byte code
  // 2. Create PSO: device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso))
  // 3. Return pso handle
  return nullptr;
}

}  // namespace demo::rhi::d3d12