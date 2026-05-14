#include "GPUDrivenRenderer.h"
#include "UploadUtils.h"
#include "../rhi/vulkan/VulkanCommandList.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace demo {

namespace {

constexpr bool kEnableExperimentalMeshletPath = false;
constexpr bool kEnableShippingVisibilitySort = true;
constexpr uint32_t kDebugCullSegmentCount = 24u;
constexpr uint32_t kLightCoarseCullingThreadCount = 64u;
constexpr uint32_t kVisibilitySortCategoryMask = 0xc0000000u;
constexpr uint32_t kVisibilitySortKeyMask = 0x3fffffffu;
constexpr uint32_t kVisibilitySortCategoryOpaque = 0x00000000u;
constexpr uint32_t kVisibilitySortCategoryAlpha = 0x40000000u;
constexpr uint32_t kVisibilitySortCategoryTransparent = 0x80000000u;

uint32_t buildGPUDrivenFlags(const MeshRecord& mesh)
{
  uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
  if(mesh.alphaMode == shaderio::LAlphaBlend)
  {
    flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
  }
  else if(mesh.alphaMode == shaderio::LAlphaMask)
  {
    flags |= shaderio::LGPUCullFlagAlphaMask;
  }
  return flags;
}

utils::Buffer createHostVisibleStorageBuffer(VkDevice device, VmaAllocator allocator, VkDeviceSize size)
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR
               | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };

  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

  utils::Buffer buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;

  const VkBufferDeviceAddressInfo addressInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = buffer.buffer,
  };
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  return buffer;
}

utils::Buffer createDeviceLocalStorageBuffer(VkDevice device, VmaAllocator allocator, VkDeviceSize size)
{
  return upload::createStaticBuffer(device,
                                    allocator,
                                    size,
                                    VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR
                                        | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR);
}

void destroyBuffer(VmaAllocator allocator, utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

uint32_t nextPowerOfTwo(uint32_t value)
{
  if(value <= 1u)
  {
    return 1u;
  }

  --value;
  value |= value >> 1u;
  value |= value >> 2u;
  value |= value >> 4u;
  value |= value >> 8u;
  value |= value >> 16u;
  return value + 1u;
}

uint32_t computeBitonicSortPassCount(uint32_t visibleCount)
{
  uint32_t passes = 0;
  for(uint32_t width = 2; width <= std::max(visibleCount, 1u); width <<= 1u)
  {
    for(uint32_t stride = width >> 1u; stride > 0; stride >>= 1u)
    {
      ++passes;
    }
  }
  return passes;
}

uint32_t encodeSortableFloatKey(float value)
{
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint32_t encodeVisibilitySortKey(uint32_t categoryValue, uint32_t subKey)
{
  return categoryValue | (subKey & kVisibilitySortKeyMask);
}

constexpr uint32_t kMaxReasonableGPUDrivenObjectCount = 1u << 20;

[[nodiscard]] rhi::TextureAspect sceneDepthAspect(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return rhi::TextureAspect::depthStencil;
    default:
      return rhi::TextureAspect::depth;
  }
}

}  // namespace

void GPUDrivenRenderer::init(GLFWwindow* window, rhi::Surface& surface, bool vSync)
{
  m_renderer.init(window, surface, vSync);
  m_sceneRegistry.init(getNativeDeviceHandle(), getAllocatorHandle());
  m_enableExperimentalMeshletPath = kEnableExperimentalMeshletPath;
  if(m_enableExperimentalMeshletPath)
  {
    m_meshletBuffer.init(getNativeDeviceHandle(), getAllocatorHandle());
  }
  m_hiZDepthPyramid.init(getNativeDeviceHandle(), getAllocatorHandle(), getSwapchainImageCount(), getSceneExtent());

  m_depthPrepass = std::make_unique<GPUDrivenDepthPrepass>(this);
  m_depthPyramidPass = std::make_unique<GPUDrivenDepthPyramidPass>(this);
  m_gpuCullingPass = std::make_unique<GPUDrivenCullingPass>(this);
  if(kEnableShippingVisibilitySort)
  {
    m_visibilitySortPass = std::make_unique<GPUDrivenVisibilitySortPass>(this);
  }
  m_lightCullingPass = std::make_unique<GPUDrivenLightCullingPass>(this);
  m_csmShadowPass = std::make_unique<GPUDrivenCSMShadowPass>(this);
  m_gbufferPass = std::make_unique<GPUDrivenGBufferPass>(this);
  m_lightPass = std::make_unique<GPUDrivenLightPass>(this);
  m_forwardPass = std::make_unique<GPUDrivenForwardPass>(this);
  m_debugPass = std::make_unique<GPUDrivenDebugPass>(this);
  m_presentPass = std::make_unique<GPUDrivenPresentPass>(this);
  m_imguiPass = std::make_unique<GPUDrivenImguiPass>(this);

  m_passExecutor.clear();
  m_passExecutor.addPass(*m_depthPrepass);
  m_passExecutor.addPass(*m_depthPyramidPass);
  m_passExecutor.addPass(*m_gpuCullingPass);
  if(m_visibilitySortPass != nullptr)
  {
    m_passExecutor.addPass(*m_visibilitySortPass);
  }
  m_passExecutor.addPass(*m_lightCullingPass);
  m_passExecutor.addPass(*m_csmShadowPass);
  m_passExecutor.addPass(*m_gbufferPass);
  m_passExecutor.addPass(*m_lightPass);
  m_passExecutor.addPass(*m_forwardPass);
  m_passExecutor.addPass(*m_debugPass);
  m_passExecutor.addPass(*m_presentPass);
  m_passExecutor.addPass(*m_imguiPass);
  bindStaticPassResources();
  m_passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_hiZDepthPyramid.getImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
  initTransparentVisibilityPatchResources();
  m_sortedBootstrapFrames.assign(std::max(1u, getSwapchainImageCount()), SortedBootstrapFrameState{});
  if(kEnableShippingVisibilitySort)
  {
    initVisibilitySortResources();
  }
}

void GPUDrivenRenderer::shutdown(rhi::Surface& surface)
{
  if(kEnableShippingVisibilitySort)
  {
    shutdownVisibilitySortResources();
  }
  shutdownTransparentVisibilityPatchResources();
  m_sortedBootstrapFrames.clear();
  m_passExecutor.clear();
  m_imguiPass.reset();
  m_presentPass.reset();
  m_debugPass.reset();
  m_forwardPass.reset();
  m_lightPass.reset();
  m_gbufferPass.reset();
  m_csmShadowPass.reset();
  m_lightCullingPass.reset();
  if(m_visibilitySortPass != nullptr)
  {
    m_visibilitySortPass.reset();
  }
  m_gpuCullingPass.reset();
  m_depthPyramidPass.reset();
  m_depthPrepass.reset();
  if(m_enableExperimentalMeshletPath)
  {
    m_meshletBuffer.deinit();
  }
  m_hiZDepthPyramid.shutdown();
  m_sceneRegistry.deinit();
  m_renderer.shutdown(surface);
}

void GPUDrivenRenderer::resize(rhi::Extent2D size)
{
  m_renderer.resize(size);
  m_hiZDepthPyramid.resize(getSceneExtent());
  m_passExecutor.clearResourceBindings();
  bindStaticPassResources();
  m_passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_hiZDepthPyramid.getImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
}

void GPUDrivenRenderer::render(const RenderParams& params)
{
  const bool sceneRenderingSuspended = m_suspendSceneRendering;
  m_hiZDepthPyramid.resize(getSceneExtent());
  flushPendingSceneUploads();
  refreshSceneView();
  uploadPersistentDrawData();
  const uint32_t safeObjectCount = getSafePersistentObjectCount();
  const shaderio::GPUCullDrawCounts cachedDrawCounts = getLastGPUCullingDrawCounts();
  const uint32_t visibleCount = cachedDrawCounts.totalCount;
  m_runtimeStats.batchStats.visibleCount = visibleCount;
  m_runtimeStats.batchStats.batchCount =
      static_cast<uint32_t>((cachedDrawCounts.opaqueCount > 0u ? 1u : 0u)
                            + (cachedDrawCounts.alphaTestCount > 0u ? 1u : 0u)
                            + (cachedDrawCounts.transparentCount > 0u ? 1u : 0u));
  m_runtimeStats.batchStats.sortPassCount = 0u;
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
  if(kEnableShippingVisibilitySort && !sceneRenderingSuspended)
  {
    m_visibilitySortInputObjects.clear();
    m_visibilitySortInputKeys.clear();
    const glm::vec3 cameraPos =
        params.cameraUniforms != nullptr ? params.cameraUniforms->cameraPosition : glm::vec3(0.0f);
    const size_t totalSortInputCount =
        m_opaqueDrawIndices.size() + m_alphaTestDrawIndices.size() + m_transparentDrawIndices.size();
    m_visibilitySortInputObjects.reserve(totalSortInputCount);
    m_visibilitySortInputKeys.reserve(totalSortInputCount);
    const auto appendSortedDraws = [&](std::span<const uint32_t> drawIndices, uint32_t categoryValue, bool sortByDistance) {
      for(uint32_t drawIndex : drawIndices)
      {
        MeshHandle meshHandle = kNullMeshHandle;
        if(!tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
        {
          continue;
        }

        const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
        if(mesh == nullptr)
        {
          continue;
        }

        uint32_t subKey = kVisibilitySortKeyMask;
        if(sortByDistance)
        {
          const glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
          const float distanceSquared = glm::dot(meshCenter - cameraPos, meshCenter - cameraPos);
          subKey = encodeSortableFloatKey(distanceSquared) >> 2u;
        }
        else if(mesh->materialIndex >= 0)
        {
          subKey = std::min(static_cast<uint32_t>(mesh->materialIndex), kVisibilitySortKeyMask);
        }

        m_visibilitySortInputObjects.push_back(drawIndex);
        m_visibilitySortInputKeys.push_back(encodeVisibilitySortKey(categoryValue, subKey));
      }
    };
    appendSortedDraws(m_opaqueDrawIndices, kVisibilitySortCategoryOpaque, false);
    appendSortedDraws(m_alphaTestDrawIndices, kVisibilitySortCategoryAlpha, false);
    appendSortedDraws(m_transparentDrawIndices, kVisibilitySortCategoryTransparent, true);
    m_runtimeStats.batchStats.sortPassCount =
        computeBitonicSortPassCount(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
    for(uint32_t sortFrameIndex = 0; sortFrameIndex < static_cast<uint32_t>(m_visibilitySortFrames.size()); ++sortFrameIndex)
    {
      prepareVisibilitySortInputs(sortFrameIndex);
    }
    if(frameIndex < m_visibilitySortFrames.size())
    {
      const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
      m_passExecutor.bindBuffer({
          .handle       = kPassGPUDrivenSortKeyBufferHandle,
          .nativeBuffer = reinterpret_cast<uint64_t>(sortResources.keyBuffer.buffer),
      });
      m_passExecutor.bindBuffer({
          .handle       = kPassGPUDrivenSortValueBufferHandle,
          .nativeBuffer = reinterpret_cast<uint64_t>(sortResources.valueBuffer.buffer),
      });
    }
  }
  else
  {
    m_visibilitySortInputObjects.clear();
    m_visibilitySortInputKeys.clear();
    m_passExecutor.bindBuffer({
        .handle       = kPassGPUDrivenSortKeyBufferHandle,
        .nativeBuffer = 0,
    });
    m_passExecutor.bindBuffer({
        .handle       = kPassGPUDrivenSortValueBufferHandle,
        .nativeBuffer = 0,
    });
  }

  RenderParams gpuParams = params;
  gpuParams.gpuDrivenSceneView =
      (!sceneRenderingSuspended && m_sceneView.usePersistentCullingObjects) ? &m_sceneView : nullptr;
  if(sceneRenderingSuspended || gpuParams.gpuDrivenSceneView != nullptr)
  {
    gpuParams.gltfModel = nullptr;
  }
  submitPassGraph(gpuParams);
  const VkDescriptorSet gpuCullingDescriptorSet =
      reinterpret_cast<VkDescriptorSet>(getGPUCullingDescriptorSetOpaque(frameIndex));
  m_hiZDepthPyramid.bindForCulling(gpuCullingDescriptorSet, 5);
  m_sceneView.depthPyramidImage = m_hiZDepthPyramid.getImage();
  m_sceneView.depthPyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
  m_sceneView.depthPyramidMipCount = m_hiZDepthPyramid.getMipCount();
  m_sceneView.depthPyramidSourceDepth = m_hiZDepthPyramid.getSourceDepth();
  m_sceneView.depthPyramidGeneration = m_hiZDepthPyramid.getGenerationCount();
  m_sceneView.depthPyramidValid = m_hiZDepthPyramid.isValid();
  m_runtimeStats.indirectDrawCount = m_runtimeStats.batchStats.visibleCount;
  m_runtimeStats.indirectCommandStride = getGPUCullingIndirectCommandStride();
  m_runtimeStats.ownsFullRenderChain = true;
  m_runtimeStats.hiZGeneration = m_hiZDepthPyramid.getGenerationCount();
  m_runtimeStats.ownsHiZVisibilityChain =
      m_hiZDepthPyramid.isValid()
      && m_hiZDepthPyramid.getSourceDepth().index == kPassSceneDepthHandle.index
      && m_hiZDepthPyramid.getSourceDepth().generation == kPassSceneDepthHandle.generation
      && m_hiZDepthPyramid.getLastBoundSet() == gpuCullingDescriptorSet
      && m_hiZDepthPyramid.getLastBoundBinding() == 5;
  if(m_sceneView.usePersistentCullingObjects
     && getGPUCullingObjectCount(frameIndex) == safeObjectCount && safeObjectCount > 0u)
  {
    m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
  }
}

void GPUDrivenRenderer::executeDepthPyramidPass(rhi::CommandList& cmd, const RenderParams&)
{
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  const VkImage sourceDepthImage = m_sceneView.sceneDepthImage;
  const VkImageView sourceDepthView = m_sceneView.sceneDepthView;
  const VkExtent2D sourceDepthExtent = m_sceneView.sceneDepthExtent;
  m_hiZDepthPyramid.generate(frameIndex,
                             rhi::vulkan::getNativeCommandBuffer(cmd),
                             sourceDepthExtent,
                             sourceDepthImage,
                             sourceDepthView,
                             kPassSceneDepthHandle);
  m_passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_hiZDepthPyramid.getImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
}

void GPUDrivenRenderer::executeGPUCullingPass(rhi::CommandList& cmd, const RenderParams& params)
{
  const uint32_t safeObjectCount = getSafePersistentObjectCount();
  const bool useExternalPersistentObjects = params.gpuDrivenSceneView != nullptr
                                            && params.gpuDrivenSceneView->usePersistentCullingObjects
                                            && params.gpuDrivenSceneView->gpuCullObjectBuffer != VK_NULL_HANDLE
                                            && safeObjectCount > 0u;

  if(params.cameraUniforms == nullptr || getGPUCullingPipelineHandle().isNull()
     || getGPUCullingPipelineLayout() == 0)
  {
    return;
  }

  const uint32_t currentFrameIndex = getCurrentFrameIndexHint();
  const uint32_t objectCount = useExternalPersistentObjects
                                   ? safeObjectCount
                                   : (params.gltfModel != nullptr ? static_cast<uint32_t>(params.gltfModel->meshes.size()) : 0u);
  const VkDescriptorSet descriptorSet = reinterpret_cast<VkDescriptorSet>(getGPUCullingDescriptorSetOpaque(currentFrameIndex));
  const VkBuffer indirectBuffer = reinterpret_cast<VkBuffer>(getGPUCullingIndirectBufferOpaque(currentFrameIndex));
  if(objectCount == 0u || descriptorSet == VK_NULL_HANDLE || indirectBuffer == VK_NULL_HANDLE)
  {
    return;
  }
   
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  vkCmdBindPipeline(vkCmd,
                    VK_PIPELINE_BIND_POINT_COMPUTE,
                    reinterpret_cast<VkPipeline>(getNativeComputePipeline(getGPUCullingPipelineHandle())));
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          reinterpret_cast<VkPipelineLayout>(getGPUCullingPipelineLayout()),
                          0,
                          1,
                          &descriptorSet,
                          0,
                          nullptr);
  vkCmdDispatch(vkCmd, (objectCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount, 1u, 1u);

  const VkBufferMemoryBarrier2 indirectBarrier{
      .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
      .dstAccessMask       = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer              = indirectBuffer,
      .offset              = 0,
      .size                = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &indirectBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void GPUDrivenRenderer::executeLightCullingPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(params.cameraUniforms == nullptr || getLightCullingPipelineLayout() == 0)
  {
    return;
  }

  const uint32_t currentFrameIndex = getCurrentFrameIndexHint();
  const VkDescriptorSet descriptorSet = reinterpret_cast<VkDescriptorSet>(getCurrentLightCullingDescriptorSet());
  if(descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  const shaderio::CameraUniforms& camera = *params.cameraUniforms;
  const glm::mat4 inverseView = glm::inverse(camera.view);
  const VkExtent2D extent = getSceneExtent();
  const uint32_t tileCountX = (extent.width + shaderio::LTileSizeX - 1u) / shaderio::LTileSizeX;
  const uint32_t tileCountY = (extent.height + shaderio::LTileSizeY - 1u) / shaderio::LTileSizeY;
  const uint32_t pointLightCount = getActivePointLightCount();
  const uint32_t spotLightCount = getActiveSpotLightCount();
  const shaderio::LightCoarseCullingUniforms coarseCullingUniforms{
      .viewProjection = camera.viewProjection,
      .cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f),
      .cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f),
      .screenTileInfo = glm::vec4(extent.width, extent.height, tileCountX, tileCountY),
      .lightCountInfo = glm::vec4(pointLightCount, spotLightCount, 0.0f, 0.0f),
      .debugInfo = glm::vec4(params.debugOptions.showLightCoarseCullingHeatmap ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f),
  };
  updateLightCoarseCullingResources(currentFrameIndex, coarseCullingUniforms);

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          reinterpret_cast<VkPipelineLayout>(getLightCullingPipelineLayout()),
                          0,
                          1,
                          &descriptorSet,
                          0,
                          nullptr);

  const auto dispatchLightKernel = [&](PipelineHandle pipelineHandle, uint32_t lightCount) {
    if(pipelineHandle.isNull() || lightCount == 0u)
    {
      return;
    }

    vkCmdBindPipeline(vkCmd,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      reinterpret_cast<VkPipeline>(getNativeComputePipeline(pipelineHandle)));
    vkCmdDispatch(vkCmd, (lightCount + kLightCoarseCullingThreadCount - 1u) / kLightCoarseCullingThreadCount, 1u, 1u);
  };

  dispatchLightKernel(getLightCullingPipelineHandle(), pointLightCount);
  dispatchLightKernel(getSpotLightCullingPipelineHandle(), spotLightCount);

  const VkMemoryBarrier2 memoryBarrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void GPUDrivenRenderer::executeCSMShadowPass(const PassContext& context)
{
  if(context.params == nullptr || context.transientAllocator == nullptr || context.cmd == nullptr
     || !m_sceneView.usePersistentCullingObjects || m_sceneView.shadowPackedMeshes == nullptr
     || m_sceneView.shadowPackedMeshCount == 0 || m_sceneView.shadowPackedVertexBuffer == VK_NULL_HANDLE
     || m_sceneView.shadowPackedIndexBuffer == VK_NULL_HANDLE)
  {
    return;
  }

  shaderio::ShadowUniforms* shadowData = getShadowUniformsData();
  if(shadowData == nullptr)
  {
    return;
  }

  const PipelineHandle csmPipeline = getCSMShadowPipelineHandle();
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(getCSMShadowPipelineLayout());
  if(csmPipeline.isNull() || pipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(getNativeGraphicsPipeline(csmPipeline));
  if(nativePipeline == VK_NULL_HANDLE)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenCSMShadow");

  CSMShadowResources& csm = getCSMShadowResources();
  const uint32_t cascadeCount = csm.getCascadeCount();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
      .aspect      = rhi::TextureAspect::depth,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(getGraphicsMaterialDescriptorSet());
  const uint32_t frameIndex = context.frameIndex;
  const bool hasShadowIndirectBuffer = getShadowCullingIndirectBufferOpaque(frameIndex) != 0;
  const bool hasDrawBindGroups = !getCSMShadowMDIDrawBindGroup(frameIndex, 0).isNull();
  const uint32_t shadowIndirectCapacity = getShadowCullingMeshCapacity(frameIndex);
  if(!hasShadowIndirectBuffer || !hasDrawBindGroups || shadowIndirectCapacity < m_sceneView.shadowPackedMeshCount)
  {
    if(hasShadowIndirectBuffer && hasDrawBindGroups && shadowIndirectCapacity < m_sceneView.shadowPackedMeshCount)
    {
      LOGW("Skipping GPUDrivenCSMShadow: indirect capacity %u smaller than shadow mesh count %u",
           shadowIndirectCapacity,
           m_sceneView.shadowPackedMeshCount);
    }
    context.cmd->endEvent();
    return;
  }

  const bool useMultiDrawIndirect = context.params->useCsmShadowMultiDrawIndirect
                                    && !getShadowCullingPipelineHandle().isNull()
                                    && getShadowCullingPipelineLayout() != 0
                                    && getShadowCullingDescriptorSetOpaque(frameIndex) != 0;

  for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
  {
    if(useMultiDrawIndirect)
    {
      const VkPipeline computePipeline =
          reinterpret_cast<VkPipeline>(getNativeComputePipeline(getShadowCullingPipelineHandle()));
      const VkPipelineLayout computeLayout = reinterpret_cast<VkPipelineLayout>(getShadowCullingPipelineLayout());
      const VkDescriptorSet computeSet = reinterpret_cast<VkDescriptorSet>(getShadowCullingDescriptorSetOpaque(frameIndex));
      const VkBuffer indirectBuffer = reinterpret_cast<VkBuffer>(getShadowCullingIndirectBufferOpaque(frameIndex));
      if(computePipeline != VK_NULL_HANDLE && computeLayout != VK_NULL_HANDLE && computeSet != VK_NULL_HANDLE
         && indirectBuffer != VK_NULL_HANDLE)
      {
        const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
        vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &computeSet, 0, nullptr);

        const shaderio::ShadowCullPushConstants pushConstants =
            buildShadowCullPushConstants(cascadeIndex, m_sceneView.shadowPackedMeshCount);
        const VkPushConstantsInfo pushInfo{
            .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
            .layout     = computeLayout,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0,
            .size       = sizeof(shaderio::ShadowCullPushConstants),
            .pValues    = &pushConstants,
        };
        rhi::vulkan::cmdPushConstants(*context.cmd, pushInfo);
        vkCmdDispatch(vkCmd,
                      (m_sceneView.shadowPackedMeshCount + shaderio::LGPUCullingThreadCount - 1u)
                          / shaderio::LGPUCullingThreadCount,
                      1u,
                      1u);

        const VkBufferMemoryBarrier2 indirectBarrier{
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask       = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = indirectBuffer,
            .offset              = 0,
            .size                = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo dependencyInfo{
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers    = &indirectBarrier,
        };
        vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
      }
    }

    const VkImageView layerView = csm.getCascadeLayerView(cascadeIndex);
    const VkExtent2D cascadeExtent = csm.getCascadeExtent();
    const rhi::Extent2D extent{cascadeExtent.width, cascadeExtent.height};

    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
        .view = rhi::TextureViewHandle::fromNative(layerView),
        .state = rhi::ResourceState::DepthStencilAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {0.0f, 0},
    };

    const rhi::RenderPassDesc passDesc{
        .renderArea = {{0, 0}, extent},
        .colorTargets = nullptr,
        .colorTargetCount = 0,
        .depthTarget = &depthTarget,
    };
    context.cmd->beginRenderPass(passDesc);
    context.cmd->setViewport(
        rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout,
                            shaderio::LSetTextures,
                            1,
                            &textureSet,
                            0,
                            nullptr);

    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
    shaderio::CameraUniforms cascadeCamera{};
    cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
    cascadeCamera.projection = cascadeCamera.viewProjection;
    cascadeCamera.view = glm::mat4(1.0f);
    cascadeCamera.inverseViewProjection = glm::inverse(cascadeCamera.viewProjection);
    cascadeCamera.cameraPosition = glm::vec3(0.0f);
    const float baseConstantBias = context.params->lightSettings.depthBias;
    const float baseSlopeBias = context.params->lightSettings.normalBias;
    const float biasScale = shadowData->cascadeBiasScale.z;
    const float cascadeBiasScale = 1.0f + static_cast<float>(cascadeIndex) * biasScale;
    const glm::vec3 lightTravelDir = glm::normalize(context.params->lightSettings.direction);
    const glm::vec3 dirToLight = -lightTravelDir;
    cascadeCamera.shadowConstantBias = baseConstantBias * cascadeBiasScale;
    cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, baseSlopeBias * cascadeBiasScale);
    std::memcpy(cameraAlloc.cpuPtr, &cascadeCamera, sizeof(cascadeCamera));
    context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cascadeCamera));

    const BindGroupHandle cameraBindGroupHandle = getCameraBindGroup(frameIndex);
    if(!cameraBindGroupHandle.isNull())
    {
      VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      const uint32_t cameraDynamicOffset = cameraAlloc.offset;
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout,
                              shaderio::LSetScene,
                              1,
                              &cameraDescriptorSet,
                              1,
                              &cameraDynamicOffset);
    }

    const BindGroupHandle drawBindGroupHandle = getCSMShadowMDIDrawBindGroup(frameIndex, cascadeIndex);
    if(!drawBindGroupHandle.isNull())
    {
      VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      rhi::vulkan::cmdBindDescriptorSets(*context.cmd,
                                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         pipelineLayout,
                                         shaderio::LSetDraw,
                                         1,
                                         &drawDescriptorSet,
                                         0,
                                         nullptr);

      constexpr VkDeviceSize vertexOffset = 0;
      constexpr VkDeviceSize indexOffset = 0;
      const VkBuffer vertexBuffer = m_sceneView.shadowPackedVertexBuffer;
      const VkBuffer indexBuffer = m_sceneView.shadowPackedIndexBuffer;
      rhi::vulkan::cmdBindVertexBuffers(*context.cmd, 0, 1, &vertexBuffer, &vertexOffset);
      rhi::vulkan::cmdBindIndexBuffer(*context.cmd, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);
      context.cmd->drawIndexedIndirect(getShadowCullingIndirectBufferOpaque(frameIndex),
                                       0,
                                       m_sceneView.shadowPackedMeshCount,
                                       sizeof(VkDrawIndexedIndirectCommand));
    }

    context.cmd->endRenderPass();
  }

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
      .aspect      = rhi::TextureAspect::depth,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::read,
      .oldState    = rhi::ResourceState::DepthStencilAttachment,
      .newState    = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  context.cmd->endEvent();
}

void GPUDrivenRenderer::executeDebugPass(const PassContext& context)
{
  const uint32_t safeObjectCount = getSafePersistentObjectCount();
  if(context.params == nullptr || context.transientAllocator == nullptr || !context.params->debugOptions.enabled)
  {
    return;
  }

  const std::vector<shaderio::DebugLineVertex>& debugVertices = getDebugLineVertices();
  const bool hasLineDebug = !debugVertices.empty();
  const bool hasGPUCullingDebug =
      context.params->debugOptions.showGPUCullingOverlay && safeObjectCount > 0u
      && !getGPUCullingDebugPipelineHandle().isNull() && getGPUCullingObjectBufferAddress(context.frameIndex) != 0
      && getGPUCullingResultBufferAddress(context.frameIndex) != 0;
  if(!hasLineDebug && !hasGPUCullingDebug)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenDebug");

  const rhi::Extent2D extent{m_sceneView.sceneDepthExtent.width, m_sceneView.sceneDepthExtent.height};
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(m_sceneView.outputView),
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(m_sceneView.outputImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  context.cmd->setViewport(
      rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle debugPipeline = getDebugPipelineHandle();
  const PipelineHandle gpuCullingDebugPipeline = getGPUCullingDebugPipelineHandle();
  if((hasLineDebug && debugPipeline.isNull()) || (hasGPUCullingDebug && gpuCullingDebugPipeline.isNull()))
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(m_sceneView.outputImage),
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::write,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::ColorAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  if(!context.cameraAllocValid)
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(m_sceneView.outputImage),
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::write,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::ColorAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
    const BindGroupHandle cameraBindGroupHandle = getCameraBindGroup(context.frameIndex);
  VkDescriptorSet cameraDescriptorSet = VK_NULL_HANDLE;
  if(!cameraBindGroupHandle.isNull())
  {
    cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  }
  const uint32_t cameraDynamicOffset = cameraAlloc.offset;
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

  if(hasLineDebug)
  {
    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        getNativeGraphicsPipeline(debugPipeline));
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(getGraphicsScenePipelineLayout());
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    if(cameraDescriptorSet != VK_NULL_HANDLE)
    {
      vkCmdBindDescriptorSets(vkCmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout,
                              shaderio::LSetScene,
                              1,
                              &cameraDescriptorSet,
                              1,
                              &cameraDynamicOffset);
    }

    const uint32_t vertexDataSize = static_cast<uint32_t>(debugVertices.size() * sizeof(shaderio::DebugLineVertex));
    const TransientAllocator::Allocation vertexAlloc =
        context.transientAllocator->allocate(vertexDataSize, alignof(shaderio::DebugLineVertex));
    std::memcpy(vertexAlloc.cpuPtr, debugVertices.data(), vertexDataSize);
    context.transientAllocator->flushAllocation(vertexAlloc, vertexDataSize);

    const uint64_t vertexBuffer = context.transientAllocator->getBufferOpaque();
    const uint64_t vertexOffset = vertexAlloc.offset;
    context.cmd->bindVertexBuffers(0, &vertexBuffer, &vertexOffset, 1);
    context.cmd->draw(static_cast<uint32_t>(debugVertices.size()), 1, 0, 0);
  }

  if(hasGPUCullingDebug)
  {
    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        getNativeGraphicsPipeline(gpuCullingDebugPipeline));
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(getDebugPipelineLayout());
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    if(cameraDescriptorSet != VK_NULL_HANDLE)
    {
      vkCmdBindDescriptorSets(vkCmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout,
                              shaderio::LSetScene,
                              1,
                              &cameraDescriptorSet,
                              1,
                              &cameraDynamicOffset);
    }

    const shaderio::PushConstantGPUCullDebug pushValues{
        .objectBufferAddress = getGPUCullingObjectBufferAddress(context.frameIndex),
        .resultBufferAddress = getGPUCullingResultBufferAddress(context.frameIndex),
        .objectCount = safeObjectCount,
        .segmentCount = kDebugCullSegmentCount,
        ._padding0 = 0u,
        ._padding1 = 0u,
    };
    const VkPushConstantsInfo pushInfo{
        .sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
        .layout = pipelineLayout,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(pushValues),
        .pValues = &pushValues,
    };
    rhi::vulkan::cmdPushConstants(*context.cmd, pushInfo);
    context.cmd->draw(pushValues.segmentCount * 2u * 3u, pushValues.objectCount, 0, 0);
  }

  context.cmd->endRenderPass();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(m_sceneView.outputImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::ColorAttachment,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });
  context.cmd->endEvent();
}

void GPUDrivenRenderer::executePresentPass(const PassContext& context)
{
  if(context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenPresent");
  const VkExtent2D srcExtent = m_sceneView.sceneDepthExtent;
  const VkExtent2D dstExtent = getSwapchainExtent();
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
  const VkImage srcImage = m_sceneView.outputImage;
  const VkImage dstImage = getCurrentSwapchainImage();
  if(srcImage == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }

  const float srcAspect = static_cast<float>(srcExtent.width) / static_cast<float>(srcExtent.height);
  const float dstAspect = static_cast<float>(dstExtent.width) / static_cast<float>(dstExtent.height);
  VkOffset3D srcOffset0 = {0, 0, 0};
  VkOffset3D srcOffset1 = {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1};
  int32_t dstY0 = 0;
  int32_t dstY1 = static_cast<int32_t>(dstExtent.height);
  int32_t dstX0 = 0;
  int32_t dstX1 = static_cast<int32_t>(dstExtent.width);

  if(dstAspect > srcAspect)
  {
    const int32_t scaledWidth = static_cast<int32_t>(dstExtent.height * srcAspect);
    const int32_t barWidth = (dstExtent.width - scaledWidth) / 2;
    dstX0 = barWidth;
    dstX1 = barWidth + scaledWidth;
  }
  else if(dstAspect < srcAspect)
  {
    const int32_t scaledHeight = static_cast<int32_t>(dstExtent.width / srcAspect);
    const int32_t barHeight = (dstExtent.height - scaledHeight) / 2;
    dstY0 = barHeight;
    dstY1 = barHeight + scaledHeight;
  }

  VkOffset3D dstOffset0 = {dstX0, dstY0, 0};
  VkOffset3D dstOffset1 = {dstX1, dstY1, 1};

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::Transfer,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::TransferSrc,
      .isSwapchain = false,
  });
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::Transfer,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::TransferDst,
      .isSwapchain = true,
  });

  VkImageBlit blitRegion{
      .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .srcOffsets = {srcOffset0, srcOffset1},
      .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .dstOffsets = {dstOffset0, dstOffset1},
  };
  vkCmdBlitImage(vkCmd,
                 srcImage,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 dstImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1,
                 &blitRegion,
                 VK_FILTER_LINEAR);

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(dstImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::Transfer,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::TransferDst,
      .newState = rhi::ResourceState::General,
      .isSwapchain = true,
  });
  context.cmd->setResourceState(
      rhi::ResourceHandle{rhi::ResourceKind::Texture, kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
      rhi::ResourceState::General);
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(srcImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::Transfer,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::TransferSrc,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  beginPresentPass(*context.cmd);
  context.cmd->endEvent();
}

void GPUDrivenRenderer::executeImguiPass(const PassContext& context)
{
  if(context.params == nullptr)
  {
    return;
  }
  context.cmd->beginEvent("GPUDrivenImgui");
  executeImGuiPass(*context.cmd, *context.params);
  endPresentPass(*context.cmd);
  context.cmd->endEvent();
}

GltfUploadResult GPUDrivenRenderer::uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd)
{
  GltfUploadResult result = m_renderer.uploadGltfModel(model, cmd);
  rebuildGPUDrivenScene(model, result, cmd);
  return result;
}

void GPUDrivenRenderer::uploadGltfModelBatch(const GltfModel&          model,
                                             std::span<const uint32_t> textureIndices,
                                             std::span<const uint32_t> materialIndices,
                                             std::span<const uint32_t> meshIndices,
                                             GltfUploadResult&         ioResult,
                                             VkCommandBuffer           cmd)
{
  m_renderer.uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
  rebuildGPUDrivenScene(model, ioResult, cmd);
}

void GPUDrivenRenderer::destroyGltfResources(const GltfUploadResult& result)
{
  clearGPUDrivenScene();
  if(m_activeUploadResult == &result)
  {
    m_activeUploadResult = nullptr;
  }
  m_renderer.destroyGltfResources(result);
}

void GPUDrivenRenderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  m_renderer.updateMeshTransform(handle, transform);

  uint32_t drawIndex = 0;
  const bool hasDrawIndex = tryGetMeshDrawIndex(handle, drawIndex);
  const auto it = m_objectIdByMeshHandle.find(packMeshHandleKey(handle));
  if(it == m_objectIdByMeshHandle.end())
  {
    return;
  }

  const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(handle);
  if(meshRecord == nullptr)
  {
    return;
  }

  m_sceneRegistry.updateTransform(it->second,
                                  transform,
                                  glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius));
  m_sceneUploadPending = true;
  if(hasDrawIndex)
  {
    markPersistentDrawDirty(drawIndex);
  }
  else
  {
    m_persistentDrawDataDirty = true;
  }
  m_runtimeStats.pendingSceneUpdates = 1;
  refreshSceneView();
}

bool GPUDrivenRenderer::tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const
{
  const auto it = m_drawIndexByMeshHandle.find(packMeshHandleKey(handle));
  if(it == m_drawIndexByMeshHandle.end())
  {
    return false;
  }
  outDrawIndex = it->second;
  return true;
}

bool GPUDrivenRenderer::tryGetMeshHandleForDrawIndex(uint32_t drawIndex, MeshHandle& outHandle) const
{
  if(drawIndex >= m_meshHandleByDrawIndex.size())
  {
    return false;
  }
  outHandle = m_meshHandleByDrawIndex[drawIndex];
  return !outHandle.isNull();
}

uint64_t GPUDrivenRenderer::packMeshHandleKey(MeshHandle handle)
{
  return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
}

void GPUDrivenRenderer::rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult, VkCommandBuffer cmd)
{
  m_activeUploadResult = &uploadResult;
  const bool firstSceneBuild = m_objectIdByMeshHandle.empty();
  if(firstSceneBuild)
  {
    clearGPUDrivenScene();
    m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
  }

  bool appendedObjects = false;
  bool appendedMeshlets = false;

  for(size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < model.meshes.size(); ++meshIndex)
  {
    const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
    const uint64_t meshKey = packMeshHandleKey(meshHandle);
    if(m_objectIdByMeshHandle.find(meshKey) != m_objectIdByMeshHandle.end())
    {
      continue;
    }

    const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
    if(meshRecord == nullptr)
    {
      continue;
    }

    GPUSceneRegistrationDesc desc{};
    desc.meshHandle = meshHandle;
    desc.meshIndex = static_cast<uint32_t>(meshIndex);
    desc.materialIndex = meshRecord->materialIndex >= 0 ? static_cast<uint32_t>(meshRecord->materialIndex) : UINT32_MAX;
    desc.transform = meshRecord->transform;
    desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
    desc.flags = buildGPUDrivenFlags(*meshRecord);
    desc.indexCount = meshRecord->indexCount;
    desc.firstIndex = meshRecord->firstIndex;
    desc.vertexOffset = meshRecord->vertexOffset;

    const uint32_t drawIndex = m_sceneRegistry.getObjectCount();
    const uint32_t objectId = m_sceneRegistry.registerObject(desc);
    m_objectIdByMeshHandle[meshKey] = objectId;
    m_drawIndexByMeshHandle[meshKey] = drawIndex;
    if(drawIndex >= m_meshHandleByDrawIndex.size())
    {
      m_meshHandleByDrawIndex.resize(drawIndex + 1u, kNullMeshHandle);
    }
    m_meshHandleByDrawIndex[drawIndex] = meshHandle;
    if(meshRecord->alphaMode == shaderio::LAlphaBlend)
    {
      m_transparentDrawIndices.push_back(drawIndex);
    }
    else if(meshRecord->alphaMode == shaderio::LAlphaMask)
    {
      m_alphaTestDrawIndices.push_back(drawIndex);
    }
    else
    {
      m_opaqueDrawIndices.push_back(drawIndex);
    }
    appendedObjects = true;

    if(m_enableExperimentalMeshletPath)
    {
      MeshletConversionResult meshlets = MeshletConverter::convert(model.meshes[meshIndex]);
      for(shaderio::Meshlet& meshlet : meshlets.meshlets)
      {
        meshlet.materialIndex = desc.materialIndex;
      }
      if(!meshlets.meshlets.empty())
      {
        m_meshletDataCpu.insert(m_meshletDataCpu.end(), meshlets.meshlets.begin(), meshlets.meshlets.end());
        appendedMeshlets = true;
      }
      if(!meshlets.packedIndices.empty())
      {
        m_meshletIndicesCpu.insert(m_meshletIndicesCpu.end(), meshlets.packedIndices.begin(), meshlets.packedIndices.end());
      }
      m_runtimeStats.meshletTriangleCount += meshlets.triangleCount;
    }
  }

  if(appendedObjects || firstSceneBuild)
  {
    ++m_sceneTopologyVersion;
    invalidateSortedBootstrapStates();
    m_sceneRegistry.syncToGpu(cmd);
  }
  if(m_enableExperimentalMeshletPath && (appendedMeshlets || firstSceneBuild))
  {
    m_meshletBuffer.uploadMeshlets(cmd, m_meshletDataCpu, m_meshletIndicesCpu);
  }
  m_sceneUploadPending = false;
  m_persistentDrawDataDirty = true;
  m_dirtyPersistentDrawIndices.clear();
  m_runtimeStats.sceneUploadCount += 1;
  m_runtimeStats.pendingSceneUpdates = 0;
  m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
  m_runtimeStats.meshletCount = m_enableExperimentalMeshletPath ? m_meshletBuffer.getMeshletCount() : 0u;
  refreshSceneView();
}

void GPUDrivenRenderer::clearGPUDrivenScene()
{
  ++m_sceneTopologyVersion;
  invalidateSortedBootstrapStates();
  m_sceneRegistry.clear();
  if(m_enableExperimentalMeshletPath)
  {
    m_meshletBuffer.clear();
  }
  m_meshletDataCpu.clear();
  m_meshletIndicesCpu.clear();
  m_persistentDrawData.clear();
  m_visibilitySortInputObjects.clear();
  m_visibilitySortInputKeys.clear();
  m_objectIdByMeshHandle.clear();
  m_drawIndexByMeshHandle.clear();
  m_meshHandleByDrawIndex.clear();
  m_opaqueDrawIndices.clear();
  m_alphaTestDrawIndices.clear();
  m_transparentDrawIndices.clear();
  m_sceneView = {};
  m_runtimeStats = {};
  m_activeUploadResult = nullptr;
  m_sceneUploadPending = false;
  m_persistentDrawDataDirty = false;
  m_dirtyPersistentDrawIndices.clear();
}

void GPUDrivenRenderer::flushPendingSceneUploads()
{
  if(!m_sceneUploadPending || !m_sceneRegistry.isDirty())
  {
    return;
  }

  m_renderer.executeUploadCommand([this](VkCommandBuffer cmd) {
    m_sceneRegistry.syncToGpu(cmd);
  });
  m_sceneUploadPending = false;
  m_persistentDrawDataDirty = true;
  m_dirtyPersistentDrawIndices.clear();
  m_runtimeStats.sceneUploadCount += 1;
  m_runtimeStats.pendingSceneUpdates = 0;
  m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
}

void GPUDrivenRenderer::invalidateSortedBootstrapStates()
{
  for(SortedBootstrapFrameState& frameState : m_sortedBootstrapFrames)
  {
    frameState = {};
  }
}

void GPUDrivenRenderer::invalidateSortedBootstrapState(uint32_t frameIndex)
{
  if(frameIndex < m_sortedBootstrapFrames.size())
  {
    m_sortedBootstrapFrames[frameIndex] = {};
  }
}

void GPUDrivenRenderer::recordSortedBootstrapState(uint32_t frameIndex, uint32_t opaqueCapacity, uint32_t alphaCapacity)
{
  if(frameIndex >= m_sortedBootstrapFrames.size())
  {
    m_sortedBootstrapFrames.resize(std::max(frameIndex + 1u, getSwapchainImageCount()), SortedBootstrapFrameState{});
  }

  m_sortedBootstrapFrames[frameIndex] = SortedBootstrapFrameState{
      .opaqueCapacity = opaqueCapacity,
      .alphaCapacity = alphaCapacity,
      .sceneTopologyVersion = m_sceneTopologyVersion,
      .valid = true,
  };
}

bool GPUDrivenRenderer::getPreviousSortedBootstrapState(uint32_t frameIndex,
                                                        uint32_t& outOpaqueCapacity,
                                                        uint32_t& outAlphaCapacity) const
{
  outOpaqueCapacity = 0u;
  outAlphaCapacity = 0u;
  if(m_sortedBootstrapFrames.empty())
  {
    return false;
  }

  const uint32_t previousFrameIndex = getPreviousFrameIndex(frameIndex);
  if(previousFrameIndex >= m_sortedBootstrapFrames.size())
  {
    return false;
  }

  const SortedBootstrapFrameState& frameState = m_sortedBootstrapFrames[previousFrameIndex];
  if(!frameState.valid || frameState.sceneTopologyVersion != m_sceneTopologyVersion)
  {
    return false;
  }

  outOpaqueCapacity = frameState.opaqueCapacity;
  outAlphaCapacity = frameState.alphaCapacity;
  return outOpaqueCapacity + outAlphaCapacity > 0u;
}

void GPUDrivenRenderer::markPersistentDrawDirty(uint32_t drawIndex)
{
  m_persistentDrawDataDirty = true;
  m_dirtyPersistentDrawIndices.push_back(drawIndex);
}

std::vector<GPUDrivenRenderer::DirtyRange> GPUDrivenRenderer::buildPersistentDrawDirtyRanges() const
{
  if(m_dirtyPersistentDrawIndices.empty())
  {
    return {};
  }

  std::vector<uint32_t> sortedIndices = m_dirtyPersistentDrawIndices;
  std::sort(sortedIndices.begin(), sortedIndices.end());
  sortedIndices.erase(std::unique(sortedIndices.begin(), sortedIndices.end()), sortedIndices.end());

  std::vector<DirtyRange> ranges;
  ranges.reserve(sortedIndices.size());

  DirtyRange currentRange{sortedIndices.front(), 1u};
  for(size_t i = 1; i < sortedIndices.size(); ++i)
  {
    const uint32_t drawIndex = sortedIndices[i];
    if(drawIndex == currentRange.first + currentRange.count)
    {
      currentRange.count += 1u;
      continue;
    }

    ranges.push_back(currentRange);
    currentRange = DirtyRange{drawIndex, 1u};
  }

  ranges.push_back(currentRange);
  return ranges;
}

void GPUDrivenRenderer::uploadPersistentDrawData()
{
  if(!m_sceneView.usePersistentCullingObjects)
  {
    return;
  }

  if(!m_persistentDrawDataDirty && !m_persistentDrawData.empty())
  {
    return;
  }

  const bool needsFullUpload = m_persistentDrawData.size() != m_sceneView.objectCount || m_persistentDrawData.empty()
                               || m_dirtyPersistentDrawIndices.empty();
  if(needsFullUpload)
  {
    m_persistentDrawData.assign(m_sceneView.objectCount, shaderio::DrawUniforms{});
  }

  const auto updateDrawPayload = [this](uint32_t drawIndex) {
    if(drawIndex >= m_persistentDrawData.size() || drawIndex >= m_meshHandleByDrawIndex.size())
    {
      return;
    }

    const MeshHandle meshHandle = m_meshHandleByDrawIndex[drawIndex];
    if(meshHandle.isNull())
    {
      m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
      return;
    }

    const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
    if(mesh == nullptr)
    {
      m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
      return;
    }

    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh->transform;
    drawData.baseColorFactor = mesh->baseColorFactor;
    drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
    drawData.normalTextureIndex = mesh->normalTextureIndex;
    drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
    drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
    drawData.metallicFactor = mesh->metallicFactor;
    drawData.roughnessFactor = mesh->roughnessFactor;
    drawData.normalScale = mesh->normalScale;
    drawData.alphaMode = mesh->alphaMode;
    drawData.alphaCutoff = mesh->alphaCutoff;
    m_persistentDrawData[drawIndex] = drawData;
  };

  std::vector<DirtyRange> dirtyRanges;
  if(needsFullUpload)
  {
    for(uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_persistentDrawData.size()); ++drawIndex)
    {
      updateDrawPayload(drawIndex);
    }
  }
  else
  {
    dirtyRanges = buildPersistentDrawDirtyRanges();
    for(const DirtyRange& range : dirtyRanges)
    {
      for(uint32_t drawIndex = range.first; drawIndex < range.first + range.count; ++drawIndex)
      {
        updateDrawPayload(drawIndex);
      }
    }
  }

  const uint32_t frameCount = getSwapchainImageCount();
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    if(needsFullUpload)
    {
      m_renderer.uploadMDIDrawData(frameIndex, m_persistentDrawData);
      m_renderer.uploadGBufferMDIDrawData(frameIndex, m_persistentDrawData);
      m_renderer.uploadDepthMDIDrawData(frameIndex, m_persistentDrawData);
      continue;
    }

    for(const DirtyRange& range : dirtyRanges)
    {
      const std::span<const shaderio::DrawUniforms> drawRange{m_persistentDrawData.data() + range.first, range.count};
      m_renderer.uploadMDIDrawDataRange(frameIndex, range.first, drawRange);
      m_renderer.uploadGBufferMDIDrawDataRange(frameIndex, range.first, drawRange);
      m_renderer.uploadDepthMDIDrawDataRange(frameIndex, range.first, drawRange);
    }
  }
  m_persistentDrawDataDirty = false;
  m_dirtyPersistentDrawIndices.clear();
}

void GPUDrivenRenderer::refreshSceneView()
{
  m_sceneView.gpuSceneObjectBufferAddress = m_sceneRegistry.getBufferAddress();
  m_sceneView.gpuCullObjectBufferAddress = m_sceneRegistry.getCullBufferAddress();
  m_sceneView.gpuCullObjectBuffer = m_sceneRegistry.getCullBufferHandle();
  m_sceneView.objectCount = m_sceneRegistry.getObjectCount();
  m_sceneView.overlayObjects = m_sceneRegistry.getOverlayObjects().empty() ? nullptr : m_sceneRegistry.getOverlayObjects().data();
  m_sceneView.overlayObjectCount = static_cast<uint32_t>(m_sceneRegistry.getOverlayObjects().size());
  if(m_sceneView.overlayObjectCount > m_sceneView.objectCount)
  {
    m_sceneView.overlayObjectCount = m_sceneView.objectCount;
  }
  m_sceneView.usePersistentCullingObjects = m_sceneView.gpuCullObjectBuffer != VK_NULL_HANDLE && m_sceneView.objectCount > 0;
  m_sceneView.authority = m_sceneView.usePersistentCullingObjects
                              ? GPUDrivenSceneAuthority::persistentCullObjects
                              : GPUDrivenSceneAuthority::none;
  m_sceneView.indirectSource = m_sceneView.usePersistentCullingObjects
                                   ? GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect
                                   : GPUDrivenIndirectSourceKind::none;
  m_sceneView.indirectCommandStride =
      m_sceneView.usePersistentCullingObjects ? m_renderer.getGPUCullingIndirectCommandStride() : 0;
  m_sceneView.meshHandles = m_activeUploadResult != nullptr && !m_activeUploadResult->meshes.empty()
                                ? m_activeUploadResult->meshes.data()
                                : nullptr;
  m_sceneView.meshHandleCount =
      m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->meshes.size()) : 0;
  m_sceneView.shadowCasterMeshIndices =
      m_activeUploadResult != nullptr && !m_activeUploadResult->shadowCasterIndices.empty()
          ? m_activeUploadResult->shadowCasterIndices.data()
          : nullptr;
  m_sceneView.shadowCasterCount =
      m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->shadowCasterIndices.size()) : 0;
  m_sceneView.shadowPackedVertexBuffer =
      m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedVertexBuffer.buffer : VK_NULL_HANDLE;
  m_sceneView.shadowPackedIndexBuffer =
      m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedIndexBuffer.buffer : VK_NULL_HANDLE;
  m_sceneView.shadowPackedMeshes =
      m_activeUploadResult != nullptr && !m_activeUploadResult->shadowPackedMeshes.empty()
          ? m_activeUploadResult->shadowPackedMeshes.data()
          : nullptr;
  m_sceneView.shadowPackedMeshCount =
      m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->shadowPackedMeshes.size()) : 0;
  m_sceneView.sceneBoundsMin = glm::vec3(0.0f);
  m_sceneView.sceneBoundsMax = glm::vec3(0.0f);
  m_sceneView.sceneBoundsValid = false;
  m_sceneView.sceneDepthFormat = getSceneDepthFormat();
  m_sceneView.sceneDepthImage = getSceneDepthImage();
  m_sceneView.sceneDepthView = getSceneDepthImageView();
  m_sceneView.sceneDepthExtent = getSceneExtent();
  for(uint32_t i = 0; i < 3; ++i)
  {
    m_sceneView.gbufferImages[i] = getSceneGBufferImage(i);
    m_sceneView.gbufferViews[i] = getSceneGBufferImageView(i);
  }
  m_sceneView.outputImage = getOutputTextureImage();
  m_sceneView.outputView = getOutputTextureView();
  m_sceneView.depthPyramidImage = m_hiZDepthPyramid.getImage();
  m_sceneView.depthPyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
  m_sceneView.depthPyramidMipCount = m_hiZDepthPyramid.getMipCount();
  m_sceneView.depthPyramidSourceDepth = m_hiZDepthPyramid.getSourceDepth();
  m_sceneView.depthPyramidGeneration = m_hiZDepthPyramid.getGenerationCount();
  m_sceneView.depthPyramidValid = m_hiZDepthPyramid.isValid();
  if(m_activeUploadResult != nullptr && !m_activeUploadResult->meshes.empty())
  {
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    bool boundsValid = false;
    for(const MeshHandle meshHandle : m_activeUploadResult->meshes)
    {
      const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
      if(mesh == nullptr)
      {
        continue;
      }
      boundsMin = glm::min(boundsMin, mesh->worldBoundsMin);
      boundsMax = glm::max(boundsMax, mesh->worldBoundsMax);
      boundsValid = true;
    }
    if(boundsValid)
    {
      m_sceneView.sceneBoundsMin = boundsMin;
      m_sceneView.sceneBoundsMax = boundsMax;
      m_sceneView.sceneBoundsValid = true;
    }
  }
  m_runtimeStats.objectCount = m_sceneView.objectCount;
  m_runtimeStats.authority = m_sceneView.authority;
  m_runtimeStats.indirectSource = m_sceneView.indirectSource;
  m_runtimeStats.indirectCommandStride = m_sceneView.indirectCommandStride;
  m_runtimeStats.usesPersistentCullObjects = m_sceneView.usePersistentCullingObjects;
  m_runtimeStats.meshletCount = m_enableExperimentalMeshletPath ? m_meshletBuffer.getMeshletCount() : 0u;
  m_runtimeStats.ownsFullRenderChain = true;
  m_runtimeStats.ownsHiZVisibilityChain = false;
  m_runtimeStats.hiZGeneration = 0;
}

uint32_t GPUDrivenRenderer::getSafePersistentObjectCount() const
{
  uint32_t safeCount = m_sceneView.objectCount;
  if(m_sceneView.meshHandleCount > 0u)
  {
    safeCount = std::min(safeCount, m_sceneView.meshHandleCount);
  }
  if(m_sceneView.overlayObjectCount > 0u)
  {
    safeCount = std::min(safeCount, m_sceneView.overlayObjectCount);
  }
  safeCount = std::min(safeCount, kMaxReasonableGPUDrivenObjectCount);
  return safeCount;
}

void GPUDrivenRenderer::initVisibilitySortResources()
{
  shutdownVisibilitySortResources();

  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
  const std::array<VkDescriptorPoolSize, 1> poolSizes{{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 2u},
  }};
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets       = frameCount,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes    = poolSizes.data(),
  };
  VK_CHECK(vkCreateDescriptorPool(nativeDevice, &poolInfo, nullptr, &m_visibilitySortDescriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &setLayoutInfo, nullptr, &m_visibilitySortSetLayout));

  const VkPushConstantRange pushConstantRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(shaderio::BitonicSortPushConstants),
  };
  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_visibilitySortSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &pipelineLayoutInfo, nullptr, &m_visibilitySortPipelineLayout));

#ifdef USE_SLANG
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice, {shader_bitonic_sort_slang, std::size(shader_bitonic_sort_slang)});
  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "bitonicSortMain",
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = m_visibilitySortPipelineLayout,
  };
  VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_visibilitySortPipeline));
  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif

  std::vector<VkDescriptorSetLayout> layouts(frameCount, m_visibilitySortSetLayout);
  std::vector<VkDescriptorSet> descriptorSets(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_visibilitySortDescriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts        = layouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, descriptorSets.data()));

  m_visibilitySortFrames.resize(frameCount);
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    m_visibilitySortFrames[frameIndex].descriptorSet = descriptorSets[frameIndex];
  }
}

void GPUDrivenRenderer::shutdownVisibilitySortResources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  const VmaAllocator allocator = getAllocatorHandle();
  for(VisibilitySortFrameResources& frameResources : m_visibilitySortFrames)
  {
    destroyBuffer(allocator, frameResources.uploadKeyBuffer);
    destroyBuffer(allocator, frameResources.uploadValueBuffer);
    destroyBuffer(allocator, frameResources.keyBuffer);
    destroyBuffer(allocator, frameResources.valueBuffer);
    frameResources = {};
  }
  m_visibilitySortFrames.clear();

  if(nativeDevice != VK_NULL_HANDLE)
  {
    if(m_visibilitySortPipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline(nativeDevice, m_visibilitySortPipeline, nullptr);
      m_visibilitySortPipeline = VK_NULL_HANDLE;
    }
    if(m_visibilitySortPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_visibilitySortPipelineLayout, nullptr);
      m_visibilitySortPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_visibilitySortSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_visibilitySortSetLayout, nullptr);
      m_visibilitySortSetLayout = VK_NULL_HANDLE;
    }
    if(m_visibilitySortDescriptorPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(nativeDevice, m_visibilitySortDescriptorPool, nullptr);
      m_visibilitySortDescriptorPool = VK_NULL_HANDLE;
    }
  }
}

void GPUDrivenRenderer::initTransparentVisibilityPatchResources()
{
  shutdownTransparentVisibilityPatchResources();

  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
  const std::array<VkDescriptorPoolSize, 1> poolSizes{{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 8u},
  }};
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = frameCount * 2u,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
  };
  VK_CHECK(vkCreateDescriptorPool(nativeDevice, &poolInfo, nullptr, &m_transparentVisibilityPatchDescriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice,
                                       &setLayoutInfo,
                                       nullptr,
                                       &m_transparentVisibilityPatchSetLayout));

  const VkPushConstantRange pushConstantRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(shaderio::TransparentVisibilityPatchPushConstants),
  };
  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_transparentVisibilityPatchSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushConstantRange,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice,
                                  &pipelineLayoutInfo,
                                  nullptr,
                                  &m_transparentVisibilityPatchPipelineLayout));

#ifdef USE_SLANG
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice,
                                {shader_transparent_visibility_patch_slang,
                                 std::size(shader_transparent_visibility_patch_slang)});
  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName = "transparentVisibilityPatchMain",
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = shaderStage,
      .layout = m_transparentVisibilityPatchPipelineLayout,
  };
  VK_CHECK(vkCreateComputePipelines(nativeDevice,
                                    VK_NULL_HANDLE,
                                    1,
                                    &pipelineInfo,
                                    nullptr,
                                    &m_transparentVisibilityPatchPipeline));
  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif

  std::vector<VkDescriptorSetLayout> layouts(frameCount * 2u, m_transparentVisibilityPatchSetLayout);
  std::vector<VkDescriptorSet> descriptorSets(frameCount * 2u, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_transparentVisibilityPatchDescriptorPool,
      .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, descriptorSets.data()));

  m_transparentVisibilityPatchFrames.resize(frameCount);
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    m_transparentVisibilityPatchFrames[frameIndex].descriptorSets[0] = descriptorSets[frameIndex * 2u + 0u];
    m_transparentVisibilityPatchFrames[frameIndex].descriptorSets[1] = descriptorSets[frameIndex * 2u + 1u];
  }
}

void GPUDrivenRenderer::shutdownTransparentVisibilityPatchResources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  m_transparentVisibilityPatchFrames.clear();

  if(nativeDevice != VK_NULL_HANDLE)
  {
    if(m_transparentVisibilityPatchPipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline(nativeDevice, m_transparentVisibilityPatchPipeline, nullptr);
      m_transparentVisibilityPatchPipeline = VK_NULL_HANDLE;
    }
    if(m_transparentVisibilityPatchPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_transparentVisibilityPatchPipelineLayout, nullptr);
      m_transparentVisibilityPatchPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_transparentVisibilityPatchSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_transparentVisibilityPatchSetLayout, nullptr);
      m_transparentVisibilityPatchSetLayout = VK_NULL_HANDLE;
    }
    if(m_transparentVisibilityPatchDescriptorPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(nativeDevice, m_transparentVisibilityPatchDescriptorPool, nullptr);
      m_transparentVisibilityPatchDescriptorPool = VK_NULL_HANDLE;
    }
  }
}

void GPUDrivenRenderer::updateTransparentVisibilityPatchDescriptorSet(uint32_t frameIndex,
                                                                      uint64_t sortKeyBufferHandle,
                                                                      uint64_t sortValueBufferHandle,
                                                                      uint64_t sourceIndirectBufferHandle,
                                                                      uint64_t targetIndirectBufferHandle)
{
  if(frameIndex >= m_transparentVisibilityPatchFrames.size())
  {
    return;
  }

  const VkDevice nativeDevice = getNativeDeviceHandle();
  TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
  const uint32_t descriptorSetIndex =
      targetIndirectBufferHandle == m_renderer.getForwardMDIIndirectBuffer(frameIndex) ? 1u : 0u;
  if(nativeDevice == VK_NULL_HANDLE || frameResources.descriptorSets[descriptorSetIndex] == VK_NULL_HANDLE || sortKeyBufferHandle == 0
     || sortValueBufferHandle == 0 || sourceIndirectBufferHandle == 0 || targetIndirectBufferHandle == 0)
  {
    return;
  }

  if(frameResources.boundSortKeyHandles[descriptorSetIndex] == sortKeyBufferHandle
     && frameResources.boundSortValueHandles[descriptorSetIndex] == sortValueBufferHandle
     && frameResources.boundSourceIndirectHandles[descriptorSetIndex] == sourceIndirectBufferHandle
     && frameResources.boundTargetIndirectHandles[descriptorSetIndex] == targetIndirectBufferHandle)
  {
    return;
  }

  const std::array<VkDescriptorBufferInfo, 4> bufferInfos{{
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sortKeyBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sortValueBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sourceIndirectBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(targetIndirectBufferHandle), 0, VK_WHOLE_SIZE},
  }};
  const std::array<VkWriteDescriptorSet, 4> writes{{
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[0],
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[1],
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[2],
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[3],
      },
  }};
  vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  frameResources.boundSortKeyHandles[descriptorSetIndex] = sortKeyBufferHandle;
  frameResources.boundSortValueHandles[descriptorSetIndex] = sortValueBufferHandle;
  frameResources.boundSourceIndirectHandles[descriptorSetIndex] = sourceIndirectBufferHandle;
  frameResources.boundTargetIndirectHandles[descriptorSetIndex] = targetIndirectBufferHandle;
}

uint32_t GPUDrivenRenderer::getPreviousFrameIndex(uint32_t frameIndex) const
{
  const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
  return (frameIndex + frameCount - 1u) % frameCount;
}

bool GPUDrivenRenderer::prepareAndDispatchVisibilityPatch(rhi::CommandList& cmd,
                                                          uint32_t          frameIndex,
                                                          uint64_t          targetIndirectBufferHandle,
                                                          uint32_t          categoryValue,
                                                          uint32_t          outputOffset)
{
  if(frameIndex >= m_transparentVisibilityPatchFrames.size()
      || frameIndex >= m_visibilitySortFrames.size()
      || m_transparentVisibilityPatchPipeline == VK_NULL_HANDLE
      || m_transparentVisibilityPatchPipelineLayout == VK_NULL_HANDLE
      || targetIndirectBufferHandle == 0)
  {
    return false;
  }

  const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
  if(sortResources.activeElementCount == 0 || sortResources.valueBuffer.buffer == VK_NULL_HANDLE)
  {
    return false;
  }

  const uint64_t sourceIndirectBufferHandle = m_renderer.getGPUCullingIndirectBufferOpaque(frameIndex);
  if(sourceIndirectBufferHandle == 0)
  {
    return false;
  }

  updateTransparentVisibilityPatchDescriptorSet(frameIndex,
                                                reinterpret_cast<uint64_t>(sortResources.keyBuffer.buffer),
                                                reinterpret_cast<uint64_t>(sortResources.valueBuffer.buffer),
                                                sourceIndirectBufferHandle,
                                                targetIndirectBufferHandle);

  const TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
  const uint32_t descriptorSetIndex =
      targetIndirectBufferHandle == m_renderer.getForwardMDIIndirectBuffer(frameIndex) ? 1u : 0u;
  if(frameResources.descriptorSets[descriptorSetIndex] == VK_NULL_HANDLE)
  {
    return false;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  const VkMemoryBarrier2 computeToComputeBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
  };
  const VkDependencyInfo computeToComputeDependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &computeToComputeBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &computeToComputeDependency);

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transparentVisibilityPatchPipeline);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_transparentVisibilityPatchPipelineLayout,
                          0,
                          1,
                          &frameResources.descriptorSets[descriptorSetIndex],
                          0,
                          nullptr);

  const shaderio::TransparentVisibilityPatchPushConstants pushConstants{
      .elementCount = sortResources.activeElementCount,
      .categoryMask = kVisibilitySortCategoryMask,
      .categoryValue = categoryValue,
      .outputOffset = outputOffset,
  };
  vkCmdPushConstants(vkCmd,
                     m_transparentVisibilityPatchPipelineLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT,
                     0,
                     sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(vkCmd, (pushConstants.elementCount + 63u) / 64u, 1u, 1u);

  const VkMemoryBarrier2 computeToIndirectBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
      .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
  };
  const VkDependencyInfo computeToIndirectDependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &computeToIndirectBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &computeToIndirectDependency);
  return true;
}

void GPUDrivenRenderer::ensureVisibilitySortCapacity(uint32_t frameIndex, uint32_t requiredCount)
{
  if(frameIndex >= m_visibilitySortFrames.size())
  {
    return;
  }

  VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
  if(frameResources.capacity >= requiredCount)
  {
    return;
  }

  const VkDevice nativeDevice = getNativeDeviceHandle();
  const VmaAllocator allocator = getAllocatorHandle();
  if(frameResources.capacity > 0)
  {
    waitForIdle();
  }
  destroyBuffer(allocator, frameResources.uploadKeyBuffer);
  destroyBuffer(allocator, frameResources.uploadValueBuffer);
  destroyBuffer(allocator, frameResources.keyBuffer);
  destroyBuffer(allocator, frameResources.valueBuffer);

  const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(requiredCount) * sizeof(uint32_t);
  frameResources.uploadKeyBuffer = upload::createMappedUploadStagingBuffer(nativeDevice, allocator, bufferSize);
  frameResources.uploadValueBuffer = upload::createMappedUploadStagingBuffer(nativeDevice, allocator, bufferSize);
  frameResources.keyBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
  frameResources.valueBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
  frameResources.capacity = requiredCount;
  updateVisibilitySortDescriptorSet(frameIndex);
}

void GPUDrivenRenderer::updateVisibilitySortDescriptorSet(uint32_t frameIndex)
{
  if(frameIndex >= m_visibilitySortFrames.size())
  {
    return;
  }

  const VkDevice nativeDevice = getNativeDeviceHandle();
  VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
  if(nativeDevice == VK_NULL_HANDLE || frameResources.descriptorSet == VK_NULL_HANDLE
     || frameResources.keyBuffer.buffer == VK_NULL_HANDLE || frameResources.valueBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const std::array<VkDescriptorBufferInfo, 2> bufferInfos{{
      VkDescriptorBufferInfo{frameResources.keyBuffer.buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameResources.valueBuffer.buffer, 0, VK_WHOLE_SIZE},
  }};
  const std::array<VkWriteDescriptorSet, 2> writes{{
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = frameResources.descriptorSet,
          .dstBinding      = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[0],
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = frameResources.descriptorSet,
          .dstBinding      = 1,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[1],
      },
  }};
  vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void GPUDrivenRenderer::prepareVisibilitySortInputs(uint32_t frameIndex)
{
  if(frameIndex >= m_visibilitySortFrames.size())
  {
    return;
  }

  VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
  if(m_visibilitySortInputObjects.empty() || m_visibilitySortInputKeys.size() != m_visibilitySortInputObjects.size())
  {
    frameResources.activeElementCount = 0;
    frameResources.paddedElementCount = 0;
    return;
  }

  const uint32_t paddedCount = nextPowerOfTwo(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
  ensureVisibilitySortCapacity(frameIndex, paddedCount);
  if(frameResources.uploadKeyBuffer.mapped == nullptr || frameResources.uploadValueBuffer.mapped == nullptr)
  {
    return;
  }

  std::vector<uint32_t> paddedKeys(paddedCount, 0xffffffffu);
  std::vector<uint32_t> paddedValues(paddedCount, 0xffffffffu);
  std::copy(m_visibilitySortInputKeys.begin(), m_visibilitySortInputKeys.end(), paddedKeys.begin());
  std::copy(m_visibilitySortInputObjects.begin(), m_visibilitySortInputObjects.end(), paddedValues.begin());

  std::memcpy(frameResources.uploadKeyBuffer.mapped, paddedKeys.data(), paddedCount * sizeof(uint32_t));
  std::memcpy(frameResources.uploadValueBuffer.mapped, paddedValues.data(), paddedCount * sizeof(uint32_t));
  VK_CHECK(vmaFlushAllocation(getAllocatorHandle(), frameResources.uploadKeyBuffer.allocation, 0, VK_WHOLE_SIZE));
  VK_CHECK(vmaFlushAllocation(getAllocatorHandle(), frameResources.uploadValueBuffer.allocation, 0, VK_WHOLE_SIZE));

  frameResources.activeElementCount = static_cast<uint32_t>(m_visibilitySortInputObjects.size());
  frameResources.paddedElementCount = paddedCount;
}

void GPUDrivenRenderer::executeVisibilitySortPass(const PassContext& context) const
{
  if(!kEnableShippingVisibilitySort)
  {
    return;
  }

  if(context.cmd == nullptr || context.frameIndex >= m_visibilitySortFrames.size()
     || m_visibilitySortPipeline == VK_NULL_HANDLE || m_visibilitySortPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[context.frameIndex];
  if(frameResources.descriptorSet == VK_NULL_HANDLE || frameResources.paddedElementCount <= 1u
     || frameResources.uploadKeyBuffer.buffer == VK_NULL_HANDLE || frameResources.uploadValueBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
  const VkBufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = static_cast<VkDeviceSize>(frameResources.paddedElementCount) * sizeof(uint32_t)};
  vkCmdCopyBuffer(vkCmd, frameResources.uploadKeyBuffer.buffer, frameResources.keyBuffer.buffer, 1, &copyRegion);
  vkCmdCopyBuffer(vkCmd, frameResources.uploadValueBuffer.buffer, frameResources.valueBuffer.buffer, 1, &copyRegion);

  const std::array<VkBufferMemoryBarrier2, 2> transferToComputeBarriers{{
      VkBufferMemoryBarrier2{
          .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .buffer              = frameResources.keyBuffer.buffer,
          .offset              = 0,
          .size                = VK_WHOLE_SIZE,
      },
      VkBufferMemoryBarrier2{
          .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .buffer              = frameResources.valueBuffer.buffer,
          .offset              = 0,
          .size                = VK_WHOLE_SIZE,
      },
  }};
  const VkDependencyInfo transferToComputeDependency{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(transferToComputeBarriers.size()),
      .pBufferMemoryBarriers    = transferToComputeBarriers.data(),
  };
  vkCmdPipelineBarrier2(vkCmd, &transferToComputeDependency);

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_visibilitySortPipeline);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_visibilitySortPipelineLayout,
                          0,
                          1,
                          &frameResources.descriptorSet,
                          0,
                          nullptr);

  const auto issueBarrier = [&]() {
    const VkMemoryBarrier2 memoryBarrier{
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
    };
    const VkDependencyInfo dependencyInfo{
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &memoryBarrier,
    };
    vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
  };

  for(uint32_t level = 2u; level <= frameResources.paddedElementCount; level <<= 1u)
  {
    for(uint32_t levelMask = level >> 1u; levelMask > 0u; levelMask >>= 1u)
    {
      const shaderio::BitonicSortPushConstants pushConstants{
          .elementCount = frameResources.paddedElementCount,
          .level = level,
          .levelMask = levelMask,
          .descending = 1u,
      };
      vkCmdPushConstants(vkCmd,
                         m_visibilitySortPipelineLayout,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         0,
                         sizeof(pushConstants),
                         &pushConstants);
      vkCmdDispatch(vkCmd, (frameResources.paddedElementCount + 63u) / 64u, 1u, 1u);
      issueBarrier();
    }
  }
}

}  // namespace demo
