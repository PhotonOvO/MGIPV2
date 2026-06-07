#pragma once

#include "../RHIPipeline.h"

namespace demo::rhi::d3d12 {

enum class PipelineShaderIdentity : uint32_t
{
  raster = 0,
  compute,
};

struct PipelineKey
{
  PipelineShaderIdentity shaderIdentity{PipelineShaderIdentity::raster};
  uint32_t               specializationVariant{0};
};

struct GraphicsPipelineCreateInfo
{
  PipelineKey          key{};
  GraphicsPipelineDesc desc{};
};

struct ComputePipelineCreateInfo
{
  PipelineKey         key{};
  ComputePipelineDesc desc{};
};

// Stub helpers consume the public RHI pipeline descriptors. A future D3D12
// implementation will lower PipelineBindingSchemaDesc to descriptor tables,
// root constants, root descriptors/GPU virtual addresses, and dynamic-buffer
// bindings inside the backend. No public layout object is part of this contract.
void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo);
void* createComputePipeline(void* nativeDevice, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::d3d12
