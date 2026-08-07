#pragma once

#include "RHIHandles.h"
#include "RHIBindlessTypes.h"
#include "RHIShaderReflection.h"
#include "RHITypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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
  std::span<const PipelineArgumentSlotDesc> argumentSlots{};


  // Root binding slots are logical slots consumed by setRootConstants,
  // setRootPointer, and dynamic-buffer metadata. Byte offsets, root
  // parameters, and argument/root indices are backend-private lowering details.
  std::span<const RootBindingDesc> rootBindings{};

};

class PipelineBindingSchemaStorage
{
public:
  PipelineBindingSchemaStorage(
    std::span<const ArgumentLayoutHandle> layouts = {},
    std::span<const RootBindingDesc> rootBindings = {},
    ShaderStage argumentVisibility = ShaderStage::all)
    : m_rootBindings(rootBindings.begin(), rootBindings.end())
  {
    m_argumentSlots.reserve(layouts.size());
    for(uint32_t slot = 0; slot < layouts.size(); ++slot)
    {
      m_argumentSlots.push_back(PipelineArgumentSlotDesc{
        .slot = slot,
        .layout = layouts[slot],
        .visibility = argumentVisibility,
      });
    }
  }

  [[nodiscard]] PipelineBindingSchemaDesc view() const
  {
    return {
      .argumentSlots = m_argumentSlots,

      .rootBindings = m_rootBindings,

    };
  }

private:
  std::vector<PipelineArgumentSlotDesc> m_argumentSlots;
  std::vector<RootBindingDesc> m_rootBindings;
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
  invalidRootBindingAlignment,
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
  for(uint32_t i = 0; i < schema.argumentSlots.size(); ++i)
  {
    const PipelineArgumentSlotDesc& slot = schema.argumentSlots[i];
    if(slot.visibility == ShaderStage::none)
    {
      return {PipelineBindingSchemaValidationError::argumentSlotWithoutVisibility, i};
    }

    for(uint32_t j = i + 1; j < schema.argumentSlots.size(); ++j)
    {
      if(slot.slot == schema.argumentSlots[j].slot)
      {
        return {PipelineBindingSchemaValidationError::duplicateArgumentSlot, j};
      }
    }
  }

  for(uint32_t i = 0; i < schema.rootBindings.size(); ++i)
  {
    const RootBindingDesc& binding = schema.rootBindings[i];
    if(binding.visibility == ShaderStage::none)
    {
      return {PipelineBindingSchemaValidationError::rootBindingWithoutVisibility, i};
    }

    if(binding.alignment != 0 && (binding.alignment & (binding.alignment - 1u)) != 0)
    {
      return {PipelineBindingSchemaValidationError::invalidRootBindingAlignment, i};
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

    for(uint32_t j = i + 1; j < schema.rootBindings.size(); ++j)
    {
      if(binding.slot == schema.rootBindings[j].slot)
      {
        return {PipelineBindingSchemaValidationError::duplicateRootSlot, j};
      }
    }
  }

  return {};
}

inline std::vector<RootBindingDesc> derivePipelineRootBindings(const ShaderReflectionData& reflection)
{
  std::vector<RootBindingDesc> bindings;
  bindings.reserve(reflection.pushConstantRanges.size());
  for(const PushConstantRange& range : reflection.pushConstantRanges)
  {
    bindings.push_back(RootBindingDesc{
        .slot = range.offset,
        .kind = RootBindingKind::constants,
        .visibility = toShaderStageMask(range.stageFlags),
        .size = range.size,
    });
  }
  return bindings;
}

struct SpecializationData
{
  std::span<const std::byte> bytes{};
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
  std::span<const std::byte> data{};
  const char* debugName{nullptr};
};

struct ShaderEntry
{
  ShaderStage         stage{ShaderStage::none};
  ShaderLibraryHandle library{};
  std::string_view    entryPoint{"main"};
  uint32_t                      specializationVariant{0};
  SpecializationData            specializationData{};
  std::span<const SpecializationConstant> specializationConstants{};

};

struct PipelineRenderingInfo
{
  std::span<const TextureFormat> colorFormats{};

  TextureFormat        depthFormat{TextureFormat::undefined};
};

struct GraphicsPipelineDesc
{
  std::span<const ShaderEntry>          shaderStages{};

  VertexInputLayoutDesc          vertexInput{};
  RasterState                    rasterState{};
  DepthState                     depthState{};
  std::span<const BlendAttachmentState> blendStates{};

  std::span<const DynamicState>         dynamicStates{};

  PipelineRenderingInfo          renderingInfo{};

  PipelineBindingSchemaDesc bindingSchema{};
  uint32_t                  specializationVariant{0};
};

struct ComputePipelineDesc
{
  ShaderEntry               shaderStage{};
  PipelineBindingSchemaDesc bindingSchema{};
  uint32_t                  specializationVariant{0};
  uint64_t                        pipelineFlags{0};
};

struct PipelineCompileOptions
{
  bool        enablePipelineCache{true};
  bool        asyncCompile{false};
  const char* debugName{nullptr};
};

}  // namespace demo::rhi
