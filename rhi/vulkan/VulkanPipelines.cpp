#include "VulkanPipelines.h"

#include "VulkanPipelineConversions.h"
#include "VulkanShaderConversions.h"

#include <algorithm>
#include <array>

namespace demo::rhi::vulkan {



VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineCreateInfo& createInfo)
{
  ASSERT(createInfo.desc != nullptr, "Graphics pipeline creation requires a desc");
  ASSERT(createInfo.layout != VK_NULL_HANDLE, "Graphics pipeline creation requires a backend-private layout");
  const GraphicsPipelineDesc& desc = *createInfo.desc;
  const VkPipelineLayout layout = createInfo.layout;

  std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
  shaderStages.reserve(static_cast<uint32_t>(desc.shaderStages.size()));

  std::vector<VkSpecializationMapEntry> mapEntries{};
  std::vector<VkSpecializationInfo>     specializationInfos{};
  specializationInfos.resize(static_cast<uint32_t>(desc.shaderStages.size()));
  for(uint32_t stageIndex = 0; stageIndex < static_cast<uint32_t>(desc.shaderStages.size()); ++stageIndex)
  {
    const ShaderEntry& stageDesc = desc.shaderStages[stageIndex];

    if(static_cast<uint32_t>(stageDesc.specializationConstants.size()) > 0)
    {
      const uint32_t baseOffset = static_cast<uint32_t>(mapEntries.size());
      mapEntries.reserve(mapEntries.size() + static_cast<uint32_t>(stageDesc.specializationConstants.size()));
      for(uint32_t i = 0; i < static_cast<uint32_t>(stageDesc.specializationConstants.size()); ++i)
      {
        const SpecializationConstant& constant = stageDesc.specializationConstants[i];
        mapEntries.push_back(VkSpecializationMapEntry{
            .constantID = constant.constantId,
            .offset     = constant.offset,
            .size       = constant.size,
        });
      }

      specializationInfos[stageIndex] = VkSpecializationInfo{
          .mapEntryCount = static_cast<uint32_t>(stageDesc.specializationConstants.size()),
          .pMapEntries   = mapEntries.data() + baseOffset,
          .dataSize      = stageDesc.specializationData.bytes.size(),
          .pData         = stageDesc.specializationData.bytes.data(),
      };
    }
    ASSERT(createInfo.shaderModules != nullptr
           && createInfo.shaderModuleCount == static_cast<uint32_t>(desc.shaderStages.size()),
           "Graphics pipeline shader modules must match shader entries");
    const VkShaderModule stageModule =
      createInfo.shaderModules != nullptr && stageIndex < createInfo.shaderModuleCount
        ? createInfo.shaderModules[stageIndex]
        : VK_NULL_HANDLE;
    ASSERT(stageModule != VK_NULL_HANDLE,
           "Graphics pipeline shader entry requires a valid backend module");
    shaderStages.push_back(VkPipelineShaderStageCreateInfo{
        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage               = toVkShaderStage(stageDesc.stage),
        .module              = stageModule,
        .pName               = stageDesc.entryPoint.data(),
        .pSpecializationInfo = static_cast<uint32_t>(stageDesc.specializationConstants.size()) > 0 ? &specializationInfos[stageIndex] : nullptr,
    });
  }

  std::vector<VkVertexInputBindingDescription> vertexBindings;
  vertexBindings.reserve(static_cast<uint32_t>(desc.vertexInput.bindings.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.vertexInput.bindings.size()); ++i)
  {
    const VertexBindingDesc& binding = desc.vertexInput.bindings[i];
    vertexBindings.push_back(VkVertexInputBindingDescription{
        .binding   = binding.binding,
        .stride    = binding.stride,
        .inputRate = toVkVertexInputRate(binding.inputRate),
    });
  }

  std::vector<VkVertexInputAttributeDescription> vertexAttributes;
  vertexAttributes.reserve(static_cast<uint32_t>(desc.vertexInput.attributes.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.vertexInput.attributes.size()); ++i)
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
  dynamicStates.reserve(static_cast<uint32_t>(desc.dynamicStates.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.dynamicStates.size()); ++i)
  {
    dynamicStates.push_back(toVkDynamicState(desc.dynamicStates[i]));
  }

  std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
  blendAttachments.reserve(static_cast<uint32_t>(desc.blendStates.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.blendStates.size()); ++i)
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
  colorFormats.reserve(static_cast<uint32_t>(desc.renderingInfo.colorFormats.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.renderingInfo.colorFormats.size()); ++i)
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
      .sType               = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .depthClampEnable    = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode         = toVkPolygonMode(desc.rasterState.polygonMode),
      .cullMode            = toVkCullMode(desc.rasterState.cullMode),
      .frontFace           = toVkFrontFace(desc.rasterState.frontFace),
      .depthBiasEnable     = desc.rasterState.depthBiasEnable ? VK_TRUE : VK_FALSE,
      .depthBiasConstantFactor = desc.rasterState.depthBiasConstantFactor,
      .depthBiasClamp      = desc.rasterState.depthBiasClamp,
      .depthBiasSlopeFactor = desc.rasterState.depthBiasSlopeFactor,
      .lineWidth           = desc.rasterState.lineWidth,
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
  ASSERT(createInfo.desc != nullptr, "Compute pipeline creation requires a desc");
  ASSERT(createInfo.layout != VK_NULL_HANDLE, "Compute pipeline creation requires a backend-private layout");
  const ComputePipelineDesc& desc = *createInfo.desc;
  const VkPipelineLayout layout = createInfo.layout;
  const VkShaderModule computeModule = createInfo.shaderModule;
  ASSERT(computeModule != VK_NULL_HANDLE,
         "Compute pipeline shader entry requires a valid backend module");
  std::vector<VkSpecializationMapEntry> mapEntries;
  mapEntries.reserve(static_cast<uint32_t>(desc.shaderStage.specializationConstants.size()));
  for(uint32_t i = 0; i < static_cast<uint32_t>(desc.shaderStage.specializationConstants.size()); ++i)
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
      .dataSize      = desc.shaderStage.specializationData.bytes.size(),
      .pData         = desc.shaderStage.specializationData.bytes.data(),
  };

  const VkPipelineShaderStageCreateInfo shaderStageInfo{
      .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage               = toVkShaderStage(desc.shaderStage.stage),
      .module              = computeModule,
      .pName               = desc.shaderStage.entryPoint.data(),
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
