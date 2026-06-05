#include "IBLResources.h"
#include "RHIFormatBridge.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace demo {

namespace {

void imageBarrier(VkCommandBuffer cmd,
                  VkImage image,
                  VkImageSubresourceRange range,
                  VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess)
{
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = srcStage,
      .srcAccessMask = srcAccess,
      .dstStageMask = dstStage,
      .dstAccessMask = dstAccess,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = range,
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
}

VkImageSubresourceRange colorRange(uint32_t mipCount, uint32_t layerCount)
{
  return VkImageSubresourceRange{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = mipCount,
      .baseArrayLayer = 0,
      .layerCount = layerCount,
  };
}

}  // namespace

void IBLResources::init(rhi::Device& device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  deinit();
  m_rhiDevice = &device;
  m_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getNativeDevice()));
  m_allocator = allocator;
  m_cubeMapSize = createInfo.cubeMapSize;
  m_dfgLUTSize = createInfo.dfgLUTSize;

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();

  const VkSamplerCreateInfo cubeSamplerInfo{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = VK_LOD_CLAMP_NONE,
  };
  VK_CHECK(vkCreateSampler(m_device, &cubeSamplerInfo, nullptr, &m_cubeMapSampler));
  dutil.setObjectName(m_cubeMapSampler, "IBL_CubeMapSampler");

  const VkSamplerCreateInfo lutSamplerInfo{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  };
  VK_CHECK(vkCreateSampler(m_device, &lutSamplerInfo, nullptr, &m_lutSampler));
  dutil.setObjectName(m_lutSampler, "IBL_LUTSampler");

  createImages(cmd, createInfo);
  generateIBLMaps(cmd, createInfo);
  transitionGeneratedImagesForSampling(cmd);
}

void IBLResources::createImages(VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();
  m_maxMipLevel = static_cast<uint32_t>(std::floor(std::log2(std::max(createInfo.cubeMapSize, 1u))));

  const auto makeView = [&](VkImage image, VkFormat format, rhi::ImageViewType viewType, uint32_t levelCount,
                            uint32_t layerCount, const char* name) -> rhi::TextureViewHandle {
    const rhi::TextureHandle imageHandle = m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(image));
    rhi::TextureViewCreateDesc desc{};
    desc.image        = imageHandle;
    desc.format       = toPortableTextureFormat(format);
    desc.viewType     = viewType;
    desc.aspect       = rhi::TextureAspect::color;
    desc.levelCount   = levelCount;
    desc.layerCount   = layerCount;
    const rhi::TextureViewHandle handle = m_rhiDevice->createTextureView(desc);
    m_rhiDevice->destroyImage(imageHandle);
    dutil.setObjectName(reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_rhiDevice->resolveTextureViewNative(handle))),
                        name);
    return handle;
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  const VkImageCreateInfo cubeInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.cubeMapFormat,
      .extent = {createInfo.cubeMapSize, createInfo.cubeMapSize, 1},
      .mipLevels = m_maxMipLevel + 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };

  VK_CHECK(vmaCreateImage(m_allocator, &cubeInfo, &imageAllocInfo, &m_prefilteredMap.image, &m_prefilteredMap.allocation, nullptr));
  dutil.setObjectName(m_prefilteredMap.image, "IBL_PrefilteredMap");

  m_prefilteredMapView = makeView(m_prefilteredMap.image, createInfo.cubeMapFormat, rhi::ImageViewType::eCube,
                                  m_maxMipLevel + 1, 6, "IBL_PrefilteredMapView");

  const VkImageCreateInfo irradianceInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.cubeMapFormat,
      .extent = {32, 32, 1},
      .mipLevels = 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VK_CHECK(vmaCreateImage(m_allocator, &irradianceInfo, &imageAllocInfo, &m_irradianceMap.image, &m_irradianceMap.allocation, nullptr));
  dutil.setObjectName(m_irradianceMap.image, "IBL_IrradianceMap");

  m_irradianceMapView = makeView(m_irradianceMap.image, createInfo.cubeMapFormat, rhi::ImageViewType::eCube, 1, 6,
                                 "IBL_IrradianceMapView");

  m_irradianceStorageView = makeView(m_irradianceMap.image, createInfo.cubeMapFormat, rhi::ImageViewType::e2DArray, 1, 6,
                                     "IBL_IrradianceStorageView");

  const VkImageCreateInfo lutInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.dfgLUTFormat,
      .extent = {createInfo.dfgLUTSize, createInfo.dfgLUTSize, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VK_CHECK(vmaCreateImage(m_allocator, &lutInfo, &imageAllocInfo, &m_dfgLUT.image, &m_dfgLUT.allocation, nullptr));
  dutil.setObjectName(m_dfgLUT.image, "IBL_DFGLUT");

  m_dfgLUTView = makeView(m_dfgLUT.image, createInfo.dfgLUTFormat, rhi::ImageViewType::e2D, 1, 1, "IBL_DFGLUTView");

  utils::cmdInitImageLayout(cmd, m_prefilteredMap.image);
  utils::cmdInitImageLayout(cmd, m_irradianceMap.image);
  utils::cmdInitImageLayout(cmd, m_dfgLUT.image);

  const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
  const VkClearColorValue neutralLut = {{0.5f, 0.5f, 0.0f, 1.0f}};
  const VkImageSubresourceRange cubeRange = colorRange(m_maxMipLevel + 1, 6);
  const VkImageSubresourceRange irradianceRange = colorRange(1, 6);
  const VkImageSubresourceRange lutRange = colorRange(1, 1);
  vkCmdClearColorImage(cmd, m_prefilteredMap.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cubeRange);
  vkCmdClearColorImage(cmd, m_irradianceMap.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &irradianceRange);
  vkCmdClearColorImage(cmd, m_dfgLUT.image, VK_IMAGE_LAYOUT_GENERAL, &neutralLut, 1, &lutRange);
}

void IBLResources::createGenerationPipeline(VkShaderModule shaderModule,
                                            const char* entryPoint,
                                            VkPipelineLayout pipelineLayout,
                                            VkPipeline& pipeline) const
{
  const VkPipelineShaderStageCreateInfo stage{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName = entryPoint,
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage,
      .layout = pipelineLayout,
  };
  VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
}

void IBLResources::generateIBLMaps(VkCommandBuffer cmd, const CreateInfo& createInfo)
{
#ifdef USE_SLANG
  const auto nativeOf = [&](rhi::TextureViewHandle handle) {
    return reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_rhiDevice->resolveTextureViewNative(handle)));
  };
  const uint32_t prefilterMipCount = m_maxMipLevel + 1u;
  const VkDescriptorPoolSize poolSizes[] = {
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, prefilterMipCount + 1u},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, prefilterMipCount + 2u},
  };
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = prefilterMipCount + 2u,
      .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
      .pPoolSizes = poolSizes,
  };
  VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_generationDescriptorPool));

  const VkPushConstantRange pushRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(GeneratePushConstants),
  };

  const std::array<VkDescriptorSetLayoutBinding, 2> envToCubeBindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo envToCubeLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(envToCubeBindings.size()),
      .pBindings = envToCubeBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(m_device, &envToCubeLayoutInfo, nullptr, &m_envGenerationSetLayout));

  const VkDescriptorSetLayoutBinding lutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkDescriptorSetLayoutCreateInfo lutLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &lutBinding,
  };
  VK_CHECK(vkCreateDescriptorSetLayout(m_device, &lutLayoutInfo, nullptr, &m_lutGenerationSetLayout));

  const VkPipelineLayoutCreateInfo envPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_envGenerationSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushRange,
  };
  VK_CHECK(vkCreatePipelineLayout(m_device, &envPipelineLayoutInfo, nullptr, &m_envGenerationPipelineLayout));

  const VkPipelineLayoutCreateInfo lutPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_lutGenerationSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushRange,
  };
  VK_CHECK(vkCreatePipelineLayout(m_device, &lutPipelineLayoutInfo, nullptr, &m_lutGenerationPipelineLayout));

  VkShaderModule dfgModule = utils::createShaderModule(m_device, {shader_ibl_dfg_slang, std::size(shader_ibl_dfg_slang)});
  createGenerationPipeline(dfgModule, "dfgLUTMain", m_lutGenerationPipelineLayout, m_dfgGenerationPipeline);
  vkDestroyShaderModule(m_device, dfgModule, nullptr);

  VkDescriptorSet dfgSet = VK_NULL_HANDLE;
  const VkDescriptorSetAllocateInfo dfgAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_generationDescriptorPool,
      .descriptorSetCount = 1,
      .pSetLayouts = &m_lutGenerationSetLayout,
  };
  VK_CHECK(vkAllocateDescriptorSets(m_device, &dfgAlloc, &dfgSet));
  const VkDescriptorImageInfo dfgImageInfo{VK_NULL_HANDLE, nativeOf(m_dfgLUTView), VK_IMAGE_LAYOUT_GENERAL};
  const VkWriteDescriptorSet dfgWrite{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = dfgSet,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .pImageInfo = &dfgImageInfo,
  };
  vkUpdateDescriptorSets(m_device, 1, &dfgWrite, 0, nullptr);

  GeneratePushConstants push{
      .width = createInfo.dfgLUTSize,
      .height = createInfo.dfgLUTSize,
      .sampleCount = createInfo.brdfSampleCount,
  };
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_dfgGenerationPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lutGenerationPipelineLayout, 0, 1, &dfgSet, 0, nullptr);
  vkCmdPushConstants(cmd, m_lutGenerationPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(cmd, (createInfo.dfgLUTSize + 7u) / 8u, (createInfo.dfgLUTSize + 7u) / 8u, 1u);

  VkShaderModule irradianceModule = VK_NULL_HANDLE;
  VkShaderModule prefilterModule = VK_NULL_HANDLE;
  VkDescriptorSet irradianceSet = VK_NULL_HANDLE;

  if(!createInfo.sourceEnvironmentView.isNull())
  {
    std::vector<VkDescriptorSetLayout> layouts(prefilterMipCount + 1u, m_envGenerationSetLayout);
    const VkDescriptorSetAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_generationDescriptorPool,
        .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
        .pSetLayouts = layouts.data(),
    };
    std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, sets.data()));
    irradianceSet = sets[0];
    std::span<VkDescriptorSet> prefilterSets{sets.data() + 1, prefilterMipCount};

    irradianceModule = utils::createShaderModule(m_device, {shader_ibl_irradiance_slang, std::size(shader_ibl_irradiance_slang)});
    prefilterModule = utils::createShaderModule(m_device, {shader_ibl_prefilter_slang, std::size(shader_ibl_prefilter_slang)});
    createGenerationPipeline(irradianceModule, "irradianceConvolutionMain", m_envGenerationPipelineLayout, m_irradianceGenerationPipeline);
    createGenerationPipeline(prefilterModule, "prefilterGGXMain", m_envGenerationPipelineLayout, m_prefilterGenerationPipeline);

    const VkDescriptorImageInfo sourceInfo{m_cubeMapSampler, nativeOf(createInfo.sourceEnvironmentView), VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo irradianceInfo{VK_NULL_HANDLE, nativeOf(m_irradianceStorageView), VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 2> writes{{
        VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = irradianceSet, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sourceInfo},
        VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = irradianceSet, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &irradianceInfo},
    }};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    push = GeneratePushConstants{
        .width = 32,
        .height = 32,
        .sourceWidth = createInfo.sourceWidth,
        .sourceHeight = createInfo.sourceHeight,
        .sourceMaxMip = std::max(1u, createInfo.sourceMipCount) - 1u,
        .sampleCount = createInfo.irradianceSampleCount,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradianceGenerationPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_envGenerationPipelineLayout, 0, 1, &irradianceSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_envGenerationPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (push.width + 7u) / 8u, (push.height + 7u) / 8u, 6u);

    imageBarrier(cmd,
                 m_irradianceMap.image,
                 colorRange(1, 6),
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    for(uint32_t mip = 0; mip <= m_maxMipLevel; ++mip)
    {
      const uint32_t mipSize = std::max(1u, createInfo.cubeMapSize >> mip);
      const VkDescriptorSet prefilterSet = prefilterSets[mip];
      const rhi::TextureHandle mipImageHandle =
          m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(m_prefilteredMap.image));
      rhi::TextureViewCreateDesc mipViewDesc{};
      mipViewDesc.image         = mipImageHandle;
      mipViewDesc.format        = toPortableTextureFormat(createInfo.cubeMapFormat);
      mipViewDesc.viewType      = rhi::ImageViewType::e2DArray;
      mipViewDesc.aspect        = rhi::TextureAspect::color;
      mipViewDesc.baseMipLevel  = mip;
      mipViewDesc.levelCount    = 1;
      mipViewDesc.baseArrayLayer = 0;
      mipViewDesc.layerCount    = 6;
      const rhi::TextureViewHandle mipView = m_rhiDevice->createTextureView(mipViewDesc);
      m_rhiDevice->destroyImage(mipImageHandle);
      m_generationMipViews.push_back(mipView);
      const VkDescriptorImageInfo prefilterOutputInfo{VK_NULL_HANDLE, nativeOf(mipView), VK_IMAGE_LAYOUT_GENERAL};
      writes = {{
          VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = prefilterSet, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sourceInfo},
          VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = prefilterSet, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &prefilterOutputInfo},
      }};
      vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

      push = GeneratePushConstants{
          .width = mipSize,
          .height = mipSize,
          .sourceWidth = createInfo.sourceWidth,
          .sourceHeight = createInfo.sourceHeight,
          .sourceMaxMip = std::max(1u, createInfo.sourceMipCount) - 1u,
          .sampleCount = createInfo.prefilterSampleCount,
          .roughness = m_maxMipLevel > 0 ? static_cast<float>(mip) / static_cast<float>(m_maxMipLevel) : 0.0f,
      };
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterGenerationPipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_envGenerationPipelineLayout, 0, 1, &prefilterSet, 0, nullptr);
      vkCmdPushConstants(cmd, m_envGenerationPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
      vkCmdDispatch(cmd, (mipSize + 7u) / 8u, (mipSize + 7u) / 8u, 6u);
    }
    m_splitSumReady = true;
  }

  if(prefilterModule != VK_NULL_HANDLE)
    vkDestroyShaderModule(m_device, prefilterModule, nullptr);
  if(irradianceModule != VK_NULL_HANDLE)
    vkDestroyShaderModule(m_device, irradianceModule, nullptr);
#else
  (void)cmd;
  (void)createInfo;
#endif
}

void IBLResources::transitionGeneratedImagesForSampling(VkCommandBuffer cmd) const
{
  imageBarrier(cmd,
               m_dfgLUT.image,
               colorRange(1, 1),
               VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  imageBarrier(cmd,
               m_irradianceMap.image,
               colorRange(1, 6),
               VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  imageBarrier(cmd,
               m_prefilteredMap.image,
               colorRange(m_maxMipLevel + 1, 6),
               VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void IBLResources::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    *this = IBLResources{};
    return;
  }

  for(rhi::TextureViewHandle view : m_generationMipViews)
  {
    m_rhiDevice->destroyTextureView(view);
  }
  if(m_prefilterGenerationPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(m_device, m_prefilterGenerationPipeline, nullptr);
  if(m_irradianceGenerationPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(m_device, m_irradianceGenerationPipeline, nullptr);
  if(m_dfgGenerationPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(m_device, m_dfgGenerationPipeline, nullptr);
  if(m_lutGenerationPipelineLayout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(m_device, m_lutGenerationPipelineLayout, nullptr);
  if(m_envGenerationPipelineLayout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(m_device, m_envGenerationPipelineLayout, nullptr);
  if(m_lutGenerationSetLayout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(m_device, m_lutGenerationSetLayout, nullptr);
  if(m_envGenerationSetLayout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(m_device, m_envGenerationSetLayout, nullptr);
  if(m_generationDescriptorPool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(m_device, m_generationDescriptorPool, nullptr);

  m_rhiDevice->destroyTextureView(m_prefilteredMapView);
  m_rhiDevice->destroyTextureView(m_irradianceMapView);
  m_rhiDevice->destroyTextureView(m_irradianceStorageView);
  m_rhiDevice->destroyTextureView(m_dfgLUTView);
  if(m_cubeMapSampler != VK_NULL_HANDLE)
    vkDestroySampler(m_device, m_cubeMapSampler, nullptr);
  if(m_lutSampler != VK_NULL_HANDLE)
    vkDestroySampler(m_device, m_lutSampler, nullptr);

  if(m_prefilteredMap.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_prefilteredMap.image, m_prefilteredMap.allocation);
  if(m_irradianceMap.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_irradianceMap.image, m_irradianceMap.allocation);
  if(m_dfgLUT.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_dfgLUT.image, m_dfgLUT.allocation);

  *this = IBLResources{};
}

}  // namespace demo
