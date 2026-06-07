#include "IBLResources.h"
#include "RHIFormatBridge.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace demo {

namespace {

[[nodiscard]] VkDevice toVkDevice(uintptr_t handle)
{
  return reinterpret_cast<VkDevice>(handle);
}

[[nodiscard]] VmaAllocator toVmaAllocator(uintptr_t handle)
{
  return reinterpret_cast<VmaAllocator>(handle);
}

[[nodiscard]] VkImage toVkImage(uintptr_t handle)
{
  return reinterpret_cast<VkImage>(handle);
}

[[nodiscard]] VmaAllocation toVmaAllocation(uintptr_t handle)
{
  return reinterpret_cast<VmaAllocation>(handle);
}

[[nodiscard]] VkShaderModule toVkShaderModule(uintptr_t handle)
{
  return reinterpret_cast<VkShaderModule>(handle);
}

[[nodiscard]] VkFormat toVkFormat(rhi::TextureFormat format)
{
  switch(format)
  {
    case rhi::TextureFormat::rgba8Unorm:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::TextureFormat::bgra8Unorm:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case rhi::TextureFormat::rgba16Sfloat:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case rhi::TextureFormat::rg16Sfloat:
      return VK_FORMAT_R16G16_SFLOAT;
    case rhi::TextureFormat::r32Sfloat:
      return VK_FORMAT_R32_SFLOAT;
    case rhi::TextureFormat::d16Unorm:
      return VK_FORMAT_D16_UNORM;
    case rhi::TextureFormat::d32Sfloat:
      return VK_FORMAT_D32_SFLOAT;
    case rhi::TextureFormat::d24UnormS8:
      return VK_FORMAT_D24_UNORM_S8_UINT;
    case rhi::TextureFormat::d32SfloatS8:
      return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case rhi::TextureFormat::undefined:
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

void imageBarrier(rhi::CommandBuffer& cmd,
                  rhi::TextureHandle image,
                  const rhi::TextureSubresourceRange& range,
                  rhi::ResourceState before,
                  rhi::ResourceState after)
{
  const rhi::TextureBarrier barrier{
      .texture = image,
      .before = before,
      .after = after,
      .range = range,
  };
  cmd.resourceBarrier(&barrier, 1, nullptr, 0);
}

rhi::TextureSubresourceRange colorRange(uint32_t mipCount, uint32_t layerCount)
{
  return rhi::TextureSubresourceRange{
      .aspect = rhi::TextureAspect::color,
      .baseMipLevel = 0,
      .levelCount = mipCount,
      .baseArrayLayer = 0,
      .layerCount = layerCount,
  };
}

}  // namespace

void IBLResources::init(rhi::Device& device, uintptr_t backendAllocatorToken, rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo)
{
  deinit();
  m_rhiDevice = &device;
  m_backendDeviceToken = static_cast<uintptr_t>(device.getBackendDeviceHandle());
  m_backendAllocatorToken = backendAllocatorToken;
  m_cubeMapSize = createInfo.cubeMapSize;
  m_dfgLUTSize = createInfo.dfgLUTSize;

  m_cubeMapSampler = m_rhiDevice->createSampler(rhi::SamplerDesc{
      .magFilter    = rhi::Filter::linear,
      .minFilter    = rhi::Filter::linear,
      .mipmapMode   = rhi::MipmapMode::linear,
      .addressModeU = rhi::AddressMode::clampToEdge,
      .addressModeV = rhi::AddressMode::clampToEdge,
      .addressModeW = rhi::AddressMode::clampToEdge,
      .maxLod       = VK_LOD_CLAMP_NONE,
      .debugName    = "IBL_CubeMapSampler",
  });
  m_lutSampler = m_rhiDevice->createSampler(rhi::SamplerDesc{
      .magFilter    = rhi::Filter::linear,
      .minFilter    = rhi::Filter::linear,
      .addressModeU = rhi::AddressMode::clampToEdge,
      .addressModeV = rhi::AddressMode::clampToEdge,
      .addressModeW = rhi::AddressMode::clampToEdge,
      .debugName    = "IBL_LUTSampler",
  });

  createImages(rhiCmd, createInfo);
  generateIBLMaps(rhiCmd, createInfo);
  transitionGeneratedImagesForSampling(rhiCmd);
}

void IBLResources::createImages(rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo)
{
  const VmaAllocator allocator = toVmaAllocator(m_backendAllocatorToken);
  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();
  m_maxMipLevel = static_cast<uint32_t>(std::floor(std::log2(std::max(createInfo.cubeMapSize, 1u))));

  const auto makeView = [&](VkImage image, rhi::TextureFormat format, rhi::ImageViewType viewType, uint32_t levelCount,
                            uint32_t layerCount, const char* name) -> rhi::TextureViewHandle {
    const rhi::TextureHandle imageHandle = m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(image));
    rhi::TextureViewCreateDesc desc{};
    desc.image        = imageHandle;
    desc.format       = format;
    desc.viewType     = viewType;
    desc.aspect       = rhi::TextureAspect::color;
    desc.levelCount   = levelCount;
    desc.layerCount   = layerCount;
    const rhi::TextureViewHandle handle = m_rhiDevice->createTextureView(desc);
    m_rhiDevice->destroyImage(imageHandle);
    dutil.setObjectName(reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_rhiDevice->resolveTextureViewBackendHandle(handle))),
                        name);
    return handle;
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  const VkImageCreateInfo cubeInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = toVkFormat(createInfo.cubeMapFormat),
      .extent = {createInfo.cubeMapSize, createInfo.cubeMapSize, 1},
      .mipLevels = m_maxMipLevel + 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };

  VkImage prefilteredMap = VK_NULL_HANDLE;
  VmaAllocation prefilteredAllocation = nullptr;
  VK_CHECK(vmaCreateImage(allocator, &cubeInfo, &imageAllocInfo, &prefilteredMap, &prefilteredAllocation, nullptr));
  m_prefilteredMap = {reinterpret_cast<uintptr_t>(prefilteredMap),
                      reinterpret_cast<uintptr_t>(prefilteredAllocation),
                      m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(prefilteredMap))};
  dutil.setObjectName(prefilteredMap, "IBL_PrefilteredMap");

  m_prefilteredMapView = makeView(prefilteredMap, createInfo.cubeMapFormat, rhi::ImageViewType::eCube,
                                  m_maxMipLevel + 1, 6, "IBL_PrefilteredMapView");

  const VkImageCreateInfo irradianceInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = toVkFormat(createInfo.cubeMapFormat),
      .extent = {32, 32, 1},
      .mipLevels = 1,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VkImage irradianceMap = VK_NULL_HANDLE;
  VmaAllocation irradianceAllocation = nullptr;
  VK_CHECK(vmaCreateImage(allocator, &irradianceInfo, &imageAllocInfo, &irradianceMap, &irradianceAllocation, nullptr));
  m_irradianceMap = {reinterpret_cast<uintptr_t>(irradianceMap),
                     reinterpret_cast<uintptr_t>(irradianceAllocation),
                     m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(irradianceMap))};
  dutil.setObjectName(irradianceMap, "IBL_IrradianceMap");

  m_irradianceMapView = makeView(irradianceMap, createInfo.cubeMapFormat, rhi::ImageViewType::eCube, 1, 6,
                                 "IBL_IrradianceMapView");

  m_irradianceStorageView = makeView(irradianceMap, createInfo.cubeMapFormat, rhi::ImageViewType::e2DArray, 1, 6,
                                     "IBL_IrradianceStorageView");

  const VkImageCreateInfo lutInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = toVkFormat(createInfo.dfgLUTFormat),
      .extent = {createInfo.dfgLUTSize, createInfo.dfgLUTSize, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };
  VkImage dfgLut = VK_NULL_HANDLE;
  VmaAllocation dfgLutAllocation = nullptr;
  VK_CHECK(vmaCreateImage(allocator, &lutInfo, &imageAllocInfo, &dfgLut, &dfgLutAllocation, nullptr));
  m_dfgLUT = {reinterpret_cast<uintptr_t>(dfgLut),
              reinterpret_cast<uintptr_t>(dfgLutAllocation),
              m_rhiDevice->registerExternalTexture(reinterpret_cast<uint64_t>(dfgLut))};
  dutil.setObjectName(dfgLut, "IBL_DFGLUT");

  m_dfgLUTView = makeView(dfgLut, createInfo.dfgLUTFormat, rhi::ImageViewType::e2D, 1, 1, "IBL_DFGLUTView");

  const rhi::TextureSubresourceRange cubeRange = colorRange(m_maxMipLevel + 1, 6);
  const rhi::TextureSubresourceRange irradianceRange = colorRange(1, 6);
  const rhi::TextureSubresourceRange lutRange = colorRange(1, 1);
  imageBarrier(rhiCmd, m_prefilteredMap.handle, cubeRange, rhi::ResourceState::Undefined, rhi::ResourceState::General);
  imageBarrier(rhiCmd, m_irradianceMap.handle, irradianceRange, rhi::ResourceState::Undefined, rhi::ResourceState::General);
  imageBarrier(rhiCmd, m_dfgLUT.handle, lutRange, rhi::ResourceState::Undefined, rhi::ResourceState::General);
  rhiCmd.clearColorTexture(m_prefilteredMap.handle, cubeRange, rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f});
  rhiCmd.clearColorTexture(m_irradianceMap.handle, irradianceRange, rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f});
  rhiCmd.clearColorTexture(m_dfgLUT.handle, lutRange, rhi::ClearColorValue{0.5f, 0.5f, 0.0f, 1.0f});
}

rhi::PipelineHandle IBLResources::createGenerationPipeline(uintptr_t shaderModule,
                                                           const char* entryPoint,
                                                           rhi::ArgumentLayoutHandle layout,
                                                           uint32_t variant) const
{
  const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{layout}};
  const std::array<rhi::PipelinePushConstantRange, 1> pushConstants{{
      rhi::PipelinePushConstantRange{.stages = rhi::ShaderStage::compute,
                                     .offset = 0,
                                     .size   = sizeof(GeneratePushConstants)},
  }};
  const rhi::ComputePipelineDesc desc{
      .shaderStage =
          rhi::PipelineShaderStageDesc{
              .stage        = rhi::ShaderStage::compute,
              .shaderModule = shaderModule,
              .entryPoint   = entryPoint,
          },
      .argumentLayouts = argumentLayouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
      .pushConstantRanges = pushConstants.data(),
      .pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size()),
      .specializationVariant = variant,
  };
  return m_rhiDevice->createComputePipeline(desc);
}

void IBLResources::generateIBLMaps(rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo)
{
  const VkDevice device = toVkDevice(m_backendDeviceToken);
#ifdef USE_SLANG
  const auto dispatchGeneration = [&](rhi::PipelineHandle pipeline,
                                      rhi::ArgumentTableHandle table,
                                      const GeneratePushConstants& constants,
                                      uint32_t groupCountX,
                                      uint32_t groupCountY,
                                      uint32_t groupCountZ) -> bool {
    if(pipeline.isNull() || table.isNull())
    {
      return false;
    }
    rhi::ComputeEncoder* encoder = rhiCmd.beginComputePass();
    if(encoder == nullptr)
    {
      return false;
    }
    encoder->setPipeline(pipeline);
    encoder->setArgumentTable(0, table);
    encoder->setRootConstants(0, &constants, sizeof(constants));
    encoder->dispatch(rhi::DispatchDesc{.groupCountX = groupCountX, .groupCountY = groupCountY, .groupCountZ = groupCountZ});
    rhiCmd.endEncoding();
    return true;
  };
  const auto createGenerationTable = [&](rhi::ArgumentLayoutHandle layout,
                                         const rhi::ArgumentWrite* writes,
                                         uint32_t writeCount) -> rhi::ArgumentTableHandle {
    const rhi::ArgumentTableHandle table = m_rhiDevice->createArgumentTable(layout);
    m_generationArgumentTables.push_back(table);
    if(writeCount > 0)
    {
      m_rhiDevice->updateArgumentTable(table, writeCount, writes);
    }
    return table;
  };
  const uint32_t prefilterMipCount = m_maxMipLevel + 1u;

  const std::array<rhi::ArgumentBinding, 2> envToCubeBindings{{
      rhi::ArgumentBinding{.binding = 0, .type = rhi::ArgumentType::combinedImageSampler, .visibility = rhi::ShaderStage::compute},
      rhi::ArgumentBinding{.binding = 1, .type = rhi::ArgumentType::storageTexture, .visibility = rhi::ShaderStage::compute},
  }};
  m_envGenerationArgumentLayout = m_rhiDevice->createArgumentLayout(rhi::ArgumentLayoutDesc{
      .bindings = envToCubeBindings.data(), .bindingCount = static_cast<uint32_t>(envToCubeBindings.size()),
      .debugName = "ibl-env-generation"});

  const rhi::ArgumentBinding lutBinding{.binding = 0, .type = rhi::ArgumentType::storageTexture, .visibility = rhi::ShaderStage::compute};
  m_lutGenerationArgumentLayout = m_rhiDevice->createArgumentLayout(rhi::ArgumentLayoutDesc{
      .bindings = &lutBinding, .bindingCount = 1, .debugName = "ibl-lut-generation"});

  VkShaderModule dfgModule = utils::createShaderModule(device, {shader_ibl_dfg_slang, std::size(shader_ibl_dfg_slang)});
  m_dfgGenerationPipeline =
      createGenerationPipeline(reinterpret_cast<uintptr_t>(dfgModule), "dfgLUTMain", m_lutGenerationArgumentLayout, 0x7201u);
  vkDestroyShaderModule(device, dfgModule, nullptr);

  const rhi::ArgumentWrite dfgWrite{
      .binding = 0, .type = rhi::ArgumentType::storageTexture, .textureView = m_dfgLUTView,
      .accessIntent = rhi::ArgumentAccessIntent::readWrite};
  const rhi::ArgumentTableHandle dfgTable = createGenerationTable(m_lutGenerationArgumentLayout, &dfgWrite, 1);

  GeneratePushConstants push{
      .width = createInfo.dfgLUTSize,
      .height = createInfo.dfgLUTSize,
      .sampleCount = createInfo.brdfSampleCount,
  };
  if(!dispatchGeneration(m_dfgGenerationPipeline,
                         dfgTable,
                         push,
                         (createInfo.dfgLUTSize + 7u) / 8u,
                         (createInfo.dfgLUTSize + 7u) / 8u,
                         1u))
  {
    return;
  }

  VkShaderModule irradianceModule = VK_NULL_HANDLE;
  VkShaderModule prefilterModule = VK_NULL_HANDLE;

  if(!createInfo.sourceEnvironmentView.isNull())
  {
    irradianceModule = utils::createShaderModule(device, {shader_ibl_irradiance_slang, std::size(shader_ibl_irradiance_slang)});
    prefilterModule = utils::createShaderModule(device, {shader_ibl_prefilter_slang, std::size(shader_ibl_prefilter_slang)});
    m_irradianceGenerationPipeline =
        createGenerationPipeline(reinterpret_cast<uintptr_t>(irradianceModule), "irradianceConvolutionMain", m_envGenerationArgumentLayout, 0x7202u);
    m_prefilterGenerationPipeline =
        createGenerationPipeline(reinterpret_cast<uintptr_t>(prefilterModule), "prefilterGGXMain", m_envGenerationArgumentLayout, 0x7203u);

    std::array<rhi::ArgumentWrite, 2> writes{{
        rhi::ArgumentWrite{.binding = 0, .type = rhi::ArgumentType::combinedImageSampler,
                           .textureView = createInfo.sourceEnvironmentView, .sampler = m_cubeMapSampler},
        rhi::ArgumentWrite{.binding = 1, .type = rhi::ArgumentType::storageTexture,
                           .textureView = m_irradianceStorageView, .accessIntent = rhi::ArgumentAccessIntent::readWrite},
    }};
    const rhi::ArgumentTableHandle irradianceTable =
        createGenerationTable(m_envGenerationArgumentLayout, writes.data(), static_cast<uint32_t>(writes.size()));

    push = GeneratePushConstants{
        .width = 32,
        .height = 32,
        .sourceWidth = createInfo.sourceWidth,
        .sourceHeight = createInfo.sourceHeight,
        .sourceMaxMip = std::max(1u, createInfo.sourceMipCount) - 1u,
        .sampleCount = createInfo.irradianceSampleCount,
    };
    if(!dispatchGeneration(m_irradianceGenerationPipeline,
                           irradianceTable,
                           push,
                           (push.width + 7u) / 8u,
                           (push.height + 7u) / 8u,
                           6u))
    {
      return;
    }

    imageBarrier(rhiCmd, m_irradianceMap.handle, colorRange(1, 6), rhi::ResourceState::General, rhi::ResourceState::General);

    for(uint32_t mip = 0; mip <= m_maxMipLevel; ++mip)
    {
      const uint32_t mipSize = std::max(1u, createInfo.cubeMapSize >> mip);
      rhi::TextureViewCreateDesc mipViewDesc{};
      mipViewDesc.image         = m_prefilteredMap.handle;
      mipViewDesc.format        = createInfo.cubeMapFormat;
      mipViewDesc.viewType      = rhi::ImageViewType::e2DArray;
      mipViewDesc.aspect        = rhi::TextureAspect::color;
      mipViewDesc.baseMipLevel  = mip;
      mipViewDesc.levelCount    = 1;
      mipViewDesc.baseArrayLayer = 0;
      mipViewDesc.layerCount    = 6;
      const rhi::TextureViewHandle mipView = m_rhiDevice->createTextureView(mipViewDesc);
      m_generationMipViews.push_back(mipView);
      writes = {{
          rhi::ArgumentWrite{.binding = 0, .type = rhi::ArgumentType::combinedImageSampler,
                             .textureView = createInfo.sourceEnvironmentView, .sampler = m_cubeMapSampler},
          rhi::ArgumentWrite{.binding = 1, .type = rhi::ArgumentType::storageTexture,
                             .textureView = mipView, .accessIntent = rhi::ArgumentAccessIntent::readWrite},
      }};
      const rhi::ArgumentTableHandle prefilterTable =
          createGenerationTable(m_envGenerationArgumentLayout, writes.data(), static_cast<uint32_t>(writes.size()));

      push = GeneratePushConstants{
          .width = mipSize,
          .height = mipSize,
          .sourceWidth = createInfo.sourceWidth,
          .sourceHeight = createInfo.sourceHeight,
          .sourceMaxMip = std::max(1u, createInfo.sourceMipCount) - 1u,
          .sampleCount = createInfo.prefilterSampleCount,
          .roughness = m_maxMipLevel > 0 ? static_cast<float>(mip) / static_cast<float>(m_maxMipLevel) : 0.0f,
      };
      if(!dispatchGeneration(m_prefilterGenerationPipeline,
                             prefilterTable,
                             push,
                             (mipSize + 7u) / 8u,
                             (mipSize + 7u) / 8u,
                             6u))
      {
        return;
      }
    }
    m_splitSumReady = true;
  }

  if(prefilterModule != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, prefilterModule, nullptr);
  if(irradianceModule != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, irradianceModule, nullptr);
#else
  (void)createInfo;
#endif
}

void IBLResources::transitionGeneratedImagesForSampling(rhi::CommandBuffer& rhiCmd) const
{
  imageBarrier(rhiCmd, m_dfgLUT.handle, colorRange(1, 1), rhi::ResourceState::General, rhi::ResourceState::ShaderRead);
  imageBarrier(rhiCmd, m_irradianceMap.handle, colorRange(1, 6), rhi::ResourceState::General, rhi::ResourceState::ShaderRead);
  imageBarrier(rhiCmd, m_prefilteredMap.handle, colorRange(m_maxMipLevel + 1, 6), rhi::ResourceState::General, rhi::ResourceState::ShaderRead);
}

void IBLResources::deinit()
{
  if(m_backendDeviceToken == 0)
  {
    *this = IBLResources{};
    return;
  }

  for(rhi::TextureViewHandle view : m_generationMipViews)
  {
    m_rhiDevice->destroyTextureView(view);
  }
  m_rhiDevice->destroyPipeline(m_prefilterGenerationPipeline);
  m_rhiDevice->destroyPipeline(m_irradianceGenerationPipeline);
  m_rhiDevice->destroyPipeline(m_dfgGenerationPipeline);
  for(rhi::ArgumentTableHandle table : m_generationArgumentTables)
  {
    m_rhiDevice->destroyArgumentTable(table);
  }
  if(!m_lutGenerationArgumentLayout.isNull())
  {
    m_rhiDevice->destroyArgumentLayout(m_lutGenerationArgumentLayout);
  }
  if(!m_envGenerationArgumentLayout.isNull())
  {
    m_rhiDevice->destroyArgumentLayout(m_envGenerationArgumentLayout);
  }

  m_rhiDevice->destroyTextureView(m_prefilteredMapView);
  m_rhiDevice->destroyTextureView(m_irradianceMapView);
  m_rhiDevice->destroyTextureView(m_irradianceStorageView);
  m_rhiDevice->destroyTextureView(m_dfgLUTView);
  m_rhiDevice->destroySampler(m_cubeMapSampler);
  m_rhiDevice->destroySampler(m_lutSampler);
  if(!m_prefilteredMap.handle.isNull())
  {
    m_rhiDevice->destroyImage(m_prefilteredMap.handle);
  }
  if(!m_irradianceMap.handle.isNull())
  {
    m_rhiDevice->destroyImage(m_irradianceMap.handle);
  }
  if(!m_dfgLUT.handle.isNull())
  {
    m_rhiDevice->destroyImage(m_dfgLUT.handle);
  }

  const VmaAllocator allocator = toVmaAllocator(m_backendAllocatorToken);
  if(m_prefilteredMap.image != 0)
    vmaDestroyImage(allocator, toVkImage(m_prefilteredMap.image), toVmaAllocation(m_prefilteredMap.allocation));
  if(m_irradianceMap.image != 0)
    vmaDestroyImage(allocator, toVkImage(m_irradianceMap.image), toVmaAllocation(m_irradianceMap.allocation));
  if(m_dfgLUT.image != 0)
    vmaDestroyImage(allocator, toVkImage(m_dfgLUT.image), toVmaAllocation(m_dfgLUT.allocation));

  *this = IBLResources{};
}

}  // namespace demo
