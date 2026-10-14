#pragma once

#include "MetalResourceTable.h"

namespace MTL {
class ComputePipelineState;
class Device;
class Function;
}

namespace MTL4 {
class Compiler;
}

namespace demo::rhi::metal {

[[nodiscard]] MetalPipelineBindingLayout buildMetalPipelineBindingLayout(
  const PipelineBindingSchemaDesc& schema, const VertexInputLayoutDesc& vertexInput,
  const MetalResourceTable& resources, bool useNestedResourceArgumentBuffer = false);

[[nodiscard]] MetalPipelineRecord compileMetalGraphicsPipeline(
  MTL::Device& device, MTL4::Compiler& compiler, const GraphicsPipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation);

[[nodiscard]] MetalPipelineRecord compileMetalComputePipeline(
  MTL4::Compiler& compiler, const ComputePipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation);

struct MetalBuiltinPipelines
{
  MTL::ComputePipelineState* blit{nullptr};
  MTL::ComputePipelineState* indexedIndirectUInt32{nullptr};
  MTL::ComputePipelineState* indexedIndirectUInt16{nullptr};
  MTL::Function* indexedIndirectFunction{nullptr};
};

// Loads the build-time Slang metallib and creates backend-private pipelines.
// Every returned object is owned by the caller.
[[nodiscard]] MetalBuiltinPipelines compileMetalBuiltinPipelines(
  MTL::Device& device, MTL4::Compiler& compiler, bool enableShaderValidation);

}  // namespace demo::rhi::metal
