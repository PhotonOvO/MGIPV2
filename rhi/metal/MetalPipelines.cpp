#include "MetalPipelines.h"

#include "_autogen/MetalBuiltins.metallib.h"
#include "MetalCommon.h"
#include "../../common/logger.h"

#include <Metal/MTL4Compiler.hpp>
#include <Metal/MTL4ComputePipeline.hpp>
#include <Metal/MTL4LibraryFunctionDescriptor.hpp>
#include <Metal/MTL4PipelineState.hpp>
#include <Metal/MTL4RenderPipeline.hpp>
#include <Metal/MTL4SpecializedFunctionDescriptor.hpp>
#include <Metal/MTLArgumentEncoder.hpp>
#include <Metal/MTLComputePipeline.hpp>
#include <Metal/MTLDepthStencil.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLFunctionConstantValues.hpp>
#include <Metal/MTLLibrary.hpp>
#include <Metal/MTLRenderPipeline.hpp>
#include <Metal/MTLVertexDescriptor.hpp>

#include <dispatch/dispatch.h>

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
  const ShaderEntry& entry, const MetalResourceTable& resources,
  bool useIcbLibrary = false)
{
  const MetalShaderLibraryRecord* library =
    resources.tryGetShaderLibrary(entry.library);
  if(library == nullptr || library->nativeLibrary == nullptr)
    throw std::invalid_argument("Metal pipeline references a stale shader library");
  if(entry.entryPoint.empty())
    throw std::invalid_argument("Metal shader entry point is empty");

  FunctionDescriptorOwner owner;
  MTL::Library* nativeLibrary = useIcbLibrary &&
                                library->icbLibrary != nullptr
    ? library->icbLibrary
    : library->nativeLibrary;
  owner.base = MTL4::LibraryFunctionDescriptor::alloc()->init();
  owner.base->setLibrary(nativeLibrary);
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
  MTL4::PipelineDescriptor& descriptor, bool enableShaderValidation,
  bool disableShaderValidation = false)
{
  auto* options = MTL4::PipelineOptions::alloc()->init();
  options->setShaderReflection(MTL4::ShaderReflectionBindingInfo);
  options->setShaderValidation(disableShaderValidation
                                 ? MTL::ShaderValidationDisabled
                                 : enableShaderValidation
                                   ? MTL::ShaderValidationEnabled
                                   : MTL::ShaderValidationDefault);
  descriptor.setOptions(options);
  options->release();
}

MTL::Function* makeMetalFunction(
  const ShaderEntry& entry, const FunctionDescriptorOwner& descriptor,
  const MetalResourceTable& resources, bool useIcbLibrary,
  std::string& failure)
{
  const MetalShaderLibraryRecord* library =
    resources.tryGetShaderLibrary(entry.library);
  if(library == nullptr || library->nativeLibrary == nullptr)
  {
    failure = "Metal argument-buffer function references a stale shader library";
    return nullptr;
  }

  MTL::Library* nativeLibrary = useIcbLibrary &&
                                library->icbLibrary != nullptr
    ? library->icbLibrary
    : library->nativeLibrary;
  NS::Error* error = nullptr;
  MTL::Function* function = descriptor.constants != nullptr
    ? nativeLibrary->newFunction(
        nsString(entry.entryPoint.data()), descriptor.constants, &error)
    : nativeLibrary->newFunction(nsString(entry.entryPoint.data()));
  if(function == nullptr)
  {
    failure = errorMessage("Metal argument-buffer function", error);
    failure += " (";
    failure += entry.entryPoint;
    failure += ')';
  }
  return function;
}

}  // namespace

MetalPipelineBindingLayout buildMetalPipelineBindingLayout(
  const PipelineBindingSchemaDesc& schema, const VertexInputLayoutDesc& vertexInput,
  const MetalResourceTable& resources, bool useNestedResourceArgumentBuffer)
{
  const PipelineBindingSchemaValidationResult validation =
    validatePipelineBindingSchema(schema);
  if(!validation.valid())
    throw std::invalid_argument("Metal pipeline binding schema is invalid");

  MetalPipelineBindingLayout result;
  uint32_t nestedArgumentCount = 0;
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
        if(useNestedResourceArgumentBuffer)
        {
          if(result.nestedArgumentBufferIndex == kInvalidMetalBinding)
            result.nestedArgumentBufferIndex = result.bufferCount++;
          placement.nestedArgumentIndex = nestedArgumentCount;
          nestedArgumentCount += binding.arrayCount;
        }
        else
        {
          placement.textureIndex = result.textureCount;
          result.textureCount += binding.arrayCount;
        }
        break;
      case ArgumentType::sampler:
        if(useNestedResourceArgumentBuffer)
        {
          if(result.nestedArgumentBufferIndex == kInvalidMetalBinding)
            result.nestedArgumentBufferIndex = result.bufferCount++;
          placement.nestedArgumentIndex = nestedArgumentCount;
          nestedArgumentCount += binding.arrayCount;
        }
        else
        {
          placement.samplerIndex = result.samplerCount;
          result.samplerCount += binding.arrayCount;
        }
        break;
      case ArgumentType::combinedImageSampler:
        if(useNestedResourceArgumentBuffer)
        {
          throw std::invalid_argument(
            "Metal ICB argument buffers require separate texture and sampler bindings");
        }
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
    desc.bindingSchema, desc.vertexInput, resources,
    desc.supportsGpuGeneratedCommands);
  result.rasterState = desc.rasterState;
  result.depthState = desc.depthState;
  result.specializationVariant = desc.specializationVariant;
  result.supportsGpuGeneratedCommands = desc.supportsGpuGeneratedCommands;

  FunctionDescriptorOwner vertexFunction = makeFunctionDescriptor(
    *vertex, resources, desc.supportsGpuGeneratedCommands);
  FunctionDescriptorOwner fragmentFunction;
  if(fragment != nullptr)
  {
    fragmentFunction = makeFunctionDescriptor(
      *fragment, resources, desc.supportsGpuGeneratedCommands);
  }

  auto* pipelineDesc = MTL4::RenderPipelineDescriptor::alloc()->init();
  pipelineDesc->setLabel(nsString("Metal4 graphics pipeline"));
  pipelineDesc->setVertexFunctionDescriptor(vertexFunction.get());
  if(fragment != nullptr)
    pipelineDesc->setFragmentFunctionDescriptor(fragmentFunction.get());
  pipelineDesc->setInputPrimitiveTopology(
    toMetalTopologyClass(desc.rasterState.topology));
  pipelineDesc->setRasterSampleCount(static_cast<uint32_t>(desc.rasterState.sampleCount));
  pipelineDesc->setRasterizationEnabled(true);
  if(result.supportsGpuGeneratedCommands)
  {
    pipelineDesc->setSupportIndirectCommandBuffers(
      MTL4::IndirectCommandBufferSupportStateEnabled);
  }
  // Xcode 27 beta's shader validation drops draws from GPU-generated ICBs and
  // then crashes its diagnostic decoder. The Slang ICB builder and ordinary
  // pipelines remain validated; API validation still covers this PSO.
  configurePipelineOptions(*pipelineDesc, enableShaderValidation,
                           result.supportsGpuGeneratedCommands);

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
  if(result.nativeRenderPipeline == nullptr &&
     result.supportsGpuGeneratedCommands)
  {
    const std::string indirectError = errorMessage(
      "MTL4::Compiler graphics pipeline with indirect-command support", error);
    LOGW("Metal disabled GPU-generated commands for %s variant %u: %s",
         result.debugName.c_str(), result.specializationVariant,
         indirectError.c_str());
    // Keep an ordinary Metal 4 PSO for the clamped direct-draw fallback.
    pipelineDesc->setSupportIndirectCommandBuffers(
      MTL4::IndirectCommandBufferSupportStateDisabled);
    result.supportsGpuGeneratedCommands = false;
    error = nullptr;
    result.nativeRenderPipeline = compiler.newRenderPipelineState(
      pipelineDesc, taskOptions, &error);
  }
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

  if(result.bindingLayout.nestedArgumentBufferIndex != kInvalidMetalBinding)
  {
    const ShaderEntry& argumentEntry = fragment != nullptr ? *fragment : *vertex;
    const FunctionDescriptorOwner& argumentDescriptor = fragment != nullptr
      ? fragmentFunction
      : vertexFunction;
    std::string functionError;
    auto function = NS::TransferPtr(makeMetalFunction(
      argumentEntry, argumentDescriptor, resources,
      desc.supportsGpuGeneratedCommands, functionError));
    if(!function)
    {
      result.nativeDepthStencilState->release();
      result.nativeRenderPipeline->release();
      throw std::runtime_error(functionError);
    }
    result.nestedArgumentEncoder = function->newArgumentEncoder(
      result.bindingLayout.nestedArgumentBufferIndex);
    if(result.nestedArgumentEncoder == nullptr)
    {
      result.nativeDepthStencilState->release();
      result.nativeRenderPipeline->release();
      throw std::runtime_error(
        "Metal ICB shader did not expose its material ParameterBlock");
    }
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

MetalBuiltinPipelines compileMetalBuiltinPipelines(
  MTL::Device& device, MTL4::Compiler& compiler, bool enableShaderValidation)
{
  dispatch_data_t payload = dispatch_data_create(
    mgip_metal_builtins_lib, mgip_metal_builtins_lib_sizeInBytes,
    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  if(payload == nullptr)
    throw std::runtime_error("Metal failed to wrap the built-in Slang metallib");
  NS::Error* error = nullptr;
  MTL::Library* library = device.newLibrary(payload, &error);
  dispatch_release(payload);
  if(library == nullptr)
    throw std::runtime_error(errorMessage("MTLDevice::newLibrary(MetalBuiltins)", error));
  library->setLabel(nsString("MGIP Metal4 built-in Slang library"));

  MetalBuiltinPipelines result;
  const auto compilePipeline =
    [&](const char* entryPoint, const char* label, MTL::Size threadgroupSize) {
      auto* functionDesc = MTL4::LibraryFunctionDescriptor::alloc()->init();
      functionDesc->setLibrary(library);
      functionDesc->setName(nsString(entryPoint));

      auto* pipelineDesc = MTL4::ComputePipelineDescriptor::alloc()->init();
      pipelineDesc->setLabel(nsString(label));
      pipelineDesc->setComputeFunctionDescriptor(functionDesc);
      pipelineDesc->setRequiredThreadsPerThreadgroup(threadgroupSize);
      pipelineDesc->setMaxTotalThreadsPerThreadgroup(
        threadgroupSize.width * threadgroupSize.height * threadgroupSize.depth);
      configurePipelineOptions(*pipelineDesc, enableShaderValidation);

      auto* taskOptions = MTL4::CompilerTaskOptions::alloc()->init();
      error = nullptr;
      MTL::ComputePipelineState* pipeline = compiler.newComputePipelineState(
        pipelineDesc, taskOptions, &error);
      taskOptions->release();
      pipelineDesc->release();
      functionDesc->release();
      if(pipeline == nullptr)
        throw std::runtime_error(errorMessage(label, error));
      return pipeline;
    };

  try
  {
    result.blit = compilePipeline(
      "mgipMetal4Blit2D", "MTL4::Compiler Slang blit pipeline",
      MTL::Size::Make(8, 8, 1));
    result.indexedIndirectUInt32 = compilePipeline(
      "mgipPrepareIndexedIndirectUInt32",
      "MTL4::Compiler Slang indexed-indirect uint32 pipeline",
      MTL::Size::Make(64, 1, 1));
    result.indexedIndirectUInt16 = compilePipeline(
      "mgipPrepareIndexedIndirectUInt16",
      "MTL4::Compiler Slang indexed-indirect uint16 pipeline",
      MTL::Size::Make(64, 1, 1));

    result.indexedIndirectFunction = library->newFunction(
      nsString("mgipPrepareIndexedIndirectUInt32"));
    if(result.indexedIndirectFunction == nullptr)
      throw std::runtime_error("Metal failed to load the indexed-indirect Slang function");
  }
  catch(...)
  {
    if(result.indexedIndirectFunction != nullptr)
      result.indexedIndirectFunction->release();
    if(result.indexedIndirectUInt16 != nullptr)
      result.indexedIndirectUInt16->release();
    if(result.indexedIndirectUInt32 != nullptr)
      result.indexedIndirectUInt32->release();
    if(result.blit != nullptr)
      result.blit->release();
    library->release();
    throw;
  }
  library->release();
  return result;
}

}  // namespace demo::rhi::metal
