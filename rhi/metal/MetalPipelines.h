#pragma once

#include "../RHIPipeline.h"

namespace demo::rhi::metal {

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

// Stub helpers consume the public RHI pipeline descriptors. A future Metal
// implementation will lower PipelineBindingSchemaDesc to argument
// buffers/tables, encoder bytes or buffers for root constants, and explicit
// GpuPtr/dynamic-buffer bindings inside the backend. No Vulkan pipeline-layout
// or push-constant byte-offset model is part of this contract.
void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo);
void* createComputePipeline(void* nativeDevice, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::metal
