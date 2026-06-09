#pragma once

#include "RHIHandles.h"
#include "RHIBindlessTypes.h"
#include "RHIShaderReflection.h"
#include "RHITypes.h"

#include <cstdint>
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

struct PipelinePushConstantRange
{
  ShaderStage stages{ShaderStage::none};
  uint32_t    offset{0};
  uint32_t    size{0};
};

enum class RootBindingKind : uint8_t
{
  constants = 0,
  gpuPointer,
  dynamicBuffer,
};

struct PipelineArgumentSlotDesc
{
  uint32_t             slot{0};
  ArgumentLayoutHandle layout{};
  ShaderStage          visibility{ShaderStage::none};
  const char*          debugName{nullptr};
};

struct DynamicBufferBindingDesc
{
  uint32_t tableSlot{0xFFFFFFFFu};
  uint32_t binding{0xFFFFFFFFu};
};

struct RootBindingDesc
{
  uint32_t                 slot{0};
  RootBindingKind          kind{RootBindingKind::constants};
  ShaderStage              visibility{ShaderStage::none};
  uint32_t                 size{0};
  uint32_t                 alignment{0};
  DynamicBufferBindingDesc dynamicBuffer{};
  const char*              debugName{nullptr};
};

struct PipelineBindingSchemaDesc
{
  // Canonical Phase 6 contract: these are logical renderer-facing table
  // slots, not backend set/root-signature/native pipeline-layout objects.
  const PipelineArgumentSlotDesc* argumentSlots{nullptr};
  uint32_t                        argumentSlotCount{0};

  // Root binding slots are logical slots consumed by setRootConstants,
  // setRootPointer, and dynamic-buffer metadata. Byte offsets, root
  // parameters, and argument/root indices are backend-private lowering details.
  const RootBindingDesc* rootBindings{nullptr};
  uint32_t               rootBindingCount{0};
};

enum class PipelineBindingSchemaValidationError : uint8_t
{
  none = 0,
  duplicateArgumentSlot,
  argumentSlotArrayMissing,
  argumentSlotWithoutVisibility,
  duplicateRootSlot,
  rootBindingArrayMissing,
  rootBindingWithoutVisibility,
  zeroSizedRootConstants,
  unsupportedGpuPointerSize,
  dynamicBufferWithoutIdentity,
  rootBindingTooLarge,
};

struct PipelineBindingSchemaValidationResult
{
  PipelineBindingSchemaValidationError error{PipelineBindingSchemaValidationError::none};
  uint32_t                             index{0};

  [[nodiscard]] constexpr bool valid() const { return error == PipelineBindingSchemaValidationError::none; }
};

inline constexpr uint32_t kGpuPointerRootBindingSize     = 8;
inline constexpr uint32_t kMaxRootBindingSizeBytes       = 256;
inline constexpr uint32_t kInvalidRootDynamicBufferField = 0xFFFFFFFFu;

[[nodiscard]] inline constexpr PipelineBindingSchemaValidationResult validatePipelineBindingSchema(const PipelineBindingSchemaDesc& schema)
{
  if(schema.argumentSlotCount > 0 && schema.argumentSlots == nullptr)
  {
    return {PipelineBindingSchemaValidationError::argumentSlotArrayMissing, 0};
  }
  if(schema.rootBindingCount > 0 && schema.rootBindings == nullptr)
  {
    return {PipelineBindingSchemaValidationError::rootBindingArrayMissing, 0};
  }

  for(uint32_t i = 0; i < schema.argumentSlotCount; ++i)
  {
    const PipelineArgumentSlotDesc& slot = schema.argumentSlots[i];
    if(slot.visibility == ShaderStage::none)
    {
      return {PipelineBindingSchemaValidationError::argumentSlotWithoutVisibility, i};
    }

    for(uint32_t j = i + 1; j < schema.argumentSlotCount; ++j)
    {
      if(slot.slot == schema.argumentSlots[j].slot)
      {
        return {PipelineBindingSchemaValidationError::duplicateArgumentSlot, j};
      }
    }
  }

  for(uint32_t i = 0; i < schema.rootBindingCount; ++i)
  {
    const RootBindingDesc& binding = schema.rootBindings[i];
    if(binding.visibility == ShaderStage::none)
    {
      return {PipelineBindingSchemaValidationError::rootBindingWithoutVisibility, i};
    }

    if(binding.size > kMaxRootBindingSizeBytes)
    {
      return {PipelineBindingSchemaValidationError::rootBindingTooLarge, i};
    }

    switch(binding.kind)
    {
      case RootBindingKind::constants:
        if(binding.size == 0)
        {
          return {PipelineBindingSchemaValidationError::zeroSizedRootConstants, i};
        }
        break;
      case RootBindingKind::gpuPointer:
        if(binding.size != kGpuPointerRootBindingSize)
        {
          return {PipelineBindingSchemaValidationError::unsupportedGpuPointerSize, i};
        }
        break;
      case RootBindingKind::dynamicBuffer:
        if(binding.dynamicBuffer.tableSlot == kInvalidRootDynamicBufferField ||
           binding.dynamicBuffer.binding == kInvalidRootDynamicBufferField)
        {
          return {PipelineBindingSchemaValidationError::dynamicBufferWithoutIdentity, i};
        }
        break;
    }

    for(uint32_t j = i + 1; j < schema.rootBindingCount; ++j)
    {
      if(binding.slot == schema.rootBindings[j].slot)
      {
        return {PipelineBindingSchemaValidationError::duplicateRootSlot, j};
      }
    }
  }

  return {};
}

inline std::vector<PipelinePushConstantRange> derivePipelinePushConstantRanges(const ShaderReflectionData& reflection)
{
  std::vector<PipelinePushConstantRange> ranges;
  ranges.reserve(reflection.pushConstantRanges.size());
  for(const PushConstantRange& range : reflection.pushConstantRanges)
  {
    ranges.push_back(PipelinePushConstantRange{
        .stages = toShaderStageMask(range.stageFlags),
        .offset = range.offset,
        .size   = range.size,
    });
  }
  return ranges;
}

struct SpecializationData
{
  const void* data{nullptr};
  uint32_t    size{0};
};

struct PipelineShaderStageDesc
{
  ShaderStage     stage{ShaderStage::none};
  // RDEV-02: renderer 侧传 SPIR-V 字节码；backend 内建/销毁 shader module。
  // spirvSize 单位为字节（非元素数）：std::size(arr) * sizeof(uint32_t)。
  const uint32_t* spirvCode{nullptr};
  size_t          spirvSize{0};
  const char*     entryPoint{"main"};
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

  // Canonical binding/root schema. The older argumentLayouts and
  // pushConstantRanges fields below are legacy compatibility inputs consumed
  // only by backend lowering while renderer call sites migrate to schema slots.
  PipelineBindingSchemaDesc bindingSchema{};

  // Transitional argument layouts: array index equals logical table slot.
  const ArgumentLayoutHandle*     argumentLayouts{nullptr};
  uint32_t                        argumentLayoutCount{0};
  const PipelinePushConstantRange* pushConstantRanges{nullptr};
  uint32_t                         pushConstantRangeCount{0};
  uint32_t                         specializationVariant{0};
};

struct ComputePipelineDesc
{
  PipelineShaderStageDesc         shaderStage{};
  PipelineBindingSchemaDesc       bindingSchema{};
  const ArgumentLayoutHandle*     argumentLayouts{nullptr};
  uint32_t                        argumentLayoutCount{0};
  const PipelinePushConstantRange* pushConstantRanges{nullptr};
  uint32_t                        pushConstantRangeCount{0};
  uint32_t                        specializationVariant{0};
  uint64_t                        pipelineFlags{0};
};

enum class ShaderIRFormat : uint8_t
{
  unknown = 0,
  spirv,
  dxil,
  metalLibrary,
};

struct ShaderLibraryDesc
{
  ShaderIRFormat format{ShaderIRFormat::unknown};
  const void*    data{nullptr};
  uint64_t       size{0};
  const char*    debugName{nullptr};
};

struct PipelineCompileOptions
{
  bool        enablePipelineCache{true};
  bool        asyncCompile{false};
  const char* debugName{nullptr};
};

}  // namespace demo::rhi
