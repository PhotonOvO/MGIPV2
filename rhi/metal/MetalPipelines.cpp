#include "MetalPipelines.h"

#include "MetalBlitSource.h"
#include "MetalCommon.h"

#include <Metal/MTL4Compiler.hpp>
#include <Metal/MTL4ComputePipeline.hpp>
#include <Metal/MTL4LibraryDescriptor.hpp>
#include <Metal/MTL4LibraryFunctionDescriptor.hpp>
#include <Metal/MTL4PipelineState.hpp>
#include <Metal/MTL4RenderPipeline.hpp>
#include <Metal/MTL4SpecializedFunctionDescriptor.hpp>
#include <Metal/MTLComputePipeline.hpp>
#include <Metal/MTLDepthStencil.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLFunctionConstantValues.hpp>
#include <Metal/MTLLibrary.hpp>
#include <Metal/MTLRenderPipeline.hpp>
#include <Metal/MTLVertexDescriptor.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace demo::rhi::metal {
namespace {

struct FunctionDescriptorOwner
{
  MTL4::LibraryFunctionDescriptor* base{nullptr};
  MTL4::SpecializedFunctionDescriptor* specialized{nullptr};
  MTL::FunctionConstantValues* constants{nullptr};

  FunctionDescriptorOwner() = default;
  FunctionDescriptorOwner(const FunctionDescriptorOwner&) = delete;
  FunctionDescriptorOwner& operator=(const FunctionDescriptorOwner&) = delete;
  FunctionDescriptorOwner(FunctionDescriptorOwner&& other) noexcept
    : base(other.base), specialized(other.specialized), constants(other.constants)
  {
    other.base = nullptr;
    other.specialized = nullptr;
    other.constants = nullptr;
  }
  FunctionDescriptorOwner& operator=(FunctionDescriptorOwner&& other) noexcept
  {
    if(this == &other)
      return *this;
    if(specialized != nullptr) specialized->release();
    if(constants != nullptr) constants->release();
    if(base != nullptr) base->release();
    base = other.base;
    specialized = other.specialized;
    constants = other.constants;
    other.base = nullptr;
    other.specialized = nullptr;
    other.constants = nullptr;
    return *this;
  }
  ~FunctionDescriptorOwner()
  {
    if(specialized != nullptr) specialized->release();
    if(constants != nullptr) constants->release();
    if(base != nullptr) base->release();
  }

  [[nodiscard]] MTL4::FunctionDescriptor* get() const
  {
    return specialized != nullptr
      ? static_cast<MTL4::FunctionDescriptor*>(specialized)
      : static_cast<MTL4::FunctionDescriptor*>(base);
  }
};

MTL::DataType specializationType(const SpecializationConstant& constant)
{
  if(constant.type == SpecializationConstantType::boolean)
    return MTL::DataTypeBool;

  switch(constant.type)
  {
  case SpecializationConstantType::signedInteger:
    switch(constant.size)
    {
    case 1: return MTL::DataTypeChar;
    case 2: return MTL::DataTypeShort;
    case 4: return MTL::DataTypeInt;
    case 8: return MTL::DataTypeLong;
    }
    break;
  case SpecializationConstantType::unsignedInteger:
    switch(constant.size)
    {
    case 1: return MTL::DataTypeUChar;
    case 2: return MTL::DataTypeUShort;
    case 4: return MTL::DataTypeUInt;
    case 8: return MTL::DataTypeULong;
    }
    break;
  case SpecializationConstantType::floatingPoint:
    switch(constant.size)
    {
    case 2: return MTL::DataTypeHalf;
    case 4: return MTL::DataTypeFloat;
    }
    break;
  case SpecializationConstantType::boolean:
    break;
  }
  throw std::invalid_argument("Metal specialization constant has an unsupported type or size");
}

FunctionDescriptorOwner makeFunctionDescriptor(
  const ShaderEntry& entry, const MetalResourceTable& resources)
{
  const MetalShaderLibraryRecord* library =
    resources.tryGetShaderLibrary(entry.library);
  if(library == nullptr || library->nativeLibrary == nullptr)
    throw std::invalid_argument("Metal pipeline references a stale shader library");
  if(entry.entryPoint.empty())
    throw std::invalid_argument("Metal shader entry point is empty");

  FunctionDescriptorOwner owner;
  owner.base = MTL4::LibraryFunctionDescriptor::alloc()->init();
  owner.base->setLibrary(library->nativeLibrary);
  owner.base->setName(NS::String::string(entry.entryPoint.data(),
                                         NS::UTF8StringEncoding));
  if(entry.specializationConstants.empty())
    return owner;
  if(entry.specializationData.bytes.empty())
    throw std::invalid_argument("Metal specialization constants have no data");

  owner.constants = MTL::FunctionConstantValues::alloc()->init();
  for(const SpecializationConstant& constant : entry.specializationConstants)
  {
    if(constant.offset > entry.specializationData.bytes.size() ||
       constant.size > entry.specializationData.bytes.size() - constant.offset)
    {
      throw std::out_of_range("Metal specialization constant exceeds its data block");
    }
    owner.constants->setConstantValue(
      entry.specializationData.bytes.data() + constant.offset,
      specializationType(constant), constant.constantId);
  }
  owner.specialized = MTL4::SpecializedFunctionDescriptor::alloc()->init();
  owner.specialized->setFunctionDescriptor(owner.base);
  owner.specialized->setConstantValues(owner.constants);
  const std::string specializedName =
    std::string(entry.entryPoint) + "_variant_" +
    std::to_string(entry.specializationVariant);
  owner.specialized->setSpecializedName(nsString(specializedName.c_str()));
  return owner;
}

MTL::ColorWriteMask toMetalWriteMask(ColorComponentFlags mask)
{
  NS::UInteger result = MTL::ColorWriteMaskNone;
  const uint8_t value = static_cast<uint8_t>(mask);
  if((value & static_cast<uint8_t>(ColorComponentFlags::r)) != 0)
    result |= MTL::ColorWriteMaskRed;
  if((value & static_cast<uint8_t>(ColorComponentFlags::g)) != 0)
    result |= MTL::ColorWriteMaskGreen;
  if((value & static_cast<uint8_t>(ColorComponentFlags::b)) != 0)
    result |= MTL::ColorWriteMaskBlue;
  if((value & static_cast<uint8_t>(ColorComponentFlags::a)) != 0)
    result |= MTL::ColorWriteMaskAlpha;
  return static_cast<MTL::ColorWriteMask>(result);
}

MTL::VertexDescriptor* makeVertexDescriptor(
  const VertexInputLayoutDesc& input, const MetalPipelineBindingLayout& bindings)
{
  if(input.bindings.empty() && input.attributes.empty())
    return nullptr;
  auto* descriptor = MTL::VertexDescriptor::alloc()->init();
  for(const VertexBindingDesc& binding : input.bindings)
  {
    const MetalVertexPlacement* placement = bindings.findVertex(binding.binding);
    if(placement == nullptr)
    {
      descriptor->release();
      throw std::runtime_error("Metal vertex layout has no argument-table placement");
    }
    MTL::VertexBufferLayoutDescriptor* layout =
      descriptor->layouts()->object(placement->bufferIndex);
    layout->setStride(binding.stride);
    layout->setStepFunction(binding.inputRate == VertexInputRate::perInstance
                              ? MTL::VertexStepFunctionPerInstance
                              : MTL::VertexStepFunctionPerVertex);
    layout->setStepRate(1);
  }
  for(const VertexAttributeDesc& attribute : input.attributes)
  {
    const MetalVertexPlacement* placement = bindings.findVertex(attribute.binding);
    if(placement == nullptr)
    {
      descriptor->release();
      throw std::runtime_error("Metal vertex attribute references an unknown binding");
    }
    MTL::VertexAttributeDescriptor* nativeAttribute =
      descriptor->attributes()->object(attribute.location);
    nativeAttribute->setFormat(toMetalVertexFormat(attribute.format));
    nativeAttribute->setOffset(attribute.offset);
    nativeAttribute->setBufferIndex(placement->bufferIndex);
  }
  return descriptor;
}

void configurePipelineOptions(
  MTL4::PipelineDescriptor& descriptor, bool enableShaderValidation)
{
  auto* options = MTL4::PipelineOptions::alloc()->init();
  options->setShaderReflection(MTL4::ShaderReflectionBindingInfo);
  options->setShaderValidation(enableShaderValidation
                                 ? MTL::ShaderValidationEnabled
                                 : MTL::ShaderValidationDefault);
  descriptor.setOptions(options);
  options->release();
}

}  // namespace

MetalPipelineBindingLayout buildMetalPipelineBindingLayout(
  const PipelineBindingSchemaDesc& schema, const VertexInputLayoutDesc& vertexInput,
  const MetalResourceTable& resources)
{
  const PipelineBindingSchemaValidationResult validation =
    validatePipelineBindingSchema(schema);
  if(!validation.valid())
    throw std::invalid_argument("Metal pipeline binding schema is invalid");

  MetalPipelineBindingLayout result;
  std::vector<PipelineArgumentSlotDesc> slots(schema.argumentSlots.begin(),
                                               schema.argumentSlots.end());
  std::sort(slots.begin(), slots.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.slot < rhs.slot; });
  for(const PipelineArgumentSlotDesc& slot : slots)
  {
    const MetalArgumentLayoutRecord* layout =
      resources.tryGetArgumentLayout(slot.layout);
    if(layout == nullptr)
      throw std::invalid_argument("Metal pipeline references a stale argument layout");
    std::vector<ArgumentBinding> arguments = layout->bindings;
    std::sort(arguments.begin(), arguments.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.binding < rhs.binding; });
    for(const ArgumentBinding& binding : arguments)
    {
      MetalArgumentPlacement placement{
        .tableSlot = slot.slot,
        .layout = slot.layout,
        .binding = binding.binding,
        .type = binding.type,
        .visibility = slot.visibility,
        .arrayCount = binding.arrayCount,
        .dynamicOffset = binding.dynamicOffset,
      };
      switch(binding.type)
      {
      case ArgumentType::uniformBuffer:
      case ArgumentType::storageBuffer:
        placement.bufferIndex = result.bufferCount;
        result.bufferCount += binding.arrayCount;
        break;
      case ArgumentType::sampledTexture:
      case ArgumentType::storageTexture:
        placement.textureIndex = result.textureCount;
        result.textureCount += binding.arrayCount;
        break;
      case ArgumentType::sampler:
        placement.samplerIndex = result.samplerCount;
        result.samplerCount += binding.arrayCount;
        break;
      case ArgumentType::combinedImageSampler:
        placement.textureIndex = result.textureCount;
        placement.samplerIndex = result.samplerCount;
        result.textureCount += binding.arrayCount;
        result.samplerCount += binding.arrayCount;
        break;
      }
      result.stages |= slot.visibility;
      result.arguments.push_back(placement);
    }
  }

  std::vector<RootBindingDesc> roots(schema.rootBindings.begin(),
                                     schema.rootBindings.end());
  std::sort(roots.begin(), roots.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.slot < rhs.slot; });
  for(const RootBindingDesc& root : roots)
  {
    result.roots.push_back(MetalRootPlacement{
      .slot = root.slot,
      .kind = root.kind,
      .visibility = root.visibility,
      .size = root.size,
      .alignment = root.alignment,
      .dynamicBuffer = root.dynamicBuffer,
      .bufferIndex = result.bufferCount++,
    });
    result.stages |= root.visibility;
  }

  std::vector<VertexBindingDesc> vertexBindings(vertexInput.bindings.begin(),
                                                 vertexInput.bindings.end());
  std::sort(vertexBindings.begin(), vertexBindings.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.binding < rhs.binding; });
  for(const VertexBindingDesc& binding : vertexBindings)
  {
    result.vertexBuffers.push_back(MetalVertexPlacement{
      .binding = binding.binding,
      .bufferIndex = result.bufferCount++,
      .stride = binding.stride,
    });
  }
  if(!vertexBindings.empty())
    result.stages |= ShaderStage::vertex;
  return result;
}

MetalPipelineRecord compileMetalGraphicsPipeline(
  MTL::Device& device, MTL4::Compiler& compiler, const GraphicsPipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation)
{
  const ShaderEntry* vertex = nullptr;
  const ShaderEntry* fragment = nullptr;
  for(const ShaderEntry& stage : desc.shaderStages)
  {
    if(stage.stage == ShaderStage::vertex && vertex == nullptr)
      vertex = &stage;
    else if(stage.stage == ShaderStage::fragment && fragment == nullptr)
      fragment = &stage;
    else
      throw std::invalid_argument("Metal graphics pipeline has duplicate or unsupported shader stages");
  }
  if(vertex == nullptr)
    throw std::invalid_argument("Metal graphics pipeline requires a vertex shader");

  MetalPipelineRecord result;
  result.kind = MetalPipelineKind::graphics;
  result.debugName = std::string(vertex->entryPoint);
  result.bindingLayout = buildMetalPipelineBindingLayout(
    desc.bindingSchema, desc.vertexInput, resources);
  result.rasterState = desc.rasterState;
  result.depthState = desc.depthState;
  result.specializationVariant = desc.specializationVariant;

  FunctionDescriptorOwner vertexFunction = makeFunctionDescriptor(*vertex, resources);
  FunctionDescriptorOwner fragmentFunction;
  if(fragment != nullptr)
    fragmentFunction = makeFunctionDescriptor(*fragment, resources);

  auto* pipelineDesc = MTL4::RenderPipelineDescriptor::alloc()->init();
  pipelineDesc->setLabel(nsString("Metal4 graphics pipeline"));
  pipelineDesc->setVertexFunctionDescriptor(vertexFunction.get());
  if(fragment != nullptr)
    pipelineDesc->setFragmentFunctionDescriptor(fragmentFunction.get());
  pipelineDesc->setInputPrimitiveTopology(
    toMetalTopologyClass(desc.rasterState.topology));
  pipelineDesc->setRasterSampleCount(static_cast<uint32_t>(desc.rasterState.sampleCount));
  pipelineDesc->setRasterizationEnabled(true);
  configurePipelineOptions(*pipelineDesc, enableShaderValidation);

  MTL::VertexDescriptor* vertexDesc =
    makeVertexDescriptor(desc.vertexInput, result.bindingLayout);
  if(vertexDesc != nullptr)
  {
    pipelineDesc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();
  }
  for(uint32_t index = 0; index < desc.renderingInfo.colorFormats.size(); ++index)
  {
    MTL4::RenderPipelineColorAttachmentDescriptor* attachment =
      pipelineDesc->colorAttachments()->object(index);
    attachment->setPixelFormat(toMetalPixelFormat(
      desc.renderingInfo.colorFormats[index]));
    const BlendAttachmentState blend = index < desc.blendStates.size()
      ? desc.blendStates[index]
      : BlendAttachmentState{};
    attachment->setBlendingState(blend.blendEnable
                                   ? MTL4::BlendStateEnabled
                                   : MTL4::BlendStateDisabled);
    attachment->setSourceRGBBlendFactor(
      toMetalBlendFactor(blend.srcColorBlendFactor));
    attachment->setDestinationRGBBlendFactor(
      toMetalBlendFactor(blend.dstColorBlendFactor));
    attachment->setRgbBlendOperation(toMetalBlendOperation(blend.colorBlendOp));
    attachment->setSourceAlphaBlendFactor(
      toMetalBlendFactor(blend.srcAlphaBlendFactor));
    attachment->setDestinationAlphaBlendFactor(
      toMetalBlendFactor(blend.dstAlphaBlendFactor));
    attachment->setAlphaBlendOperation(toMetalBlendOperation(blend.alphaBlendOp));
    attachment->setWriteMask(toMetalWriteMask(blend.colorWriteMask));
  }

  auto* taskOptions = MTL4::CompilerTaskOptions::alloc()->init();
  NS::Error* error = nullptr;
  result.nativeRenderPipeline = compiler.newRenderPipelineState(
    pipelineDesc, taskOptions, &error);
  taskOptions->release();
  pipelineDesc->release();
  if(result.nativeRenderPipeline == nullptr)
    throw std::runtime_error(errorMessage("MTL4::Compiler graphics pipeline", error));

  auto* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
  depthDesc->setDepthCompareFunction(desc.depthState.depthTestEnable
                                      ? toMetalCompare(desc.depthState.depthCompareOp)
                                      : MTL::CompareFunctionAlways);
  depthDesc->setDepthWriteEnabled(desc.depthState.depthWriteEnable);
  result.nativeDepthStencilState = device.newDepthStencilState(depthDesc);
  depthDesc->release();
  if(result.nativeDepthStencilState == nullptr)
  {
    result.nativeRenderPipeline->release();
    throw std::runtime_error("Metal failed to create depth-stencil state");
  }
  return result;
}

MetalPipelineRecord compileMetalComputePipeline(
  MTL4::Compiler& compiler, const ComputePipelineDesc& desc,
  const MetalResourceTable& resources, bool enableShaderValidation)
{
  if(desc.shaderStage.stage != ShaderStage::compute)
    throw std::invalid_argument("Metal compute pipeline requires a compute shader");
  if(desc.threadgroupSize.width == 0 || desc.threadgroupSize.height == 0 ||
     desc.threadgroupSize.depth == 0)
    throw std::invalid_argument("Metal compute pipeline threadgroup size is invalid");

  MetalPipelineRecord result;
  result.kind = MetalPipelineKind::compute;
  result.debugName = std::string(desc.shaderStage.entryPoint);
  result.bindingLayout = buildMetalPipelineBindingLayout(
    desc.bindingSchema, {}, resources);
  result.specializationVariant = desc.specializationVariant;
  result.computeThreadsPerThreadgroup = desc.threadgroupSize;

  FunctionDescriptorOwner function =
    makeFunctionDescriptor(desc.shaderStage, resources);
  auto* pipelineDesc = MTL4::ComputePipelineDescriptor::alloc()->init();
  pipelineDesc->setLabel(nsString("Metal4 compute pipeline"));
  pipelineDesc->setComputeFunctionDescriptor(function.get());
  const MTL::Size threads = MTL::Size::Make(
    desc.threadgroupSize.width, desc.threadgroupSize.height,
    desc.threadgroupSize.depth);
  pipelineDesc->setRequiredThreadsPerThreadgroup(threads);
  pipelineDesc->setMaxTotalThreadsPerThreadgroup(
    static_cast<uint64_t>(desc.threadgroupSize.width) *
    desc.threadgroupSize.height * desc.threadgroupSize.depth);
  configurePipelineOptions(*pipelineDesc, enableShaderValidation);

  auto* taskOptions = MTL4::CompilerTaskOptions::alloc()->init();
  NS::Error* error = nullptr;
  result.nativeComputePipeline = compiler.newComputePipelineState(
    pipelineDesc, taskOptions, &error);
  taskOptions->release();
  pipelineDesc->release();
  if(result.nativeComputePipeline == nullptr)
    throw std::runtime_error(errorMessage("MTL4::Compiler compute pipeline", error));
  result.computeThreadExecutionWidth =
    static_cast<uint32_t>(result.nativeComputePipeline->threadExecutionWidth());
  result.computeMaxThreadsPerThreadgroup =
    static_cast<uint32_t>(result.nativeComputePipeline->maxTotalThreadsPerThreadgroup());
  return result;
}

MTL::ComputePipelineState* compileMetalBlitPipeline(
  MTL4::Compiler& compiler, bool enableShaderValidation)
{
  auto* compileOptions = MTL::CompileOptions::alloc()->init();
  compileOptions->setLanguageVersion(MTL::LanguageVersion4_0);
  compileOptions->setMathMode(MTL::MathModeSafe);

  auto* libraryDesc = MTL4::LibraryDescriptor::alloc()->init();
  libraryDesc->setName(nsString("MGIP Metal4 compute blit library"));
  libraryDesc->setOptions(compileOptions);
  libraryDesc->setSource(nsString(kMetal4BlitSource));
  NS::Error* error = nullptr;
  MTL::Library* library = compiler.newLibrary(libraryDesc, &error);
  libraryDesc->release();
  compileOptions->release();
  if(library == nullptr)
    throw std::runtime_error(errorMessage("MTL4::Compiler blit library", error));

  auto* functionDesc = MTL4::LibraryFunctionDescriptor::alloc()->init();
  functionDesc->setLibrary(library);
  functionDesc->setName(nsString("mgipMetal4Blit2D"));

  auto* pipelineDesc = MTL4::ComputePipelineDescriptor::alloc()->init();
  pipelineDesc->setLabel(nsString("MGIP Metal4 compute blit pipeline"));
  pipelineDesc->setComputeFunctionDescriptor(functionDesc);
  pipelineDesc->setRequiredThreadsPerThreadgroup(MTL::Size::Make(8, 8, 1));
  pipelineDesc->setMaxTotalThreadsPerThreadgroup(64);
  configurePipelineOptions(*pipelineDesc, enableShaderValidation);

  auto* taskOptions = MTL4::CompilerTaskOptions::alloc()->init();
  MTL::ComputePipelineState* pipeline = compiler.newComputePipelineState(
    pipelineDesc, taskOptions, &error);
  taskOptions->release();
  pipelineDesc->release();
  functionDesc->release();
  library->release();
  if(pipeline == nullptr)
    throw std::runtime_error(errorMessage("MTL4::Compiler blit pipeline", error));
  return pipeline;
}

}  // namespace demo::rhi::metal
