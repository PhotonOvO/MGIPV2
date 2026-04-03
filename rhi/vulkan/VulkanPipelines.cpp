#include "VulkanPipelines.h"

#include <algorithm>
#include <array>

namespace demo::rhi::vulkan {

namespace {

VkShaderStageFlagBits toVkShaderStage(ShaderStage stage)
{
  switch(stage)
  {
    case ShaderStage::vertex:
      return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::fragment:
      return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::compute:
      return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::geometry:
      return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::tessControl:
      return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::tessEval:
      return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    default:
      return VK_SHADER_STAGE_ALL;
  }
}

VkShaderStageFlags toVkShaderStageMask(ShaderStage stageMask)
{
  VkShaderStageFlags flags = 0;
  const uint32_t     mask  = static_cast<uint32_t>(stageMask);
  if((mask & static_cast<uint32_t>(ShaderStage::vertex)) != 0)
  {
    flags |= VK_SHADER_STAGE_VERTEX_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::fragment)) != 0)
  {
    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::compute)) != 0)
  {
    flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::geometry)) != 0)
  {
    flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::tessControl)) != 0)
  {
    flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::tessEval)) != 0)
  {
    flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  return flags;
}

VkFormat toVkFormat(TextureFormat format)
{
  switch(format)
  {
    case TextureFormat::rgba8Unorm:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::bgra8Unorm:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::d16Unorm:
      return VK_FORMAT_D16_UNORM;
    case TextureFormat::d32Sfloat:
      return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::d24UnormS8:
      return VK_FORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::d32SfloatS8:
      return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

VkPrimitiveTopology toVkTopology(PrimitiveTopology topology)
{
  switch(topology)
  {
    case PrimitiveTopology::pointList:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::lineList:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::lineStrip:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::triangleStrip:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkPolygonMode toVkPolygonMode(PolygonMode mode)
{
  switch(mode)
  {
    case PolygonMode::line:
      return VK_POLYGON_MODE_LINE;
    case PolygonMode::point:
      return VK_POLYGON_MODE_POINT;
    default:
      return VK_POLYGON_MODE_FILL;
  }
}

VkCullModeFlags toVkCullMode(CullMode mode)
{
  switch(mode)
  {
    case CullMode::front:
      return VK_CULL_MODE_FRONT_BIT;
    case CullMode::back:
      return VK_CULL_MODE_BACK_BIT;
    case CullMode::frontAndBack:
      return VK_CULL_MODE_FRONT_AND_BACK;
    default:
      return VK_CULL_MODE_NONE;
  }
}

VkFrontFace toVkFrontFace(FrontFace frontFace)
{
  return frontFace == FrontFace::clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkCompareOp toVkCompareOp(CompareOp op)
{
  switch(op)
  {
    case CompareOp::never:
      return VK_COMPARE_OP_NEVER;
    case CompareOp::less:
      return VK_COMPARE_OP_LESS;
    case CompareOp::equal:
      return VK_COMPARE_OP_EQUAL;
    case CompareOp::greater:
      return VK_COMPARE_OP_GREATER;
    case CompareOp::notEqual:
      return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::greaterOrEqual:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::always:
      return VK_COMPARE_OP_ALWAYS;
    default:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
  }
}

VkBlendFactor toVkBlendFactor(BlendFactor factor)
{
  switch(factor)
  {
    case BlendFactor::zero:
      return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::srcColor:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::oneMinusSrcColor:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::dstColor:
      return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::oneMinusDstColor:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::srcAlpha:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::oneMinusSrcAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::dstAlpha:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::oneMinusDstAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
      return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp toVkBlendOp(BlendOp op)
{
  switch(op)
  {
    case BlendOp::subtract:
      return VK_BLEND_OP_SUBTRACT;
    case BlendOp::reverseSubtract:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::min:
      return VK_BLEND_OP_MIN;
    case BlendOp::max:
      return VK_BLEND_OP_MAX;
    default:
      return VK_BLEND_OP_ADD;
  }
}

VkColorComponentFlags toVkColorMask(ColorComponentFlags mask)
{
  const uint32_t        bits = static_cast<uint32_t>(mask);
  VkColorComponentFlags vkMask{0};
  if((bits & static_cast<uint32_t>(ColorComponentFlags::r)) != 0)
  {
    vkMask |= VK_COLOR_COMPONENT_R_BIT;
  }
  if((bits & static_cast<uint32_t>(ColorComponentFlags::g)) != 0)
  {
    vkMask |= VK_COLOR_COMPONENT_G_BIT;
  }
  if((bits & static_cast<uint32_t>(ColorComponentFlags::b)) != 0)
  {
    vkMask |= VK_COLOR_COMPONENT_B_BIT;
  }
  if((bits & static_cast<uint32_t>(ColorComponentFlags::a)) != 0)
  {
    vkMask |= VK_COLOR_COMPONENT_A_BIT;
  }
  return vkMask;
}

VkSampleCountFlagBits toVkSampleCount(SampleCount count)
{
  switch(count)
  {
    case SampleCount::count2:
      return VK_SAMPLE_COUNT_2_BIT;
    case SampleCount::count4:
      return VK_SAMPLE_COUNT_4_BIT;
    case SampleCount::count8:
      return VK_SAMPLE_COUNT_8_BIT;
    default:
      return VK_SAMPLE_COUNT_1_BIT;
  }
}

VkVertexInputRate toVkVertexInputRate(VertexInputRate rate)
{
  return rate == VertexInputRate::perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
}

VkFormat toVkVertexFormat(VertexFormat format)
{
  switch(format)
  {
    case VertexFormat::r32Sfloat:
      return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::r32g32Sfloat:
      return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::r32g32b32Sfloat:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::r32g32b32a32Sfloat:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

VkDynamicState toVkDynamicState(DynamicState dynamicState)
{
  switch(dynamicState)
  {
    case DynamicState::scissor:
      return VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
    default:
      return VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
  }
}

}  // namespace

void VulkanPipelineLayout::init(void* nativeDevice, const PipelineLayoutDesc& desc)
{
  VulkanPipelineLayoutLowering lowering{};
  init(nativeDevice, desc, lowering);
}

void VulkanPipelineLayout::init(void* nativeDevice, const PipelineLayoutDesc& desc, const VulkanPipelineLayoutLowering& lowering)
{
  deinit();
  m_device = static_cast<VkDevice>(nativeDevice);
  m_desc   = desc;

  std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
  descriptorSetLayouts.reserve(lowering.setLayoutCount);
  uint32_t maxLogicalSet = 0;
  bool     hasEntries    = false;
  for(const PipelineLayoutEntry& entry : m_desc.entries)
  {
    maxLogicalSet = (std::max)(maxLogicalSet, entry.logicalSet);
    hasEntries    = true;
  }

  if(hasEntries)
  {
    descriptorSetLayouts.assign(maxLogicalSet + 1u, VK_NULL_HANDLE);
    for(uint32_t i = 0; i < lowering.setLayoutCount; ++i)
    {
      const VulkanPipelineLayoutBindingMapping& mapping = lowering.setLayouts[i];
      if(mapping.logicalSet < descriptorSetLayouts.size())
      {
        descriptorSetLayouts[mapping.logicalSet] = mapping.descriptorSetLayout;
      }
    }

    while(!descriptorSetLayouts.empty() && descriptorSetLayouts.back() == VK_NULL_HANDLE)
    {
      descriptorSetLayouts.pop_back();
    }
  }
  else
  {
    for(uint32_t i = 0; i < lowering.setLayoutCount; ++i)
    {
      descriptorSetLayouts.push_back(lowering.setLayouts[i].descriptorSetLayout);
    }
  }

  std::vector<VkPushConstantRange> pushConstantRanges;
  pushConstantRanges.reserve(m_desc.pushConstantRanges.size());
  for(const PipelinePushConstantRange& range : m_desc.pushConstantRanges)
  {
    pushConstantRanges.push_back(VkPushConstantRange{
        .stageFlags = toVkShaderStageMask(range.stages),
        .offset     = range.offset,
        .size       = range.size,
    });
  }

  const VkPipelineLayoutCreateInfo createInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = static_cast<uint32_t>(descriptorSetLayouts.size()),
      .pSetLayouts            = descriptorSetLayouts.data(),
      .pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size()),
      .pPushConstantRanges    = pushConstantRanges.data(),
  };
  VK_CHECK(vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_layout));
}

void VulkanPipelineLayout::deinit()
{
  if(m_device != VK_NULL_HANDLE && m_layout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(m_device, m_layout, nullptr);
  }
  m_layout = VK_NULL_HANDLE;
  m_desc   = PipelineLayoutDesc{};
}

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineCreateInfo& createInfo)
{
  const GraphicsPipelineDesc& desc = createInfo.desc;
  ASSERT(desc.layout != nullptr, "GraphicsPipelineDesc requires a valid PipelineLayout");

  const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(desc.layout->getNativeHandle());

  std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
  shaderStages.reserve(desc.shaderStageCount);

  std::vector<VkSpecializationMapEntry> mapEntries{};
  std::vector<VkSpecializationInfo>     specializationInfos{};
  specializationInfos.resize(desc.shaderStageCount);
  for(uint32_t stageIndex = 0; stageIndex < desc.shaderStageCount; ++stageIndex)
  {
    const PipelineShaderStageDesc& stageDesc = desc.shaderStages[stageIndex];

    if(stageDesc.specializationConstantCount > 0)
    {
      const uint32_t baseOffset = static_cast<uint32_t>(mapEntries.size());
      mapEntries.reserve(mapEntries.size() + stageDesc.specializationConstantCount);
      for(uint32_t i = 0; i < stageDesc.specializationConstantCount; ++i)
      {
        const SpecializationConstant& constant = stageDesc.specializationConstants[i];
        mapEntries.push_back(VkSpecializationMapEntry{
            .constantID = constant.constantId,
            .offset     = constant.offset,
            .size       = constant.size,
        });
      }

      specializationInfos[stageIndex] = VkSpecializationInfo{
          .mapEntryCount = stageDesc.specializationConstantCount,
          .pMapEntries   = mapEntries.data() + baseOffset,
          .dataSize      = stageDesc.specializationData.size,
          .pData         = stageDesc.specializationData.data,
      };
    }

    shaderStages.push_back(VkPipelineShaderStageCreateInfo{
        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage               = toVkShaderStage(stageDesc.stage),
        .module              = reinterpret_cast<VkShaderModule>(stageDesc.shaderModule),
        .pName               = stageDesc.entryPoint,
        .pSpecializationInfo = stageDesc.specializationConstantCount > 0 ? &specializationInfos[stageIndex] : nullptr,
    });
  }

  std::vector<VkVertexInputBindingDescription> vertexBindings;
  vertexBindings.reserve(desc.vertexInput.bindingCount);
  for(uint32_t i = 0; i < desc.vertexInput.bindingCount; ++i)
  {
    const VertexBindingDesc& binding = desc.vertexInput.bindings[i];
    vertexBindings.push_back(VkVertexInputBindingDescription{
        .binding   = binding.binding,
        .stride    = binding.stride,
        .inputRate = toVkVertexInputRate(binding.inputRate),
    });
  }

  std::vector<VkVertexInputAttributeDescription> vertexAttributes;
  vertexAttributes.reserve(desc.vertexInput.attributeCount);
  for(uint32_t i = 0; i < desc.vertexInput.attributeCount; ++i)
  {
    const VertexAttributeDesc& attribute = desc.vertexInput.attributes[i];
    vertexAttributes.push_back(VkVertexInputAttributeDescription{
        .location = attribute.location,
        .binding  = attribute.binding,
        .format   = toVkVertexFormat(attribute.format),
        .offset   = attribute.offset,
    });
  }

  std::vector<VkDynamicState> dynamicStates;
  dynamicStates.reserve(desc.dynamicStateCount);
  for(uint32_t i = 0; i < desc.dynamicStateCount; ++i)
  {
    dynamicStates.push_back(toVkDynamicState(desc.dynamicStates[i]));
  }

  std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
  blendAttachments.reserve(desc.blendStateCount);
  for(uint32_t i = 0; i < desc.blendStateCount; ++i)
  {
    const BlendAttachmentState& blendState = desc.blendStates[i];
    blendAttachments.push_back(VkPipelineColorBlendAttachmentState{
        .blendEnable         = blendState.blendEnable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = toVkBlendFactor(blendState.srcColorBlendFactor),
        .dstColorBlendFactor = toVkBlendFactor(blendState.dstColorBlendFactor),
        .colorBlendOp        = toVkBlendOp(blendState.colorBlendOp),
        .srcAlphaBlendFactor = toVkBlendFactor(blendState.srcAlphaBlendFactor),
        .dstAlphaBlendFactor = toVkBlendFactor(blendState.dstAlphaBlendFactor),
        .alphaBlendOp        = toVkBlendOp(blendState.alphaBlendOp),
        .colorWriteMask      = toVkColorMask(blendState.colorWriteMask),
    });
  }

  std::vector<VkFormat> colorFormats;
  colorFormats.reserve(desc.renderingInfo.colorFormatCount);
  for(uint32_t i = 0; i < desc.renderingInfo.colorFormatCount; ++i)
  {
    colorFormats.push_back(toVkFormat(desc.renderingInfo.colorFormats[i]));
  }

  const VkPipelineVertexInputStateCreateInfo vertexInputInfo{
      .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount   = static_cast<uint32_t>(vertexBindings.size()),
      .pVertexBindingDescriptions      = vertexBindings.data(),
      .vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size()),
      .pVertexAttributeDescriptions    = vertexAttributes.data(),
  };

  const VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology               = toVkTopology(desc.rasterState.topology),
      .primitiveRestartEnable = desc.rasterState.primitiveRestartEnable ? VK_TRUE : VK_FALSE,
  };

  const VkPipelineDynamicStateCreateInfo dynamicStateInfo{
      .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
      .pDynamicStates    = dynamicStates.data(),
  };

  const VkPipelineRasterizationStateCreateInfo rasterizerInfo{
      .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = toVkPolygonMode(desc.rasterState.polygonMode),
      .cullMode    = toVkCullMode(desc.rasterState.cullMode),
      .frontFace   = toVkFrontFace(desc.rasterState.frontFace),
      .lineWidth   = desc.rasterState.lineWidth,
  };

  const VkPipelineMultisampleStateCreateInfo multisamplingInfo{
      .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = toVkSampleCount(desc.rasterState.sampleCount),
  };

  const VkPipelineColorBlendStateCreateInfo colorBlendingInfo{
      .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable   = VK_FALSE,
      .logicOp         = VK_LOGIC_OP_COPY,
      .attachmentCount = static_cast<uint32_t>(blendAttachments.size()),
      .pAttachments    = blendAttachments.data(),
  };

  const VkPipelineDepthStencilStateCreateInfo depthStateInfo{
      .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable  = desc.depthState.depthTestEnable ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = desc.depthState.depthWriteEnable ? VK_TRUE : VK_FALSE,
      .depthCompareOp   = toVkCompareOp(desc.depthState.depthCompareOp),
  };

  const VkPipelineRenderingCreateInfo renderingInfo{
      .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount    = static_cast<uint32_t>(colorFormats.size()),
      .pColorAttachmentFormats = colorFormats.data(),
      .depthAttachmentFormat   = toVkFormat(desc.renderingInfo.depthFormat),
  };

  const VkGraphicsPipelineCreateInfo pipelineInfo{
      .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext               = &renderingInfo,
      .stageCount          = static_cast<uint32_t>(shaderStages.size()),
      .pStages             = shaderStages.data(),
      .pVertexInputState   = &vertexInputInfo,
      .pInputAssemblyState = &inputAssemblyInfo,
      .pRasterizationState = &rasterizerInfo,
      .pMultisampleState   = &multisamplingInfo,
      .pDepthStencilState  = &depthStateInfo,
      .pColorBlendState    = &colorBlendingInfo,
      .pDynamicState       = &dynamicStateInfo,
      .layout              = layout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, const ComputePipelineCreateInfo& createInfo)
{
  const ComputePipelineDesc& desc = createInfo.desc;
  ASSERT(desc.layout != nullptr, "ComputePipelineDesc requires a valid PipelineLayout");

  const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(desc.layout->getNativeHandle());

  std::vector<VkSpecializationMapEntry> mapEntries;
  mapEntries.reserve(desc.shaderStage.specializationConstantCount);
  for(uint32_t i = 0; i < desc.shaderStage.specializationConstantCount; ++i)
  {
    const SpecializationConstant& constant = desc.shaderStage.specializationConstants[i];
    mapEntries.push_back(VkSpecializationMapEntry{
        .constantID = constant.constantId,
        .offset     = constant.offset,
        .size       = constant.size,
    });
  }

  const VkSpecializationInfo specializationInfo{
      .mapEntryCount = static_cast<uint32_t>(mapEntries.size()),
      .pMapEntries   = mapEntries.data(),
      .dataSize      = desc.shaderStage.specializationData.size,
      .pData         = desc.shaderStage.specializationData.data,
  };

  const VkPipelineShaderStageCreateInfo shaderStageInfo{
      .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage               = toVkShaderStage(desc.shaderStage.stage),
      .module              = reinterpret_cast<VkShaderModule>(desc.shaderStage.shaderModule),
      .pName               = desc.shaderStage.entryPoint,
      .pSpecializationInfo = mapEntries.empty() ? nullptr : &specializationInfo,
  };

  const VkPipelineCreateFlags2CreateInfoKHR createFlags2{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
      .flags = createInfo.pipelineFlags,
  };

  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext  = &createFlags2,
      .stage  = shaderStageInfo,
      .layout = layout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  return pipeline;
}

}  // namespace demo::rhi::vulkan
