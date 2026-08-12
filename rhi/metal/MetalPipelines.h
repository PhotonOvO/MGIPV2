#pragma once

#include "MetalResourceTable.h"

namespace MTL {
class ComputePipelineState;
class Device;
}

namespace MTL4 {
class Compiler;
}

namespace demo::rhi::metal {

[[nodiscard]] MetalPipelineBindingLayout buildMetalPipelineBindingLayout(
  const PipelineBindingSchemaDesc& schema, const VertexInputLayoutDesc& vertexInput,
  const MetalResourceTable& resources);

[[nodiscard]] MetalPipelineRecord compileMetalGraphicsPipeline(
  MTL::Device& device, MTL4::Compiler& compiler, const GraphicsPipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation);

[[nodiscard]] MetalPipelineRecord compileMetalComputePipeline(
  MTL4::Compiler& compiler, const ComputePipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation);

// Compiles the backend-private scaling/flipping kernel through the Metal 4
// compiler. The returned pipeline is owned by the caller.
[[nodiscard]] MTL::ComputePipelineState* compileMetalBlitPipeline(
  MTL4::Compiler& compiler, bool enableShaderValidation);

}  // namespace demo::rhi::metal
