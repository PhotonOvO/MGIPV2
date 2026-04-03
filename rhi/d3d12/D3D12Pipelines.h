#pragma once

#include "../RHIPipeline.h"

#include <vector>

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

class D3D12PipelineLayout final : public PipelineLayout
{
public:
  D3D12PipelineLayout() = default;
  ~D3D12PipelineLayout() override;

  void init(void* nativeDevice, const PipelineLayoutDesc& desc) override;
  void deinit() override;

  [[nodiscard]] const PipelineLayoutDesc& getDesc() const override { return m_desc; }
  [[nodiscard]] uint64_t getNativeHandle() const override { return reinterpret_cast<uint64_t>(m_rootSignature); }

private:
  // NOTES: D3D12 Root Signature for Pipeline Layout
  // D3D12 uses root signatures to define pipeline layout:
  //
  // Root Signature Parameters:
  // - Root constants: Inline 32-bit constants (max 64 DWORDs, 256 bytes)
  // - Root descriptors: Direct buffer/resource views (no descriptor heap indirection)
  // - Descriptor tables: Pointers into descriptor heaps (for bindless)
  //
  // Bind Table Mapping:
  // - RHIBindLayout entries map to descriptor table parameters
  // - Each descriptor table points to a region in CBV_SRV_UAV or sampler heap
  // - Logical indices map to offsets within the descriptor table
  //
  // Push Constants:
  // - Use root constants (inline 32-bit values)
  // - Limited to 256 bytes total (64 DWORDs)
  // - Bound to root parameter in shader via register space/b0
  //
  // Initialization:
  // 1. Create D3D12_ROOT_SIGNATURE_DESC:
  //    - Add root constant parameter for push constants
  //    - Add descriptor table parameters for each bind table
  // 2. Serialize root signature: D3D12SerializeRootSignature()
  // 3. Create root signature: device->CreateRootSignature()

  void* m_device{nullptr};  // ID3D12Device

  void* m_rootSignature{nullptr};  // ID3D12RootSignature

  std::vector<uint32_t> m_descriptorTableSizes;
  uint32_t              m_pushConstantSize{0};

  PipelineLayoutDesc m_desc{};
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

void* createGraphicsPipeline(void* nativeDevice, const GraphicsPipelineCreateInfo& createInfo);
void* createComputePipeline(void* nativeDevice, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::d3d12