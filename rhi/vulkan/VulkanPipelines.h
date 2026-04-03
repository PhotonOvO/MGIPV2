#pragma once

#include "../../common/Common.h"
#include "../RHIPipeline.h"

#include <vector>

namespace demo::rhi::vulkan {

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

struct VulkanPipelineLayoutBindingMapping
{
  uint32_t              logicalSet{0};
  VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
};

inline VulkanPipelineLayoutBindingMapping makePipelineLayoutBindingMapping(uint32_t logicalSet, uint64_t descriptorSetLayoutHandle)
{
  return VulkanPipelineLayoutBindingMapping{
      .logicalSet          = logicalSet,
      .descriptorSetLayout = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(descriptorSetLayoutHandle)),
  };
}

struct VulkanPipelineLayoutLowering
{
  const VulkanPipelineLayoutBindingMapping* setLayouts{nullptr};
  uint32_t                                  setLayoutCount{0};
};

class VulkanPipelineLayout final : public PipelineLayout
{
public:
  void init(void* nativeDevice, const PipelineLayoutDesc& desc) override;
  void init(void* nativeDevice, const PipelineLayoutDesc& desc, const VulkanPipelineLayoutLowering& lowering);
  void deinit() override;

  [[nodiscard]] const PipelineLayoutDesc& getDesc() const override { return m_desc; }
  [[nodiscard]] uint64_t getNativeHandle() const override { return reinterpret_cast<uint64_t>(m_layout); }

private:
  VkDevice           m_device{VK_NULL_HANDLE};
  VkPipelineLayout   m_layout{VK_NULL_HANDLE};
  PipelineLayoutDesc m_desc{};
};

struct GraphicsPipelineCreateInfo
{
  PipelineKey          key{};
  GraphicsPipelineDesc desc{};
};

struct ComputePipelineCreateInfo
{
  PipelineKey            key{};
  ComputePipelineDesc    desc{};
  VkPipelineCreateFlags2 pipelineFlags{0};
};

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineCreateInfo& createInfo);
VkPipeline createComputePipeline(VkDevice device, const ComputePipelineCreateInfo& createInfo);

}  // namespace demo::rhi::vulkan
