#include "D3D12Device.h"

#include "D3D12CommandBuffer.h"
#include "D3D12Pipelines.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace demo::rhi::d3d12 {
namespace {

void checkHresult(HRESULT result, const char* operation)
{
  if(FAILED(result))
  {
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

std::string d3d12ValidationMessages(ID3D12Device* device)
{
  ID3D12InfoQueue* queue = nullptr;
  if(device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
    return {};

  std::ostringstream result;
  const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
  const UINT64 first = count > 32 ? count - 32 : 0;
  for(UINT64 index = first; index < count; ++index)
  {
    SIZE_T size = 0;
    if(FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0)
      continue;
    std::vector<std::byte> storage(size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if(SUCCEEDED(queue->GetMessage(index, message, &size)) &&
       message->pDescription != nullptr)
      result << '\n' << "[D3D12 " << message->ID << "] " << message->pDescription;
  }
  queue->Release();
  return result.str();
}

bool hasStage(ShaderStage mask, ShaderStage bit)
{
  return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(bit)) != 0;
}
DXGI_FORMAT toDxgiFormat(TextureFormat format)
{
  switch(format)
  {
  case TextureFormat::rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
  case TextureFormat::rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case TextureFormat::bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
  case TextureFormat::rgba16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case TextureFormat::rg16Sfloat: return DXGI_FORMAT_R16G16_FLOAT;
  case TextureFormat::r32Sfloat: return DXGI_FORMAT_R32_FLOAT;
  case TextureFormat::r16Sfloat: return DXGI_FORMAT_R16_FLOAT;
  case TextureFormat::rgba8Snorm: return DXGI_FORMAT_R8G8B8A8_SNORM;
  case TextureFormat::r11g11b10Ufloat: return DXGI_FORMAT_R11G11B10_FLOAT;
  case TextureFormat::d16Unorm: return DXGI_FORMAT_D16_UNORM;
  case TextureFormat::d32Sfloat: return DXGI_FORMAT_D32_FLOAT;
  case TextureFormat::d24UnormS8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case TextureFormat::d32SfloatS8: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT toVertexFormat(VertexFormat format)
{
  switch(format)
  {
  case VertexFormat::r32Sfloat: return DXGI_FORMAT_R32_FLOAT;
  case VertexFormat::r32g32Sfloat: return DXGI_FORMAT_R32G32_FLOAT;
  case VertexFormat::r32g32b32Sfloat: return DXGI_FORMAT_R32G32B32_FLOAT;
  case VertexFormat::r32g32b32a32Sfloat: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  case VertexFormat::r8g8b8a8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
  default: return DXGI_FORMAT_UNKNOWN;
  }
}

D3D12_BLEND toBlend(BlendFactor factor)
{
  switch(factor)
  {
  case BlendFactor::zero: return D3D12_BLEND_ZERO;
  case BlendFactor::srcColor: return D3D12_BLEND_SRC_COLOR;
  case BlendFactor::oneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
  case BlendFactor::dstColor: return D3D12_BLEND_DEST_COLOR;
  case BlendFactor::oneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
  case BlendFactor::srcAlpha: return D3D12_BLEND_SRC_ALPHA;
  case BlendFactor::oneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
  case BlendFactor::dstAlpha: return D3D12_BLEND_DEST_ALPHA;
  case BlendFactor::oneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
  default: return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND_OP toBlendOp(BlendOp op)
{
  switch(op)
  {
  case BlendOp::subtract: return D3D12_BLEND_OP_SUBTRACT;
  case BlendOp::reverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
  case BlendOp::min: return D3D12_BLEND_OP_MIN;
  case BlendOp::max: return D3D12_BLEND_OP_MAX;
  default: return D3D12_BLEND_OP_ADD;
  }
}

D3D12_PRIMITIVE_TOPOLOGY toTopology(PrimitiveTopology topology)
{
  switch(topology)
  {
  case PrimitiveTopology::pointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  case PrimitiveTopology::lineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
  case PrimitiveTopology::lineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
  case PrimitiveTopology::triangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE toTopologyType(PrimitiveTopology topology)
{
  if(topology == PrimitiveTopology::pointList)
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  if(topology == PrimitiveTopology::lineList || topology == PrimitiveTopology::lineStrip)
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

UINT8 toColorMask(ColorComponentFlags mask)
{
  return static_cast<UINT8>(mask);
}

D3D12_DESCRIPTOR_RANGE_TYPE toRangeType(D3D12ShaderBindingKind kind)
{
  switch(kind)
  {
  case D3D12ShaderBindingKind::constantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  case D3D12ShaderBindingKind::unorderedAccess:return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  case D3D12ShaderBindingKind::sampler:        return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  default:                                     return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  }
}

D3D12_ROOT_PARAMETER_TYPE toRootDescriptorType(D3D12ShaderBindingKind kind)
{
  switch(kind)
  {
  case D3D12ShaderBindingKind::unorderedAccess:return D3D12_ROOT_PARAMETER_TYPE_UAV;
  case D3D12ShaderBindingKind::shaderResource: return D3D12_ROOT_PARAMETER_TYPE_SRV;
  default:                                     return D3D12_ROOT_PARAMETER_TYPE_CBV;
  }
}

struct LayoutPlacement
{
  uint32_t logicalSet{0};
  uint32_t logicalBinding{0};
  uint32_t arrayCount{1};
  bool dynamicOffset{false};
  uint32_t resourceOffset{~0u};
  uint32_t samplerOffset{~0u};
};

struct RootBuildResult
{
  ID3D12RootSignature* signature{nullptr};
  std::unordered_map<uint32_t, D3D12Device::PipelineTableBinding> tables;
  std::unordered_map<uint32_t, std::vector<D3D12Device::PipelineRecord::RootParameterBinding>> roots;
};

const LayoutPlacement* findPlacement(const std::vector<LayoutPlacement>& placements,
                                     uint32_t set, uint32_t binding)
{
  const auto found = std::find_if(
    placements.begin(), placements.end(),
    [set, binding](const LayoutPlacement& placement)
    {
      return placement.logicalSet == set && placement.logicalBinding == binding;
    });
  return found == placements.end() ? nullptr : &*found;
}

RootBuildResult buildRootSignature(
  ID3D12Device* device, const D3D12CompiledShader& shader,
  const std::vector<LayoutPlacement>& placements,
  const PipelineBindingSchemaDesc& bindingSchema,
  ShaderStage stageMask, bool graphics)
{
  RootBuildResult result{};
  std::vector<D3D12_ROOT_PARAMETER> parameters;
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> rangeGroups;
  parameters.reserve(32);
  rangeGroups.reserve(16);

  std::vector<uint32_t> logicalSets;
  for(const LayoutPlacement& placement : placements)
  {
    if(std::find(logicalSets.begin(), logicalSets.end(), placement.logicalSet) ==
       logicalSets.end())
      logicalSets.push_back(placement.logicalSet);
  }

  for(uint32_t logicalSet : logicalSets)
  {
    std::vector<D3D12_DESCRIPTOR_RANGE> resourceRanges;
    std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges;
    for(const D3D12ShaderBinding& binding : shader.bindings)
    {
      if(binding.kind == D3D12ShaderBindingKind::pushConstants ||
         binding.logicalSet != logicalSet)
        continue;

      const LayoutPlacement* placement =
        findPlacement(placements, binding.logicalSet, binding.logicalBinding);
      if(placement == nullptr)
      {
        throw std::runtime_error(
          "DX12 shader binding is absent from the argument layout: " + binding.name);
      }

      if(placement->dynamicOffset &&
         binding.kind != D3D12ShaderBindingKind::sampler)
      {
        const uint32_t rootIndex = static_cast<uint32_t>(parameters.size());
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = toRootDescriptorType(binding.kind);
        parameter.Descriptor.ShaderRegister = binding.shaderRegister;
        parameter.Descriptor.RegisterSpace = binding.registerSpace;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters.push_back(parameter);
        result.tables[logicalSet].dynamicBindings.push_back({
          binding.logicalBinding, rootIndex, static_cast<uint32_t>(binding.kind)});
        continue;
      }

      const bool sampler = binding.kind == D3D12ShaderBindingKind::sampler;
      const uint32_t offset = sampler ? placement->samplerOffset
                                      : placement->resourceOffset;
      if(offset == ~0u)
        throw std::runtime_error("DX12 reflected descriptor has no heap placement");

      D3D12_DESCRIPTOR_RANGE range{};
      range.RangeType = toRangeType(binding.kind);
      range.NumDescriptors = placement->arrayCount;
      range.BaseShaderRegister = binding.shaderRegister;
      range.RegisterSpace = binding.registerSpace;
      range.OffsetInDescriptorsFromTableStart = offset;
      (sampler ? samplerRanges : resourceRanges).push_back(range);
    }

    const auto addTable =
      [&](std::vector<D3D12_DESCRIPTOR_RANGE>& ranges, bool sampler)
      {
        if(ranges.empty())
          return;
        rangeGroups.push_back(std::move(ranges));
        const uint32_t rootIndex = static_cast<uint32_t>(parameters.size());
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges =
          static_cast<UINT>(rangeGroups.back().size());
        parameter.DescriptorTable.pDescriptorRanges = rangeGroups.back().data();
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters.push_back(parameter);
        auto& table = result.tables[logicalSet];
        (sampler ? table.samplerParameters
                 : table.resourceParameters).push_back(rootIndex);
      };
    addTable(resourceRanges, false);
    addTable(samplerRanges, true);
  }

  const auto pushBinding = std::find_if(
    shader.bindings.begin(), shader.bindings.end(),
    [](const D3D12ShaderBinding& binding)
    {
      return binding.kind == D3D12ShaderBindingKind::pushConstants;
    });
  if(pushBinding != shader.bindings.end())
  {
    uint32_t totalSize = 0;
    std::vector<std::pair<const RootBindingDesc*, uint32_t>> constantBindings;
    for(uint32_t index = 0; index < static_cast<uint32_t>(bindingSchema.rootBindings.size()); ++index)
    {
      const RootBindingDesc& binding = bindingSchema.rootBindings[index];
      if(binding.kind == RootBindingKind::dynamicBuffer ||
         (static_cast<uint32_t>(binding.visibility) &
          static_cast<uint32_t>(stageMask)) == 0)
        continue;
      if(binding.size == 0 || (binding.size & 3u) != 0)
        throw std::runtime_error("DX12 root constants require a 4-byte size");
      const uint32_t alignment = std::max(4u, binding.alignment);
      const uint32_t offset = (totalSize + alignment - 1u) & ~(alignment - 1u);
      constantBindings.emplace_back(&binding, offset);
      totalSize = offset + binding.size;
    }
    if(!constantBindings.empty())
    {
      const uint32_t rootIndex = static_cast<uint32_t>(parameters.size());
      D3D12_ROOT_PARAMETER parameter{};
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      parameter.Constants.ShaderRegister = pushBinding->shaderRegister;
      parameter.Constants.RegisterSpace = pushBinding->registerSpace;
      parameter.Constants.Num32BitValues = totalSize / 4u;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      parameters.push_back(parameter);
      for(const auto& [binding, offset] : constantBindings)
        result.roots[binding->slot].push_back({rootIndex, offset, binding->size});
    }
  }

  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = static_cast<UINT>(parameters.size());
  desc.pParameters = parameters.data();
  desc.Flags = graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                        : D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ID3DBlob* serialized = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT serializeResult = D3D12SerializeRootSignature(
    &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if(FAILED(serializeResult))
  {
    std::string message = "D3D12SerializeRootSignature failed";
    if(errors != nullptr && errors->GetBufferPointer() != nullptr)
      message += ": " + std::string(
        static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    if(errors != nullptr)
      errors->Release();
    if(serialized != nullptr)
      serialized->Release();
    throw std::runtime_error(message);
  }
  if(errors != nullptr)
    errors->Release();
  checkHresult(device->CreateRootSignature(
                 0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                 IID_PPV_ARGS(&result.signature)),
               "ID3D12Device::CreateRootSignature");
  serialized->Release();
  return result;
}

}  // namespace

ShaderLibraryHandle D3D12Device::createShaderLibrary(const ShaderLibraryDesc& desc)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createShaderLibrary called before initialization");
  if(desc.data.empty())
    throw std::runtime_error("D3D12 shader library requires a non-empty payload");
  if(desc.format != ShaderIRFormat::spirv && desc.format != ShaderIRFormat::dxil)
    throw std::runtime_error("D3D12 shader library requires SPIR-V identity or DXIL");

  if(m_nextShaderLibraryIndex == 0)
    throw std::runtime_error("D3D12 shader library handle space exhausted");
  const uint32_t index = m_nextShaderLibraryIndex++;
  const auto* begin = reinterpret_cast<const uint8_t*>(desc.data.data());
  ShaderLibraryRecord record{};
  record.format = desc.format;
  record.bytes.assign(begin, begin + desc.data.size());
  m_shaderLibraries.emplace(index, std::move(record));
  return {index, m_handleGeneration};
}

void D3D12Device::destroyShaderLibrary(ShaderLibraryHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  m_shaderLibraries.erase(handle.index);
}

PipelineHandle D3D12Device::createComputePipeline(const ComputePipelineDesc& desc)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::pipelineCreations);
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createComputePipeline called before initialization");
  if(desc.shaderStage.stage != ShaderStage::compute)
    throw std::runtime_error("D3D12 compute pipeline requires a compute shader");
  const PipelineBindingSchemaValidationResult validation =
    validatePipelineBindingSchema(desc.bindingSchema);
  if(!validation.valid())
    throw std::runtime_error("D3D12 compute pipeline received an invalid binding schema");

  std::vector<LayoutPlacement> placements;
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.bindingSchema.argumentSlots.size()); ++index)
  {
    const PipelineArgumentSlotDesc& slot = desc.bindingSchema.argumentSlots[index];
    const auto layoutIt = m_argumentLayouts.find(slot.layout.index);
    if(slot.layout.generation != m_handleGeneration || layoutIt == m_argumentLayouts.end())
      throw std::runtime_error("D3D12 compute pipeline received a stale argument layout");
    for(const ArgumentBindingPlacement& placement : layoutIt->second.bindings)
    {
      placements.push_back({
        slot.slot, placement.binding.binding, placement.binding.arrayCount,
        placement.binding.dynamicOffset, placement.resourceOffset,
        placement.samplerOffset});
    }
  }
  const auto libraryIt = m_shaderLibraries.find(desc.shaderStage.library.index);
  if(desc.shaderStage.library.generation != m_handleGeneration || libraryIt == m_shaderLibraries.end())
    throw std::runtime_error("D3D12 compute pipeline received a stale shader library");
  if(libraryIt->second.format != ShaderIRFormat::spirv)
    throw std::runtime_error("D3D12 runtime shader compilation currently requires a SPIR-V identity library");
  D3D12CompiledShader shader = compileShaderToDxil(
    desc.shaderStage, std::span<const uint8_t>{libraryIt->second.bytes});
  RootBuildResult root = buildRootSignature(
    static_cast<ID3D12Device*>(m_d3d12Device), shader, placements,
    desc.bindingSchema, ShaderStage::compute, false);

  D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
  native.pRootSignature = root.signature;
  native.CS = {shader.bytecode.data(), shader.bytecode.size()};

  ID3D12PipelineState* pipeline = nullptr;
  try
  {
    checkHresult(static_cast<ID3D12Device*>(m_d3d12Device)->CreateComputePipelineState(
                   &native, IID_PPV_ARGS(&pipeline)),
                 "ID3D12Device::CreateComputePipelineState");
  }
  catch(...)
  {
    root.signature->Release();
    throw;
  }

  if(m_nextPipelineIndex == 0)
  {
    pipeline->Release();
    root.signature->Release();
    throw std::runtime_error("D3D12 pipeline handle space exhausted");
  }
  const uint32_t index = m_nextPipelineIndex++;
  PipelineRecord record{};
  record.pipelineState = pipeline;
  record.rootSignature = root.signature;
  record.compute = true;
  record.tables = std::move(root.tables);
  record.rootParameters = std::move(root.roots);
  m_pipelines.emplace(index, std::move(record));
  return {index, m_handleGeneration};
}

PipelineHandle D3D12Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::pipelineCreations);
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createGraphicsPipeline called before initialization");
  if(desc.shaderStages.empty())
    throw std::runtime_error("D3D12 graphics pipeline requires shader stages");
  const PipelineBindingSchemaValidationResult validation =
    validatePipelineBindingSchema(desc.bindingSchema);
  if(!validation.valid())
    throw std::runtime_error("D3D12 graphics pipeline received an invalid binding schema");
  if(static_cast<uint32_t>(desc.renderingInfo.colorFormats.size()) > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
    throw std::runtime_error("D3D12 graphics pipeline has too many render targets");

  std::vector<LayoutPlacement> placements;
  const auto appendLayout = [&](uint32_t slot, ArgumentLayoutHandle handle)
  {
    const auto layoutIt = m_argumentLayouts.find(handle.index);
    if(handle.generation != m_handleGeneration || layoutIt == m_argumentLayouts.end())
      throw std::runtime_error("D3D12 graphics pipeline received a stale argument layout");
    for(const ArgumentBindingPlacement& placement : layoutIt->second.bindings)
    {
      placements.push_back({
        slot, placement.binding.binding, placement.binding.arrayCount,
        placement.binding.dynamicOffset, placement.resourceOffset,
        placement.samplerOffset});
    }
  };
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.bindingSchema.argumentSlots.size()); ++index)
  {
    const PipelineArgumentSlotDesc& slot = desc.bindingSchema.argumentSlots[index];
    appendLayout(slot.slot, slot.layout);
  }

  std::vector<D3D12CompiledShader> shaders;
  shaders.reserve(static_cast<uint32_t>(desc.shaderStages.size()));
  D3D12CompiledShader merged{};
  const D3D12CompiledShader* vertexShader = nullptr;
  const D3D12CompiledShader* fragmentShader = nullptr;
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.shaderStages.size()); ++index)
  {
    const ShaderEntry& entry = desc.shaderStages[index];
    const auto libraryIt = m_shaderLibraries.find(entry.library.index);
    if(entry.library.generation != m_handleGeneration || libraryIt == m_shaderLibraries.end())
      throw std::runtime_error("D3D12 graphics pipeline received a stale shader library");
    if(libraryIt->second.format != ShaderIRFormat::spirv)
      throw std::runtime_error("D3D12 runtime shader compilation currently requires a SPIR-V identity library");
    shaders.push_back(compileShaderToDxil(
      entry, std::span<const uint8_t>{libraryIt->second.bytes}));
    D3D12CompiledShader& shader = shaders.back();
    for(const D3D12ShaderBinding& binding : shader.bindings)
    {
      const auto duplicate = std::find_if(
        merged.bindings.begin(), merged.bindings.end(),
        [&binding](const D3D12ShaderBinding& value)
        {
          return value.logicalSet == binding.logicalSet &&
                 value.logicalBinding == binding.logicalBinding &&
                 value.kind == binding.kind &&
                 value.shaderRegister == binding.shaderRegister &&
                 value.registerSpace == binding.registerSpace;
        });
      if(duplicate == merged.bindings.end())
        merged.bindings.push_back(binding);
    }
    if(desc.shaderStages[index].stage == ShaderStage::vertex)
    {
      vertexShader = &shader;
      merged.vertexInputs = shader.vertexInputs;
    }
    else if(desc.shaderStages[index].stage == ShaderStage::fragment)
      fragmentShader = &shader;
  }
  if(vertexShader == nullptr)
    throw std::runtime_error("D3D12 graphics pipeline requires a vertex shader");

  RootBuildResult root = buildRootSignature(
    static_cast<ID3D12Device*>(m_d3d12Device), merged, placements,
    desc.bindingSchema, ShaderStage::allGraphics, true);

  std::vector<std::string> semanticNames;
  std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
  semanticNames.reserve(static_cast<uint32_t>(desc.vertexInput.attributes.size()));
  inputElements.reserve(static_cast<uint32_t>(desc.vertexInput.attributes.size()));
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.vertexInput.attributes.size()); ++index)
  {
    const VertexAttributeDesc& attribute = desc.vertexInput.attributes[index];
    const auto reflected = std::find_if(
      vertexShader->vertexInputs.begin(), vertexShader->vertexInputs.end(),
      [&attribute](const D3D12VertexInput& input)
      {
        return input.location == attribute.location;
      });
    if(reflected == vertexShader->vertexInputs.end())
    {
      root.signature->Release();
      throw std::runtime_error("DX12 vertex reflection is missing an RHI location");
    }
    const auto binding = std::find_if(
      desc.vertexInput.bindings.begin(),
      desc.vertexInput.bindings.end(),
      [&attribute](const VertexBindingDesc& value)
      {
        return value.binding == attribute.binding;
      });
    if(binding == desc.vertexInput.bindings.end())
    {
      root.signature->Release();
      throw std::runtime_error("DX12 vertex attribute references a missing binding");
    }
    semanticNames.push_back(reflected->semanticName);
    inputElements.push_back({
      semanticNames.back().c_str(), reflected->semanticIndex,
      toVertexFormat(attribute.format), attribute.binding, attribute.offset,
      binding->inputRate == VertexInputRate::perInstance
        ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
        : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      binding->inputRate == VertexInputRate::perInstance ? 1u : 0u});
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
  native.pRootSignature = root.signature;
  native.VS = {vertexShader->bytecode.data(), vertexShader->bytecode.size()};
  if(fragmentShader != nullptr)
    native.PS = {fragmentShader->bytecode.data(), fragmentShader->bytecode.size()};

  native.BlendState.IndependentBlendEnable = TRUE;
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.renderingInfo.colorFormats.size()); ++index)
  {
    const BlendAttachmentState source =
      index < static_cast<uint32_t>(desc.blendStates.size()) ? desc.blendStates[index]
                                   : BlendAttachmentState{};
    D3D12_RENDER_TARGET_BLEND_DESC& target = native.BlendState.RenderTarget[index];
    target.BlendEnable = source.blendEnable;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = toBlend(source.srcColorBlendFactor);
    target.DestBlend = toBlend(source.dstColorBlendFactor);
    target.BlendOp = toBlendOp(source.colorBlendOp);
    target.SrcBlendAlpha = toBlend(source.srcAlphaBlendFactor);
    target.DestBlendAlpha = toBlend(source.dstAlphaBlendFactor);
    target.BlendOpAlpha = toBlendOp(source.alphaBlendOp);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = toColorMask(source.colorWriteMask);
  }

  native.SampleMask = UINT_MAX;
  native.RasterizerState.FillMode =
    desc.rasterState.polygonMode == PolygonMode::line
      ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  native.RasterizerState.CullMode =
    desc.rasterState.cullMode == CullMode::front ? D3D12_CULL_MODE_FRONT :
    desc.rasterState.cullMode == CullMode::back ? D3D12_CULL_MODE_BACK :
                                                  D3D12_CULL_MODE_NONE;

  native.RasterizerState.FrontCounterClockwise =
    desc.rasterState.frontFace == FrontFace::counterClockwise;
  native.RasterizerState.DepthBias =
    static_cast<INT>(desc.rasterState.depthBiasConstantFactor);
  native.RasterizerState.DepthBiasClamp = desc.rasterState.depthBiasClamp;
  native.RasterizerState.SlopeScaledDepthBias = desc.rasterState.depthBiasSlopeFactor;
  native.RasterizerState.DepthClipEnable = TRUE;
  native.RasterizerState.MultisampleEnable =
    desc.rasterState.sampleCount != SampleCount::count1;
  native.RasterizerState.AntialiasedLineEnable = FALSE;
  native.RasterizerState.ForcedSampleCount = 0;
  native.RasterizerState.ConservativeRaster =
    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

  native.DepthStencilState.DepthEnable = desc.depthState.depthTestEnable;
  native.DepthStencilState.DepthWriteMask = desc.depthState.depthWriteEnable
    ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  native.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(
    static_cast<uint32_t>(desc.depthState.depthCompareOp) + 1u);
  native.DepthStencilState.StencilEnable = FALSE;
  native.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  native.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  native.DepthStencilState.FrontFace = {
    D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
    D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  native.DepthStencilState.BackFace = native.DepthStencilState.FrontFace;

  native.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};
  native.PrimitiveTopologyType = toTopologyType(desc.rasterState.topology);
  native.NumRenderTargets = static_cast<uint32_t>(desc.renderingInfo.colorFormats.size());
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.renderingInfo.colorFormats.size()); ++index)
    native.RTVFormats[index] = toDxgiFormat(desc.renderingInfo.colorFormats[index]);
  native.DSVFormat = toDxgiFormat(desc.renderingInfo.depthFormat);
  native.SampleDesc.Count = static_cast<UINT>(desc.rasterState.sampleCount);
  native.SampleDesc.Quality = 0;
  native.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

  ID3D12PipelineState* pipeline = nullptr;
  try
  {
    checkHresult(static_cast<ID3D12Device*>(m_d3d12Device)->
                   CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&pipeline)),
                 "ID3D12Device::CreateGraphicsPipelineState");
  }
  catch(...)
  {
    root.signature->Release();
    throw;
  }

  if(m_nextPipelineIndex == 0)
  {
    pipeline->Release();
    root.signature->Release();
    throw std::runtime_error("D3D12 pipeline handle space exhausted");
  }

  const uint32_t index = m_nextPipelineIndex++;
  PipelineRecord record{};
  record.pipelineState = pipeline;
  record.rootSignature = root.signature;
  record.compute = false;
  record.primitiveTopology =
    static_cast<uint32_t>(toTopology(desc.rasterState.topology));
  record.tables = std::move(root.tables);
  record.rootParameters = std::move(root.roots);
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.vertexInput.bindings.size()); ++index)
  {
    const VertexBindingDesc& binding = desc.vertexInput.bindings[index];
    if(record.vertexStrides.size() <= binding.binding)
      record.vertexStrides.resize(binding.binding + 1u);
    record.vertexStrides[binding.binding] = binding.stride;
  }
  const auto drawIndexRoot = record.rootParameters.find(0u);
  if(drawIndexRoot != record.rootParameters.end() &&
     !drawIndexRoot->second.empty())
  {
    const PipelineRecord::RootParameterBinding& drawIndexBinding =
      drawIndexRoot->second.front();
    if(drawIndexBinding.size < sizeof(uint32_t) ||
       (drawIndexBinding.destinationOffset & 3u) != 0)
    {
      pipeline->Release();
      root.signature->Release();
      throw std::runtime_error(
        "D3D12 MDI draw-index root constant has an invalid layout");
    }

    std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> arguments{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = drawIndexBinding.rootParameter;
    arguments[0].Constant.DestOffsetIn32BitValues =
      drawIndexBinding.destinationOffset / sizeof(uint32_t);
    arguments[0].Constant.Num32BitValuesToSet = 1;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{
      .ByteStride = sizeof(uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
      .NumArgumentDescs = static_cast<UINT>(arguments.size()),
      .pArgumentDescs = arguments.data(),
      .NodeMask = 0,
    };
    ID3D12CommandSignature* indirectSignature = nullptr;
    try
    {
      checkHresult(
        static_cast<ID3D12Device*>(m_d3d12Device)->CreateCommandSignature(
          &signatureDesc,
          static_cast<ID3D12RootSignature*>(record.rootSignature),
          IID_PPV_ARGS(&indirectSignature)),
        "ID3D12Device::CreateCommandSignature(MDI draw index)");
    }
    catch(...)
    {
      pipeline->Release();
      root.signature->Release();
      throw;
    }
    record.indexedIndirectSignature = indirectSignature;
  }
  m_pipelines.emplace(index, std::move(record));
  return {index, m_handleGeneration};
}


void D3D12Device::destroyPipeline(PipelineHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  const auto found = m_pipelines.find(handle.index);
  if(found == m_pipelines.end())
    return;
  const SubmissionTokenSet dependencies = found->second.pendingUses;
  m_retiredPipelines.push_back(
    RetiredPipeline{std::move(found->second), dependencies});
  m_pipelines.erase(found);
}

void D3D12Device::bindPipeline(void* commandListValue, PipelineHandle handle,
                               bool compute) const
{
  if(commandListValue == nullptr || handle.isNull() || handle.generation != m_handleGeneration)
    throw std::runtime_error("D3D12 pipeline bind received invalid arguments");
  const auto found = m_pipelines.find(handle.index);
  if(found == m_pipelines.end() || found->second.compute != compute)
    throw std::runtime_error("D3D12 pipeline bind received a stale or wrong-kind pipeline");

  auto* list = static_cast<ID3D12GraphicsCommandList*>(commandListValue);
  auto* root = static_cast<ID3D12RootSignature*>(found->second.rootSignature);
  if(compute)
    list->SetComputeRootSignature(root);
  else
  {
    list->SetGraphicsRootSignature(root);
    list->IASetPrimitiveTopology(
      static_cast<D3D12_PRIMITIVE_TOPOLOGY>(found->second.primitiveTopology));
  }
  list->SetPipelineState(static_cast<ID3D12PipelineState*>(found->second.pipelineState));
  std::array<ID3D12DescriptorHeap*, 2> heaps{
    static_cast<ID3D12DescriptorHeap*>(m_cbvSrvUavHeap),
    static_cast<ID3D12DescriptorHeap*>(m_samplerHeap),
  };
  list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
}

void D3D12Device::bindArgumentTable(
  void* commandListValue, PipelineHandle pipelineHandle, bool compute,
  uint32_t slot, ArgumentTableHandle tableHandle,
  const uint64_t* dynamicOffsets, uint32_t dynamicOffsetCount) const
{
  const auto pipelineIt = m_pipelines.find(pipelineHandle.index);
  const auto tableIt = m_argumentTables.find(tableHandle.index);
  if(pipelineHandle.generation != m_handleGeneration || pipelineIt == m_pipelines.end() ||
     tableHandle.generation != m_handleGeneration || tableIt == m_argumentTables.end())
    throw std::runtime_error("D3D12 argument-table bind received a stale handle");
  const auto bindingIt = pipelineIt->second.tables.find(slot);
  if(bindingIt == pipelineIt->second.tables.end())
    return;

  auto* list = static_cast<ID3D12GraphicsCommandList*>(commandListValue);
  const PipelineTableBinding& binding = bindingIt->second;
  const uint64_t resourceGpu = resolveArgumentTableResourceGpu(tableHandle);
  for(uint32_t root : binding.resourceParameters)
  {
    if(compute)
      list->SetComputeRootDescriptorTable(root, {resourceGpu});
    else
      list->SetGraphicsRootDescriptorTable(root, {resourceGpu});
  }
  const uint64_t samplerGpu = resolveArgumentTableSamplerGpu(tableHandle);
  for(uint32_t root : binding.samplerParameters)
  {
    if(compute)
      list->SetComputeRootDescriptorTable(root, {samplerGpu});
    else
      list->SetGraphicsRootDescriptorTable(root, {samplerGpu});
  }

  const auto layoutIt = m_argumentLayouts.find(tableIt->second.layout.index);
  if(layoutIt == m_argumentLayouts.end())
    throw std::runtime_error("D3D12 argument table refers to a stale layout");
  std::vector<uint32_t> dynamicBindings;
  for(const ArgumentBindingPlacement& placement : layoutIt->second.bindings)
  {
    if(placement.binding.dynamicOffset)
      dynamicBindings.push_back(placement.binding.binding);
  }

  for(const PipelineTableBinding::DynamicBinding& dynamic : binding.dynamicBindings)
  {
    const auto indexIt = std::find(dynamicBindings.begin(), dynamicBindings.end(),
                                  dynamic.binding);
    if(indexIt == dynamicBindings.end())
      throw std::runtime_error("D3D12 dynamic root descriptor has no layout binding");
    const uint32_t offsetIndex =
      static_cast<uint32_t>(std::distance(dynamicBindings.begin(), indexIt));
    const uint64_t dynamicOffset =
      dynamicOffsets != nullptr && offsetIndex < dynamicOffsetCount
        ? dynamicOffsets[offsetIndex] : 0;
    const auto valueIt = tableIt->second.buffers.find(dynamic.binding);
    if(valueIt == tableIt->second.buffers.end())
      throw std::runtime_error("D3D12 dynamic binding has not been written");
    const auto bufferIt = m_buffers.find(valueIt->second.buffer.index);
    if(valueIt->second.buffer.generation != m_handleGeneration || bufferIt == m_buffers.end())
      throw std::runtime_error("D3D12 dynamic binding refers to a stale buffer");
    const D3D12_GPU_VIRTUAL_ADDRESS address =
      static_cast<ID3D12Resource*>(bufferIt->second.resource)->GetGPUVirtualAddress() +
      valueIt->second.offset + dynamicOffset;
    if(compute)
    {
      if(dynamic.kind == static_cast<uint32_t>(D3D12ShaderBindingKind::shaderResource))
        list->SetComputeRootShaderResourceView(dynamic.rootParameter, address);
      else if(dynamic.kind == static_cast<uint32_t>(D3D12ShaderBindingKind::unorderedAccess))
        list->SetComputeRootUnorderedAccessView(dynamic.rootParameter, address);
      else
        list->SetComputeRootConstantBufferView(dynamic.rootParameter, address);
    }
    else
    {
      if(dynamic.kind == static_cast<uint32_t>(D3D12ShaderBindingKind::shaderResource))
        list->SetGraphicsRootShaderResourceView(dynamic.rootParameter, address);
      else if(dynamic.kind == static_cast<uint32_t>(D3D12ShaderBindingKind::unorderedAccess))
        list->SetGraphicsRootUnorderedAccessView(dynamic.rootParameter, address);
      else
        list->SetGraphicsRootConstantBufferView(dynamic.rootParameter, address);
    }
  }
}

void D3D12Device::bindRootConstants(
  void* commandListValue, PipelineHandle pipelineHandle, bool compute,
  uint32_t slot, const void* data, uint32_t size) const
{
  if(data == nullptr || size == 0 || (size & 3u) != 0)
    throw std::runtime_error("D3D12 root constants require 4-byte data");
  const auto pipelineIt = m_pipelines.find(pipelineHandle.index);
  if(pipelineHandle.generation != m_handleGeneration || pipelineIt == m_pipelines.end())
    throw std::runtime_error("D3D12 root constants received a stale pipeline");
  const auto rootIt = pipelineIt->second.rootParameters.find(slot);
  if(rootIt == pipelineIt->second.rootParameters.end())
    throw std::runtime_error("D3D12 pipeline has no root-constant slot");

  auto* list = static_cast<ID3D12GraphicsCommandList*>(commandListValue);
  for(const PipelineRecord::RootParameterBinding& binding : rootIt->second)
  {
    if(size > binding.size)
      throw std::runtime_error("D3D12 root-constant write exceeds the declared slot size");
    if(compute)
      list->SetComputeRoot32BitConstants(binding.rootParameter, size / 4u, data,
                                         binding.destinationOffset / 4u);
    else
      list->SetGraphicsRoot32BitConstants(binding.rootParameter, size / 4u, data,
                                          binding.destinationOffset / 4u);
  }
}

uint32_t D3D12Device::vertexStride(PipelineHandle pipelineHandle,
                                   uint32_t binding) const
{
  const auto found = m_pipelines.find(pipelineHandle.index);
  if(pipelineHandle.generation != m_handleGeneration || found == m_pipelines.end() ||
     binding >= found->second.vertexStrides.size())
    return 0;
  return found->second.vertexStrides[binding];
}

void* D3D12Device::drawIndirectSignature(
  bool indexed, PipelineHandle pipelineHandle, uint32_t stride) const
{
  if(!indexed)
    return m_drawSignature;

  if(stride == 0 || stride == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
    return m_drawIndexedSignature;
  if(stride != sizeof(uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
    return nullptr;

  const auto found = m_pipelines.find(pipelineHandle.index);
  if(pipelineHandle.generation != m_handleGeneration || found == m_pipelines.end() ||
     found->second.compute)
    return nullptr;
  return found->second.indexedIndirectSignature;
}

void* D3D12Device::dispatchIndirectSignature() const
{
  return m_dispatchSignature;
}

}  // namespace demo::rhi::d3d12
