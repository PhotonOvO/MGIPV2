#pragma once

#include "RHIHandles.h"
#include "RHIBindlessTypes.h"
#include "RHIShaderReflection.h"
#include "RHITypes.h"

#include <string>
#include <vector>

namespace demo::rhi {

inline constexpr ResourceIndex makeLogicalResourceIndex(uint32_t logicalSet, uint32_t logicalBinding)
{
  return (logicalSet << 16u) | (logicalBinding & 0xFFFFu);
}

inline constexpr ShaderStage toShaderStageMask(ShaderStageFlagBits flags)
{
  ShaderStage stages = ShaderStage::none;
  if(flags & ShaderStageFlagBits::vertex)
  {
    stages |= ShaderStage::vertex;
  }
  if(flags & ShaderStageFlagBits::fragment)
  {
    stages |= ShaderStage::fragment;
  }
  if(flags & ShaderStageFlagBits::compute)
  {
    stages |= ShaderStage::compute;
  }
  return stages;
}

struct PipelineLayoutEntry
{
  std::string        name{};
  ResourceIndex      logicalIndex{kInvalidResourceIndex};
  uint32_t           logicalSet{0};
  uint32_t           logicalBinding{0};
  ShaderResourceType resourceType{ShaderResourceType::unknown};
  DescriptorType     descriptorType{DescriptorType::sampledImage};
  ShaderStage        stages{ShaderStage::none};
  uint32_t           arraySize{1};

  [[nodiscard]] bool isBindlessArray() const { return arraySize > 1; }
};

struct PipelinePushConstantRange
{
  ShaderStage stages{ShaderStage::none};
  uint32_t    offset{0};
  uint32_t    size{0};
};

struct PipelineLayoutDesc
{
  std::vector<PipelineLayoutEntry>       entries{};
  std::vector<PipelinePushConstantRange> pushConstantRanges{};
  std::string                            debugName{};
};

inline PipelineLayoutDesc derivePipelineLayoutDesc(const ShaderReflectionData& reflection)
{
  PipelineLayoutDesc desc{};
  desc.entries.reserve(reflection.resourceBindings.size());
  desc.pushConstantRanges.reserve(reflection.pushConstantRanges.size());

  for(const ShaderResourceBinding& binding : reflection.resourceBindings)
  {
    desc.entries.push_back(PipelineLayoutEntry{
        .name           = binding.name,
        .logicalIndex   = makeLogicalResourceIndex(binding.set, binding.binding),
        .logicalSet     = binding.set,
        .logicalBinding = binding.binding,
        .resourceType   = binding.resourceType,
        .descriptorType = binding.descriptorType,
        .stages         = toShaderStageMask(binding.stageFlags),
        .arraySize      = binding.arraySize,
    });
  }

  for(const PushConstantRange& range : reflection.pushConstantRanges)
  {
    desc.pushConstantRanges.push_back(PipelinePushConstantRange{
        .stages = toShaderStageMask(range.stageFlags),
        .offset = range.offset,
        .size   = range.size,
    });
  }

  return desc;
}

class PipelineLayout
{
public:
  virtual ~PipelineLayout() = default;

  virtual void init(void* nativeDevice, const PipelineLayoutDesc& desc) = 0;
  virtual void deinit()                                                 = 0;

  [[nodiscard]] virtual const PipelineLayoutDesc& getDesc() const         = 0;
  [[nodiscard]] virtual uint64_t                  getNativeHandle() const = 0;
};

struct SpecializationData
{
  const void* data{nullptr};
  uint32_t    size{0};
};

struct PipelineShaderStageDesc
{
  ShaderStage                   stage{ShaderStage::none};
  uint64_t                      shaderModule{0};
  const char*                   entryPoint{"main"};
  uint32_t                      specializationVariant{0};
  SpecializationData            specializationData{};
  const SpecializationConstant* specializationConstants{nullptr};
  uint32_t                      specializationConstantCount{0};
};

struct PipelineRenderingInfo
{
  const TextureFormat* colorFormats{nullptr};
  uint32_t             colorFormatCount{0};
  TextureFormat        depthFormat{TextureFormat::undefined};
};

struct GraphicsPipelineDesc
{
  const PipelineLayout*          layout{nullptr};
  const PipelineShaderStageDesc* shaderStages{nullptr};
  uint32_t                       shaderStageCount{0};
  VertexInputLayoutDesc          vertexInput{};
  RasterState                    rasterState{};
  DepthState                     depthState{};
  const BlendAttachmentState*    blendStates{nullptr};
  uint32_t                       blendStateCount{0};
  const DynamicState*            dynamicStates{nullptr};
  uint32_t                       dynamicStateCount{0};
  PipelineRenderingInfo          renderingInfo{};

  // BindGroupLayouts for each descriptor set slot
  const BindGroupLayoutHandle*   bindGroupLayouts{nullptr};
  uint32_t                       bindGroupLayoutCount{0};
};

struct ComputePipelineDesc
{
  const PipelineLayout*   layout{nullptr};
  PipelineShaderStageDesc shaderStage{};
};

}  // namespace demo::rhi
