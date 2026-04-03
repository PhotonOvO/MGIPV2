#pragma once

#include "../RHIPipeline.h"

#include <vector>

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

class MetalPipelineLayout final : public PipelineLayout
{
public:
  MetalPipelineLayout() = default;
  ~MetalPipelineLayout() override;

  void init(void* nativeDevice, const PipelineLayoutDesc& desc) override;
  void deinit() override;

  [[nodiscard]] const PipelineLayoutDesc& getDesc() const override { return m_desc; }
  [[nodiscard]] uint64_t                  getNativeHandle() const override { return 0; }

private:
  void* m_device{nullptr};  // id<MTLDevice>

  std::vector<uint32_t> m_argumentBufferIndices;  // Indices of bind tables
  uint32_t              m_pushConstantRange{0};   // Size of push constants

  PipelineLayoutDesc m_desc{};
  bool               m_initialized{false};
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

}  // namespace demo::rhi::metal