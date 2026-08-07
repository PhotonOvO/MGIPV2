#pragma once

#include "../RHIPipeline.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace demo::rhi::d3d12 {

enum class D3D12ShaderBindingKind : uint8_t
{
  constantBuffer = 0,
  shaderResource,
  unorderedAccess,
  sampler,
  pushConstants,
};

struct D3D12ShaderBinding
{
  std::string name;
  uint32_t logicalSet{0};
  uint32_t logicalBinding{0};
  D3D12ShaderBindingKind kind{D3D12ShaderBindingKind::shaderResource};
  uint32_t shaderRegister{0};
  uint32_t registerSpace{0};
  uint32_t descriptorCount{1};
  bool unbounded{false};
};

struct D3D12VertexInput
{
  uint32_t location{0};
  std::string semanticName;
  uint32_t semanticIndex{0};
};

struct D3D12CompiledShader
{
  std::vector<uint8_t> bytecode;
  std::vector<D3D12ShaderBinding> bindings;
  std::vector<D3D12VertexInput> vertexInputs;
  std::filesystem::path sourcePath;
};

[[nodiscard]] D3D12CompiledShader compileShaderToDxil(
  const ShaderEntry& stage, std::span<const uint8_t> shaderBytes);

}  // namespace demo::rhi::d3d12
