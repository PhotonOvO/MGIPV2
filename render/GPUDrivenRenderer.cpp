#include "GPUDrivenRenderer.h"
#include "BatchUploadContext.h"
#include "UploadUtils.h"
#include "../common/TracyProfiling.h"
#include "../loader/Ktx2Loader.h"
#include "../rhi/vulkan/VulkanCommandList.h"
#include "../rhi/vulkan/VulkanAdoptedBindGroup.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <span>

namespace demo {

namespace {

constexpr bool kEnableExperimentalMeshletPath = true;
constexpr bool kEnableShippingVisibilitySort = true;
constexpr uint32_t kDebugCullSegmentCount = 24u;
constexpr uint32_t kLightCoarseCullingThreadCount = 64u;
constexpr uint32_t kVisibilitySortCategoryMask = 0xc0000000u;
constexpr uint32_t kVisibilitySortKeyMask = 0x3fffffffu;
constexpr uint32_t kVisibilitySortCategoryOpaque = 0x00000000u;
constexpr uint32_t kVisibilitySortCategoryAlpha = 0x40000000u;
constexpr uint32_t kVisibilitySortCategoryTransparent = 0x80000000u;
constexpr uint32_t kMobileMaxTransparentDraws = 2048u;
constexpr float kGPUDrivenHiZDepthEpsilon = 2.0e-3f;
constexpr float kGPUDrivenHiZConservativeRadiusScale = 1.1f;
constexpr float kGPUDrivenHiZConservativeRadiusBias = 0.1f;
constexpr float kGPUDrivenHiZNearRejectEpsilon = 1.0e-4f;
constexpr float kGPUDrivenHiZLargeObjectFootprintThreshold = 192.0f;
constexpr float kGPUDrivenHiZFastCameraFallbackDistance = 8.0f;
constexpr uint32_t kGPUDrivenLightPassTextureCount = kPackedGBufferTargetCount + 18u;
constexpr uint32_t kGPUDrivenLightPassDepthTextureIndex = kPackedGBufferTargetCount;
constexpr uint32_t kGPUDrivenLightPassSceneColorHdrIndex = kPackedGBufferTargetCount + 1u;
constexpr uint32_t kGPUDrivenLightPassBloomHalfIndex = kPackedGBufferTargetCount + 2u;
constexpr uint32_t kGPUDrivenLightPassBloomQuarterIndex = kPackedGBufferTargetCount + 3u;
constexpr uint32_t kGPUDrivenLightPassVelocityIndex = kPackedGBufferTargetCount + 4u;
constexpr uint32_t kGPUDrivenLightPassHistoryReadIndex = kPackedGBufferTargetCount + 5u;
constexpr uint32_t kGPUDrivenLightPassHistoryWriteIndex = kPackedGBufferTargetCount + 6u;
constexpr uint32_t kGPUDrivenLightPassIBLEnvironmentIndex = kPackedGBufferTargetCount + 7u;
constexpr uint32_t kGPUDrivenLightPassAOIndex = kPackedGBufferTargetCount + 8u;
constexpr uint32_t kGPUDrivenLightPassSSRIndex = kPackedGBufferTargetCount + 9u;
constexpr uint32_t kGPUDrivenLightPassBloomEighthIndex = kPackedGBufferTargetCount + 10u;
constexpr uint32_t kGPUDrivenLightPassBloomSixteenthIndex = kPackedGBufferTargetCount + 11u;
constexpr uint32_t kGPUDrivenLightPassBloomThirtySecondIndex = kPackedGBufferTargetCount + 12u;
constexpr uint32_t kGPUDrivenLightPassBloomUpsampleSixteenthIndex = kPackedGBufferTargetCount + 13u;
constexpr uint32_t kGPUDrivenLightPassBloomUpsampleEighthIndex = kPackedGBufferTargetCount + 14u;
constexpr uint32_t kGPUDrivenLightPassBloomUpsampleQuarterIndex = kPackedGBufferTargetCount + 15u;
constexpr uint32_t kGPUDrivenLightPassBloomOutputIndex = kPackedGBufferTargetCount + 16u;
constexpr uint32_t kGPUDrivenLightPassColorGradingLutIndex = kPackedGBufferTargetCount + 17u;
constexpr VkFormat kGPUDrivenAOFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat kGPUDrivenSSRFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGPUDrivenShadowAtlasFormat = VK_FORMAT_D32_SFLOAT;

uint64_t estimateImageBytes(VkExtent2D extent, uint32_t bytesPerPixel)
{
  return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * bytesPerPixel;
}
constexpr const char* kGPUDrivenDefaultIBLEnvironmentPath = "resources/environment/lilienstein_4k.ktx2";

uint32_t bytesPerPixelForFormat(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
      return 4u;
    case VK_FORMAT_R16G16_SFLOAT:
      return 4u;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return 8u;
    default:
      return 0u;
  }
}

uint64_t estimateTextureBytes(VkExtent2D extent, VkFormat format)
{
  return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height)
         * static_cast<uint64_t>(bytesPerPixelForFormat(format));
}

glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
  const float length = glm::length(value);
  return length > 1.0e-5f ? value / length : fallback;
}

glm::mat4 resolveSceneLightTransform(const SceneLight& light,
                                      std::span<const SceneNode> sceneNodes,
                                      std::span<const GltfNodeData> gltfNodes)
{
  if(light.nodeIndex >= 0)
  {
    const size_t nodeIndex = static_cast<size_t>(light.nodeIndex);
    if(nodeIndex < sceneNodes.size())
    {
      return sceneNodes[nodeIndex].worldTransform;
    }
    if(nodeIndex < gltfNodes.size())
    {
      return gltfNodes[nodeIndex].worldTransform;
    }
  }
  return glm::mat4(1.0f);
}

glm::vec3 sceneLightTravelDirection(const glm::mat4& transform)
{
  return safeNormalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)),
                       glm::vec3(-0.45f, -0.8f, -0.25f));
}

float sceneLightEffectiveRange(const SceneLight& light, const GPUDrivenSceneView& sceneView)
{
  if(light.range > 0.0f)
  {
    return light.range;
  }
  if(sceneView.sceneBoundsValid)
  {
    return std::max(glm::length(sceneView.sceneBoundsMax - sceneView.sceneBoundsMin) * 1.25f, 4.0f);
  }
  return 32.0f;
}

float halton(uint64_t index, uint32_t base)
{
  float result = 0.0f;
  float fraction = 1.0f / static_cast<float>(base);
  while(index > 0)
  {
    result += static_cast<float>(index % base) * fraction;
    index /= base;
    fraction /= static_cast<float>(base);
  }
  return result;
}

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

uint32_t buildMeshletGPUDrivenFlags(const MeshRecord& mesh)
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

glm::vec4 transformBoundsSphere(const glm::mat4& transform, const glm::vec4& localSphere)
{
  const glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(glm::vec3(localSphere), 1.0f));
  const float scaleX = glm::length(glm::vec3(transform[0]));
  const float scaleY = glm::length(glm::vec3(transform[1]));
  const float scaleZ = glm::length(glm::vec3(transform[2]));
  const float maxScale = std::max(scaleX, std::max(scaleY, scaleZ));
  return glm::vec4(worldCenter, std::max(localSphere.w * maxScale, 0.0f));
}

glm::vec4 computeTransformedBoundsSphere(const MeshRecord& meshRecord, const glm::mat4& transform)
{
  const glm::vec3 localMin = meshRecord.localBoundsMin;
  const glm::vec3 localMax = meshRecord.localBoundsMax;
  const glm::vec3 corners[8] = {
      {localMin.x, localMin.y, localMin.z},
      {localMax.x, localMin.y, localMin.z},
      {localMin.x, localMax.y, localMin.z},
      {localMax.x, localMax.y, localMin.z},
      {localMin.x, localMin.y, localMax.z},
      {localMax.x, localMin.y, localMax.z},
      {localMin.x, localMax.y, localMax.z},
      {localMax.x, localMax.y, localMax.z},
  };

  glm::vec3 worldMin = glm::vec3(transform * glm::vec4(corners[0], 1.0f));
  glm::vec3 worldMax = worldMin;
  for(uint32_t i = 1; i < 8; ++i) {
    const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corners[i], 1.0f));
    worldMin = glm::min(worldMin, worldCorner);
    worldMax = glm::max(worldMax, worldCorner);
  }

  const glm::vec3 center = (worldMin + worldMax) * 0.5f;
  const float     radius = glm::length(worldMax - center);
  return glm::vec4(center, radius);
}

void includeBoundsSphere(glm::vec3& boundsMin, glm::vec3& boundsMax, bool& boundsValid, const glm::vec4& sphere)
{
  if(sphere.w <= 0.0f)
  {
    return;
  }

  const glm::vec3 center = glm::vec3(sphere);
  const glm::vec3 extent(sphere.w);
  const glm::vec3 sphereMin = center - extent;
  const glm::vec3 sphereMax = center + extent;
  boundsMin = boundsValid ? glm::min(boundsMin, sphereMin) : sphereMin;
  boundsMax = boundsValid ? glm::max(boundsMax, sphereMax) : sphereMax;
  boundsValid = true;
}

void includeMeshBounds(glm::vec3& boundsMin, glm::vec3& boundsMax, bool& boundsValid, const MeshRecord& mesh)
{
  boundsMin = boundsValid ? glm::min(boundsMin, mesh.worldBoundsMin) : mesh.worldBoundsMin;
  boundsMax = boundsValid ? glm::max(boundsMax, mesh.worldBoundsMax) : mesh.worldBoundsMax;
  boundsValid = true;
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

void GPUDrivenRenderer::init(void* window, rhi::Surface& surface, bool vSync)
{
  m_renderer.init(window, surface, vSync);
  m_renderer.setBindingResolverOverride(this);
  m_sceneRegistry.init(getNativeDeviceHandle(), getAllocatorHandle());
  initLightingResources();
  initIBLResources();
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
  m_clusteredLightCullingPass = std::make_unique<GPUDrivenClusteredLightCullingPass>(this);
  m_csmShadowPass = std::make_unique<GPUDrivenCSMShadowPass>(this);
  m_shadowAtlasPass = std::make_unique<GPUDrivenShadowAtlasPass>(this);
  m_gbufferPass = std::make_unique<GPUDrivenGBufferPass>(this);
  m_lightPass = std::make_unique<GPUDrivenLightPass>(this);
  m_skyboxPass = std::make_unique<GPUDrivenSkyboxPass>(this);
  m_aoPass = std::make_unique<GPUDrivenAOPass>(this);
  m_ssrPass = std::make_unique<GPUDrivenSSRPass>(this);
  m_forwardPass = std::make_unique<GPUDrivenForwardPass>(this);
  m_velocityPass = std::make_unique<GPUDrivenVelocityPass>(this);
  m_taaResolvePass = std::make_unique<GPUDrivenTAAResolvePass>(this);
  m_bloomPrefilterPass = std::make_unique<GPUDrivenBloomPrefilterPass>(this);
  m_bloomDownsamplePass = std::make_unique<GPUDrivenBloomDownsamplePass>(this);
  m_finalColorPass = std::make_unique<GPUDrivenFinalColorPass>(this);
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
  m_passExecutor.addPass(*m_clusteredLightCullingPass);
  m_passExecutor.addPass(*m_csmShadowPass);
  m_passExecutor.addPass(*m_shadowAtlasPass);
  m_passExecutor.addPass(*m_gbufferPass);
  m_passExecutor.addPass(*m_aoPass);
  m_passExecutor.addPass(*m_ssrPass);
  m_passExecutor.addPass(*m_lightPass);
  m_passExecutor.addPass(*m_skyboxPass);
  m_passExecutor.addPass(*m_forwardPass);
  m_passExecutor.addPass(*m_velocityPass);
  m_passExecutor.addPass(*m_taaResolvePass);
  m_passExecutor.addPass(*m_bloomPrefilterPass);
  m_passExecutor.addPass(*m_bloomDownsamplePass);
  m_passExecutor.addPass(*m_finalColorPass);
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
  initPhase7Resources();
  bindPhase7PassResources();
  m_sortedBootstrapFrames.assign(std::max(1u, getSwapchainImageCount()), SortedBootstrapFrameState{});
  if(kEnableShippingVisibilitySort)
  {
    initVisibilitySortResources();
  }
  initLightingPipelines();
  initPhase7Pipelines();
}

void GPUDrivenRenderer::shutdown(rhi::Surface& surface)
{
  shutdownLightingPipelines();
  shutdownPhase7Pipelines();
  if(kEnableShippingVisibilitySort)
  {
    shutdownVisibilitySortResources();
  }
  shutdownTransparentVisibilityPatchResources();
  shutdownPhase7Resources();
  m_sortedBootstrapFrames.clear();
  m_passExecutor.clear();
  m_imguiPass.reset();
  m_presentPass.reset();
  m_debugPass.reset();
  m_taaResolvePass.reset();
  m_velocityPass.reset();
  m_forwardPass.reset();
  m_ssrPass.reset();
  m_aoPass.reset();
  m_skyboxPass.reset();
  m_lightPass.reset();
  m_gbufferPass.reset();
  m_shadowAtlasPass.reset();
  m_csmShadowPass.reset();
  m_lightCullingPass.reset();
  m_clusteredLightCullingPass.reset();
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
  shutdownIBLResources();
  shutdownLightingResources();
  m_sceneRegistry.deinit();
  m_renderer.shutdown(surface);
}

void GPUDrivenRenderer::resize(rhi::Extent2D size)
{
  m_renderer.resize(size);
  m_hiZDepthPyramid.resize(getSceneExtent());
  resizePhase7Resources();
  m_passExecutor.clearResourceBindings();
  bindStaticPassResources();
  m_passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_hiZDepthPyramid.getImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
  bindPhase7PassResources();
}

void GPUDrivenRenderer::render(const RenderParams& params)
{
  TRACY_ZONE_SCOPED("GPUDrivenRenderer::render");

  const bool sceneRenderingSuspended = m_suspendSceneRendering;
  {
    TRACY_ZONE_SCOPED("GPUDriven::HiZResize");
    m_hiZDepthPyramid.resize(getSceneExtent());
  }
  {
    TRACY_ZONE_SCOPED("GPUDriven::FlushSceneUploads");
    flushPendingSceneUploads();
  }
  {
    TRACY_ZONE_SCOPED("GPUDriven::RefreshSceneView");
    refreshSceneView();
  }
  {
    TRACY_ZONE_SCOPED("GPUDriven::UploadPersistentDrawData");
    uploadPersistentDrawData();
  }
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
  if(params.cameraUniforms != nullptr)
  {
    getCSMShadowResources().updateCascadeMatrices(*params.cameraUniforms,
                                                  params.lightSettings.direction,
                                                  params.lightSettings.shadowDistance,
                                                  m_sceneView.sceneBoundsMin,
                                                  m_sceneView.sceneBoundsMax,
                                                  m_sceneView.sceneBoundsValid);
  }
  updateGPUDrivenLights(params, frameIndex);
  m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
  m_runtimeStats.visibilityDiagnostics = GPUDrivenVisibilityDiagnostics{
      .safeObjectCount = safeObjectCount,
      .currentGPUCullingObjectCount = getGPUCullingObjectCount(frameIndex),
      .previousGPUCullingObjectCount = getPreviousGPUCullingObjectCount(frameIndex),
      .opaqueCapacity = static_cast<uint32_t>(m_opaqueDrawIndices.size()),
      .alphaCapacity = static_cast<uint32_t>(m_alphaTestDrawIndices.size()),
      .transparentCapacity = static_cast<uint32_t>(m_transparentDrawIndices.size()),
      .maxMobileTransparentDraws = kMobileMaxTransparentDraws,
      .transparentOrderingCpuSeeded = !m_transparentDrawIndices.empty(),
      .materialSortKeysCpuSeeded = true,
      .transparentCapacityOverflow = m_transparentDrawIndices.size() > kMobileMaxTransparentDraws,
  };
  if(kEnableShippingVisibilitySort && !sceneRenderingSuspended)
  {
    {
      TRACY_ZONE_SCOPED("GPUDriven::BuildVisibilitySortInputs");
      if(m_cachedStaticVisibilitySortTopologyVersion != m_sceneTopologyVersion)
      {
        TRACY_ZONE_SCOPED("GPUDriven::BuildStaticVisibilitySortInputs");
        m_cachedStaticVisibilitySortObjects.clear();
        m_cachedStaticVisibilitySortKeys.clear();
        const size_t staticSortInputCount = m_opaqueDrawIndices.size() + m_alphaTestDrawIndices.size();
        m_cachedStaticVisibilitySortObjects.reserve(staticSortInputCount);
        m_cachedStaticVisibilitySortKeys.reserve(staticSortInputCount);
        const auto appendStaticDraws = [&](std::span<const uint32_t> drawIndices, uint32_t categoryValue) {
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

            const uint32_t subKey =
                mesh->materialIndex >= 0 ? std::min(static_cast<uint32_t>(mesh->materialIndex), kVisibilitySortKeyMask)
                                         : kVisibilitySortKeyMask;
            m_cachedStaticVisibilitySortObjects.push_back(drawIndex);
            m_cachedStaticVisibilitySortKeys.push_back(encodeVisibilitySortKey(categoryValue, subKey));
          }
        };
        appendStaticDraws(m_opaqueDrawIndices, kVisibilitySortCategoryOpaque);
        appendStaticDraws(m_alphaTestDrawIndices, kVisibilitySortCategoryAlpha);
        m_cachedStaticVisibilitySortTopologyVersion = m_sceneTopologyVersion;
      }

      const glm::vec3 cameraPos =
          params.cameraUniforms != nullptr ? params.cameraUniforms->cameraPosition : glm::vec3(0.0f);
      const size_t totalSortInputCount = m_cachedStaticVisibilitySortObjects.size() + m_transparentDrawIndices.size();
      m_visibilitySortInputObjects.clear();
      m_visibilitySortInputKeys.clear();
      m_visibilitySortInputObjects.reserve(totalSortInputCount);
      m_visibilitySortInputKeys.reserve(totalSortInputCount);
      m_visibilitySortInputObjects.insert(m_visibilitySortInputObjects.end(),
                                          m_cachedStaticVisibilitySortObjects.begin(),
                                          m_cachedStaticVisibilitySortObjects.end());
      m_visibilitySortInputKeys.insert(m_visibilitySortInputKeys.end(),
                                       m_cachedStaticVisibilitySortKeys.begin(),
                                       m_cachedStaticVisibilitySortKeys.end());
      const auto appendTransparentDraws = [&](std::span<const uint32_t> drawIndices) {
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

          const glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
          const float distanceSquared = glm::dot(meshCenter - cameraPos, meshCenter - cameraPos);
          const uint32_t subKey = encodeSortableFloatKey(distanceSquared) >> 2u;

          m_visibilitySortInputObjects.push_back(drawIndex);
          m_visibilitySortInputKeys.push_back(encodeVisibilitySortKey(kVisibilitySortCategoryTransparent, subKey));
        }
      };
      appendTransparentDraws(m_transparentDrawIndices);
      m_runtimeStats.batchStats.sortPassCount =
          computeBitonicSortPassCount(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
      m_runtimeStats.visibilityDiagnostics.sortInputCount =
          static_cast<uint32_t>(m_visibilitySortInputObjects.size());
      m_runtimeStats.visibilityDiagnostics.sortPaddedCount =
          m_visibilitySortInputObjects.empty()
              ? 0u
              : nextPowerOfTwo(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
    }
    {
      TRACY_ZONE_SCOPED("GPUDriven::PrepareVisibilitySortInputs");
      prepareVisibilitySortInputs(frameIndex);
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
  m_previousTAAJitterUv = m_currentTAAJitterUv;
  m_currentTAAJitterUv = glm::vec2(0.0f);
  if(params.cameraUniforms != nullptr)
  {
    const shaderio::CameraUniforms unjitteredCamera = *params.cameraUniforms;
    if(!m_previousCameraValid)
    {
      m_previousCameraUniforms = unjitteredCamera;
      m_previousJitteredViewProjection = unjitteredCamera.viewProjection;
    }

    m_temporalCameraUniforms = unjitteredCamera;
    m_temporalCameraUniforms.prevView = m_previousCameraUniforms.view;
    m_temporalCameraUniforms.prevProjection = m_previousCameraUniforms.projection;
    m_temporalCameraUniforms.prevViewProjection = m_previousCameraUniforms.viewProjection;
    m_temporalCameraUniforms.unjitteredViewProjection = unjitteredCamera.viewProjection;
    m_temporalCameraUniforms.unjitteredInverseViewProjection = unjitteredCamera.inverseViewProjection;
    m_temporalCameraUniforms.prevUnjitteredViewProjection = m_previousCameraUniforms.viewProjection;
    m_temporalCameraUniforms.prevJitteredViewProjection = m_previousJitteredViewProjection;
    const VkExtent2D temporalExtent = getSceneExtent();
    if(gpuParams.debugOptions.enablePostProcessing && gpuParams.debugOptions.enableTAA
       && temporalExtent.width > 0u && temporalExtent.height > 0u)
    {
      const uint64_t jitterIndex = m_temporalFrameCounter + 1u;
      const glm::vec2 jitter = (glm::vec2(halton(jitterIndex, 2u), halton(jitterIndex, 3u)) - glm::vec2(0.5f))
                               * (2.0f * gpuParams.debugOptions.taaJitterScale);
      const glm::vec2 jitterNdc = glm::vec2(jitter.x / static_cast<float>(temporalExtent.width),
                                            jitter.y / static_cast<float>(temporalExtent.height));
      m_currentTAAJitterUv = jitterNdc * 0.5f;
      m_temporalCameraUniforms.projection[2][0] += jitterNdc.x;
      m_temporalCameraUniforms.projection[2][1] += jitterNdc.y;
      m_temporalCameraUniforms.viewProjection = m_temporalCameraUniforms.projection * m_temporalCameraUniforms.view;
      m_temporalCameraUniforms.inverseViewProjection = glm::inverse(m_temporalCameraUniforms.viewProjection);
    }
    gpuParams.cameraUniforms = &m_temporalCameraUniforms;
  }
  {
    TRACY_ZONE_SCOPED("GPUDriven::BuildRenderParams");
    if(isMeshletRenderingActive())
    {
      gpuParams.debugOptions.enableGPUOcclusionCulling = params.debugOptions.enableGPUMeshletOcclusionCulling;
    }
    m_lastHiZCameraDeltaDistance = 0.0f;
    m_lastHiZFastCameraFallbackTriggered = false;
    if(params.cameraUniforms != nullptr)
    {
      const glm::vec3 cameraPosition = params.cameraUniforms->cameraPosition;
      if(m_hiZCameraHistoryValid)
      {
        m_lastHiZCameraDeltaDistance = glm::length(cameraPosition - m_lastHiZCameraPosition);
        m_lastHiZFastCameraFallbackTriggered =
            m_lastHiZCameraDeltaDistance > kGPUDrivenHiZFastCameraFallbackDistance;
        if(m_lastHiZFastCameraFallbackTriggered)
        {
          gpuParams.debugOptions.enableGPUOcclusionCulling = false;
        }
      }
      m_lastHiZCameraPosition = cameraPosition;
      m_hiZCameraHistoryValid = true;
    }
    gpuParams.gpuDrivenSceneView =
        (!sceneRenderingSuspended && m_sceneView.usePersistentCullingObjects) ? &m_sceneView : nullptr;
    if(sceneRenderingSuspended || gpuParams.gpuDrivenSceneView != nullptr)
    {
      gpuParams.gltfModel = nullptr;
    }
    const uint32_t historyReadIndex = (frameIndex + 1u) & 1u;
    const uint32_t historyWriteIndex = frameIndex & 1u;
    m_sceneView.sceneColorHistoryReadImage = getSceneColorHistoryImage(historyReadIndex);
    m_sceneView.sceneColorHistoryReadView = getSceneColorHistoryView(historyReadIndex);
    m_sceneView.sceneColorHistoryWriteImage = getSceneColorHistoryImage(historyWriteIndex);
    m_sceneView.sceneColorHistoryWriteView = getSceneColorHistoryView(historyWriteIndex);
    updatePhase7Descriptors(frameIndex);
    m_passExecutor.bindTexture({
        .handle       = kPassVelocityHandle,
        .nativeImage  = reinterpret_cast<uint64_t>(m_sceneView.velocityImage),
        .aspect       = rhi::TextureAspect::color,
        .initialState = rhi::ResourceState::General,
        .isSwapchain  = false,
    });
    m_passExecutor.bindTexture({
        .handle       = kPassSceneColorHistoryReadHandle,
        .nativeImage  = reinterpret_cast<uint64_t>(m_sceneView.sceneColorHistoryReadImage),
        .aspect       = rhi::TextureAspect::color,
        .initialState = rhi::ResourceState::General,
        .isSwapchain  = false,
    });
    m_passExecutor.bindTexture({
        .handle       = kPassSceneColorHistoryWriteHandle,
        .nativeImage  = reinterpret_cast<uint64_t>(m_sceneView.sceneColorHistoryWriteImage),
        .aspect       = rhi::TextureAspect::color,
        .initialState = rhi::ResourceState::General,
        .isSwapchain  = false,
    });
  }
  {
    TRACY_ZONE_SCOPED("GPUDriven::submitPassGraph");
    submitPassGraph(gpuParams);
  }
  const VkDescriptorSet gpuCullingDescriptorSet =
      reinterpret_cast<VkDescriptorSet>(getGPUCullingDescriptorSetOpaque(frameIndex));
  {
    TRACY_ZONE_SCOPED("GPUDriven::PostSubmitStats");
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
    const HiZDepthPyramid::MobilePolicy& hiZPolicy = m_hiZDepthPyramid.getMobilePolicy();
    const VkExtent2D hiZSourceExtent = m_hiZDepthPyramid.getSourceExtent();
    const VkExtent2D hiZPyramidExtent = m_hiZDepthPyramid.getExtent();
    m_runtimeStats.hiZDiagnostics = GPUDrivenHiZDiagnostics{
        .sourceWidth = hiZSourceExtent.width,
        .sourceHeight = hiZSourceExtent.height,
        .pyramidWidth = hiZPyramidExtent.width,
        .pyramidHeight = hiZPyramidExtent.height,
        .mipCount = m_hiZDepthPyramid.getMipCount(),
        .fullMipCount = m_hiZDepthPyramid.getFullMipCount(),
        .policyDownsampleDivisor = hiZPolicy.downsampleDivisor,
        .policyMaxMipCount = hiZPolicy.maxMipCount,
        .policyMinMipSize = hiZPolicy.minMipSize,
        .estimatedMemoryBytes = m_hiZDepthPyramid.getEstimatedMemoryBytes(),
        .generation = m_hiZDepthPyramid.getGenerationCount(),
        .valid = m_hiZDepthPyramid.isValid(),
        .boundForGpuCulling = m_runtimeStats.ownsHiZVisibilityChain,
        .frustumCullingEnabled = gpuParams.debugOptions.enableGPUFrustumCulling,
        .occlusionCullingEnabled = gpuParams.debugOptions.enableGPUOcclusionCulling,
        .meshletOcclusionEnabled = gpuParams.debugOptions.enableGPUMeshletOcclusionCulling,
        .meshletConeCullingEnabled = gpuParams.debugOptions.enableGPUMeshletConeCulling,
        .depthEpsilon = kGPUDrivenHiZDepthEpsilon,
        .conservativeRadiusScale = kGPUDrivenHiZConservativeRadiusScale,
        .conservativeRadiusBias = kGPUDrivenHiZConservativeRadiusBias,
        .nearRejectEpsilon = kGPUDrivenHiZNearRejectEpsilon,
        .largeObjectFootprintThreshold = kGPUDrivenHiZLargeObjectFootprintThreshold,
        .fastCameraFallbackDistance = kGPUDrivenHiZFastCameraFallbackDistance,
        .cameraDeltaDistance = m_lastHiZCameraDeltaDistance,
        .fastCameraFallbackTriggered = m_lastHiZFastCameraFallbackTriggered,
    };
    const VkExtent2D postExtent = getSceneExtent();
    const VkFormat outputFormat = getOutputTextureFormat();
    const VkFormat sceneColorFormat = getSceneColorHdrFormat();
    constexpr VkFormat kRecommendedMobileHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    m_runtimeStats.postProcessDiagnostics = GPUDrivenPostProcessDiagnostics{
        .displayWidth = getSwapchainExtent().width,
        .displayHeight = getSwapchainExtent().height,
        .internalWidth = postExtent.width,
        .internalHeight = postExtent.height,
        .outputWidth = postExtent.width,
        .outputHeight = postExtent.height,
        .outputFormat = outputFormat,
        .sceneColorFormat = sceneColorFormat,
        .recommendedHdrFormat = kRecommendedMobileHdrFormat,
        .outputMemoryBytes = getOutputTextureEstimatedBytes(),
        .sceneColorMemoryBytes = getSceneColorHdrEstimatedBytes(),
        .recommendedHdrMemoryBytes = estimateTextureBytes(postExtent, kRecommendedMobileHdrFormat),
        .bloomHalfQuarterMemoryBytes = getBloomEstimatedBytes(),
        .fixedExposure = gpuParams.debugOptions.postExposure,
        .adaptiveExposureTarget = gpuParams.debugOptions.exposureTargetLuminance,
        .minAutoExposure = gpuParams.debugOptions.minAutoExposure,
        .maxAutoExposure = gpuParams.debugOptions.maxAutoExposure,
        .bloomIntensity = gpuParams.debugOptions.bloomIntensity,
        .bloomThreshold = gpuParams.debugOptions.bloomThreshold,
        .colorSaturation = gpuParams.debugOptions.colorSaturation,
        .colorContrast = gpuParams.debugOptions.colorContrast,
        .colorGamma = gpuParams.debugOptions.colorGamma,
        .colorLutStrength = gpuParams.debugOptions.colorLutStrength,
        .vignetteIntensity = gpuParams.debugOptions.vignetteIntensity,
        .lensDirtIntensity = gpuParams.debugOptions.lensDirtIntensity,
        .taaBlendWeight = gpuParams.debugOptions.taaBlendWeight,
        .taaJitterScale = gpuParams.debugOptions.taaJitterScale,
        .renderScale = gpuParams.debugOptions.renderScale,
        .upscaleMode = static_cast<uint32_t>(std::max(0, gpuParams.debugOptions.upscalingMode)),
        .hdrSceneColorActive = sceneColorFormat == kRecommendedMobileHdrFormat
                               && m_sceneView.sceneColorHdrImage != VK_NULL_HANDLE
                               && !getGPUDrivenLightHdrPipelineHandle().isNull(),
        .mobileHdrRecommended = true,
        .toneMapInLightPass = false,
        .finalColorPassActive = !getFinalColorPipelineHandle().isNull(),
        .velocityBufferActive = m_sceneView.velocityImage != VK_NULL_HANDLE
                                && !getVelocityPipelineHandle().isNull(),
        .taaPassActive = gpuParams.debugOptions.enablePostProcessing
                         && gpuParams.debugOptions.enableTAA
                         && !getTAAResolvePipelineHandle().isNull(),
        .taaHistoryValid = m_taaHistoryValid,
        .temporalUpscalingActive = false,
        .internalRenderScaleBlocked = gpuParams.debugOptions.renderScale < 0.999f,
        .exposurePassActive = gpuParams.debugOptions.enablePostProcessing,
        .adaptiveExposureActive = gpuParams.debugOptions.enablePostProcessing
                                  && gpuParams.debugOptions.enableAdaptiveExposure,
        .bloomPassActive = gpuParams.debugOptions.enableBloom
                           && gpuParams.debugOptions.enablePostProcessing
                           && !getBloomPrefilterPipelineHandle().isNull()
                           && !getBloomDownsamplePipelineHandle().isNull()
                           && !getBloomUpsamplePipelineHandle().isNull(),
        .colorGradingLutActive = gpuParams.debugOptions.enablePostProcessing
                                 && gpuParams.debugOptions.enableColorGrading
                                 && gpuParams.debugOptions.colorLutStrength > 0.0f
                                 && m_sceneView.colorGradingLutView != VK_NULL_HANDLE,
        .lensEffectsActive = gpuParams.debugOptions.enablePostProcessing
                             && gpuParams.debugOptions.enableLensEffects,
    };
    const VkExtent2D iblExtent = getIBLEnvironmentExtent();
    m_runtimeStats.iblDiagnostics = GPUDrivenIBLDiagnostics{
        .width = iblExtent.width,
        .height = iblExtent.height,
        .mipCount = getIBLEnvironmentMipCount(),
        .format = getIBLEnvironmentFormat(),
        .estimatedMemoryBytes = getIBLEnvironmentEstimatedBytes(),
        .intensity = gpuParams.debugOptions.iblIntensity,
        .enabled = gpuParams.debugOptions.enableIBL,
        .loaded = getIBLEnvironmentLoaded(),
        .fallback = getIBLUsingFallback(),
        .irradianceReady = m_iblResources.isSplitSumReady(),
        .prefilteredReady = m_iblResources.isSplitSumReady(),
        .brdfLutReady = m_iblResources.isInitialized(),
        .splitSumReady = m_iblResources.isSplitSumReady(),
        .debugMode = gpuParams.debugOptions.iblDebugMode,
        .path = getIBLEnvironmentPath(),
        .sourceMode = m_iblResources.isSplitSumReady()
                          ? "split_sum"
                          : (getIBLEnvironmentLoaded() ? "equirect_fallback" : "flat_ambient"),
        .status = getIBLEnvironmentStatus(),
    };
    m_lightResources.cacheClusterStats(frameIndex);
    const GPUDrivenLightResources::Diagnostics lightDiagnostics = m_lightResources.getDiagnostics();
    m_runtimeStats.clusteredLightingDiagnostics = GPUDrivenClusteredLightingDiagnostics{
        .gridX = lightDiagnostics.clusterGridX,
        .gridY = lightDiagnostics.clusterGridY,
        .gridZ = lightDiagnostics.clusterGridZ,
        .clusterCount = lightDiagnostics.clusterCount,
        .maxLightsPerCluster = lightDiagnostics.maxLightsPerCluster,
        .activePointLights = lightDiagnostics.activePointLights,
        .activeSpotLights = lightDiagnostics.activeSpotLights,
        .maxPointLights = lightDiagnostics.maxPointLights,
        .maxSpotLights = lightDiagnostics.maxSpotLights,
        .maxOccupancy = lightDiagnostics.maxOccupancy,
        .overflowClusterCount = lightDiagnostics.overflowClusterCount,
        .appendedLightReferences = lightDiagnostics.appendedLightReferences,
        .clusterMemoryBytes = lightDiagnostics.clusterMemoryBytes,
        .lightMemoryBytes = lightDiagnostics.lightMemoryBytes,
        .enabled = gpuParams.debugOptions.enableClusteredLighting,
        .resourcesOwned = lightDiagnostics.initialized,
        .descriptorsReady = lightDiagnostics.lightingDescriptorsReady && lightDiagnostics.clusteredDescriptorsReady,
        .fallbackActive = !gpuParams.debugOptions.enableClusteredLighting || !lightDiagnostics.clusteredDescriptorsReady,
    };
    const uint64_t aoBytes = estimateImageBytes(m_phase7HalfExtent, 2u) * 2u;
    const uint64_t ssrBytes = estimateImageBytes(m_phase7HalfExtent, 8u);
    m_runtimeStats.aoReflectionDiagnostics = GPUDrivenAOReflectionDiagnostics{
        .aoWidth = m_phase7HalfExtent.width,
        .aoHeight = m_phase7HalfExtent.height,
        .ssrWidth = m_phase7HalfExtent.width,
        .ssrHeight = m_phase7HalfExtent.height,
        .estimatedMemoryBytes = aoBytes + ssrBytes,
        .aoEnabled = gpuParams.debugOptions.enableAO,
        .aoReady = m_aoDenoised.view != VK_NULL_HANDLE && m_gtaoPipeline != VK_NULL_HANDLE && m_aoDenoisePipeline != VK_NULL_HANDLE,
        .ssrEnabled = gpuParams.debugOptions.enableSSR,
        .ssrReady = m_ssrRaw.view != VK_NULL_HANDLE && m_ssrTracePipeline != VK_NULL_HANDLE,
    };
    const uint32_t shadowAtlasTileSize = std::max(1u, m_shadowAtlasTileSize);
    const uint32_t shadowAtlasCapacity =
        (m_shadowAtlasExtent.width / shadowAtlasTileSize) * (m_shadowAtlasExtent.height / shadowAtlasTileSize);
    CSMShadowResources& shadowAtlasCsm = getCSMShadowResources();
    const bool shadowAtlasReady = m_shadowAtlas.view != VK_NULL_HANDLE && m_shadowAtlas.image != VK_NULL_HANDLE
                                  && !getCSMShadowPipelineHandle().isNull();
    const bool shadowAtlasHasScene = m_sceneView.usePersistentCullingObjects
                                     && m_sceneView.shadowPackedMeshes != nullptr
                                     && m_sceneView.shadowPackedMeshCount > 0
                                     && m_sceneView.shadowPackedVertexBuffer != VK_NULL_HANDLE
                                     && m_sceneView.shadowPackedIndexBuffer != VK_NULL_HANDLE;
    const uint32_t shadowAtlasExpectedTiles =
        gpuParams.debugOptions.enableShadowAtlas && shadowAtlasReady && shadowAtlasHasScene
            ? std::min(shadowAtlasCsm.getCascadeCount(), shadowAtlasCapacity)
            : 0u;
    const std::string shadowAtlasStatus =
        !gpuParams.debugOptions.enableShadowAtlas
            ? "Disabled; using CSM fallback"
            : (!shadowAtlasReady
                   ? "Enabled, but atlas image or CSM shadow pipeline is missing; using CSM fallback"
                   : (!shadowAtlasHasScene
                          ? "Enabled and allocated; no packed shadow casters this frame; using CSM fallback"
                          : "Rendering cascade tiles into GPUDriven shadow atlas; lighting still samples CSM fallback"));
    m_shadowAtlasAllocatedTiles = shadowAtlasExpectedTiles;
    m_runtimeStats.shadowAtlasDiagnostics = GPUDrivenShadowAtlasDiagnostics{
        .atlasWidth = m_shadowAtlasExtent.width,
        .atlasHeight = m_shadowAtlasExtent.height,
        .tileSize = shadowAtlasTileSize,
        .tileCapacity = shadowAtlasCapacity,
        .allocatedTiles = shadowAtlasExpectedTiles,
        .estimatedMemoryBytes = estimateImageBytes(m_shadowAtlasExtent, 4u),
        .enabled = gpuParams.debugOptions.enableShadowAtlas,
        .ready = shadowAtlasReady,
        .fallbackToCSM = true,
        .status = shadowAtlasStatus,
    };
    if(m_sceneView.usePersistentCullingObjects
       && getGPUCullingObjectCount(frameIndex) == safeObjectCount && safeObjectCount > 0u)
    {
      m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
    }
    updateOwnershipDiagnostics(frameIndex, sceneRenderingSuspended, safeObjectCount);
  }
  if(params.cameraUniforms != nullptr)
  {
    m_previousCameraUniforms = *params.cameraUniforms;
    m_previousJitteredViewProjection = m_temporalCameraUniforms.viewProjection;
    m_previousCameraValid = true;
  }
  m_taaHistoryValid = gpuParams.debugOptions.enablePostProcessing && gpuParams.debugOptions.enableTAA
                      && !getTAAResolvePipelineHandle().isNull();
  ++m_temporalFrameCounter;
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
  const VkDescriptorSet gpuCullingDescriptorSet =
      reinterpret_cast<VkDescriptorSet>(getGPUCullingDescriptorSetOpaque(frameIndex));
  if(gpuCullingDescriptorSet != VK_NULL_HANDLE)
  {
    m_hiZDepthPyramid.bindForCulling(gpuCullingDescriptorSet, 5);
  }
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
  const VkBuffer drawCountBuffer = reinterpret_cast<VkBuffer>(getGPUCullingDrawCountBufferOpaque(currentFrameIndex));
  if(objectCount == 0u || descriptorSet == VK_NULL_HANDLE || indirectBuffer == VK_NULL_HANDLE
     || drawCountBuffer == VK_NULL_HANDLE)
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

  const std::array<VkBufferMemoryBarrier2, 2> indirectBarriers{{
      VkBufferMemoryBarrier2{
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
      },
      VkBufferMemoryBarrier2{
          .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask        = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
          .dstAccessMask       = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .buffer              = drawCountBuffer,
          .offset              = 0,
          .size                = VK_WHOLE_SIZE,
      },
  }};
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(indirectBarriers.size()),
      .pBufferMemoryBarriers    = indirectBarriers.data(),
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

  const uint32_t pointLightCount = getActivePointLightCount();
  const uint32_t spotLightCount = getActiveSpotLightCount();

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
                      pipelineHandle.index == m_pointLightCoarseCullingPipeline.index
                          ? m_pointLightCoarseCullingVkPipeline
                          : m_spotLightCoarseCullingVkPipeline);
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

void GPUDrivenRenderer::executeClusteredLightCullingPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(!params.debugOptions.enableClusteredLighting || m_clusteredLightCullingVkPipeline == VK_NULL_HANDLE)
  {
    return;
  }
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  if(frameIndex >= m_lightCoarseCullingDescriptorSets.size())
  {
    return;
  }
  VkDescriptorSet descriptorSet = m_lightCoarseCullingDescriptorSets[frameIndex];
  if(descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  const VkBuffer statsBuffer = m_lightResources.getClusterStatsBuffer(frameIndex);
  if(statsBuffer != VK_NULL_HANDLE)
  {
    vkCmdFillBuffer(vkCmd, statsBuffer, 0, sizeof(GPUDrivenLightResources::ClusterStats), 0u);
    const VkMemoryBarrier2 statsResetBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
    };
    const VkDependencyInfo statsResetDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &statsResetBarrier,
    };
    vkCmdPipelineBarrier2(vkCmd, &statsResetDependency);
  }

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_clusteredLightCullingVkPipeline);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_lightCoarseCullingPipelineLayout,
                          0,
                          1,
                          &descriptorSet,
                          0,
                          nullptr);
  vkCmdDispatch(vkCmd, (shaderio::LClusterCount + 63u) / 64u, 1u, 1u);

  const VkMemoryBarrier2 memoryBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void GPUDrivenRenderer::executeAOPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(!params.debugOptions.enableAO || m_gtaoPipeline == VK_NULL_HANDLE || m_aoDenoisePipeline == VK_NULL_HANDLE)
  {
    return;
  }
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  if(frameIndex >= m_aoDescriptorSets.size() || frameIndex >= m_aoDenoiseDescriptorSets.size()
     || m_aoRaw.image == VK_NULL_HANDLE || m_aoDenoised.image == VK_NULL_HANDLE)
  {
    return;
  }
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  const shaderio::GPUDrivenAOPushConstants push{
      .params0 = glm::vec4(m_phase7HalfExtent.width, m_phase7HalfExtent.height, params.debugOptions.aoRadius, params.debugOptions.aoIntensity),
      .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, getSceneExtent().width)),
                           1.0f / static_cast<float>(std::max(1u, getSceneExtent().height)),
                           64.0f,
                           0.35f),
  };

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_gtaoPipeline);
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_aoPipelineLayout, 0, 1, &m_aoDescriptorSets[frameIndex], 0, nullptr);
  vkCmdPushConstants(vkCmd, m_aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(vkCmd, (m_phase7HalfExtent.width + 7u) / 8u, (m_phase7HalfExtent.height + 7u) / 8u, 1u);

  const VkMemoryBarrier2 computeBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &computeBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_aoDenoisePipeline);
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_aoPipelineLayout, 0, 1, &m_aoDenoiseDescriptorSets[frameIndex], 0, nullptr);
  vkCmdPushConstants(vkCmd, m_aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(vkCmd, (m_phase7HalfExtent.width + 7u) / 8u, (m_phase7HalfExtent.height + 7u) / 8u, 1u);
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void GPUDrivenRenderer::executeSSRPass(rhi::CommandList& cmd,
                                       const RenderParams& params,
                                       VkBuffer cameraBuffer,
                                       uint32_t cameraOffset)
{
  if(!params.debugOptions.enableSSR || m_ssrTracePipeline == VK_NULL_HANDLE || m_ssrRaw.image == VK_NULL_HANDLE
     || cameraBuffer == VK_NULL_HANDLE)
  {
    return;
  }
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  if(frameIndex >= m_ssrDescriptorSets.size())
  {
    return;
  }
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  const VkDescriptorBufferInfo cameraBufferInfo{
      .buffer = cameraBuffer,
      .offset = cameraOffset,
      .range = sizeof(shaderio::CameraUniforms),
  };
  const VkWriteDescriptorSet cameraWrite{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = m_ssrDescriptorSets[frameIndex],
      .dstBinding = 5,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = &cameraBufferInfo,
  };
  vkUpdateDescriptorSets(getNativeDeviceHandle(), 1, &cameraWrite, 0, nullptr);

  const shaderio::GPUDrivenSSRPushConstants push{
      .params0 = glm::vec4(m_phase7HalfExtent.width,
                           m_phase7HalfExtent.height,
                           static_cast<float>(std::max(1, params.debugOptions.ssrMaxSteps)),
                           params.debugOptions.ssrThickness),
      .params1 = glm::vec4(0.05f, 80.0f, 1.0f, 0.0f),
  };

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrTracePipeline);
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPipelineLayout, 0, 1, &m_ssrDescriptorSets[frameIndex], 0, nullptr);
  vkCmdPushConstants(vkCmd, m_ssrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(vkCmd, (m_phase7HalfExtent.width + 7u) / 8u, (m_phase7HalfExtent.height + 7u) / 8u, 1u);

  const VkMemoryBarrier2 memoryBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &memoryBarrier,
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

  const bool useShadowCulling = !getShadowCullingPipelineHandle().isNull()
                                && getShadowCullingPipelineLayout() != 0
                                && getShadowCullingDescriptorSetOpaque(frameIndex) != 0;
  if(!useShadowCulling)
  {
    LOGW("Skipping GPUDrivenCSMShadow: shadow indirect culling pipeline is unavailable");
    context.cmd->endEvent();
    return;
  }

  for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
  {
    if(useShadowCulling)
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
    cascadeCamera.prevView = cascadeCamera.view;
    cascadeCamera.prevProjection = cascadeCamera.projection;
    cascadeCamera.prevViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredInverseViewProjection = cascadeCamera.inverseViewProjection;
    cascadeCamera.prevUnjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.prevJitteredViewProjection = cascadeCamera.viewProjection;
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
      const uint32_t dynamicOffsets[] = {cameraAlloc.offset, 0u};
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout,
                              shaderio::LSetScene,
                              1,
                              &cameraDescriptorSet,
                              2,
                              dynamicOffsets);
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

void GPUDrivenRenderer::executeShadowAtlasPass(const PassContext& context)
{
  m_shadowAtlasAllocatedTiles = 0u;
  if(context.params == nullptr || context.transientAllocator == nullptr || context.cmd == nullptr
     || !context.params->debugOptions.enableShadowAtlas || !m_sceneView.usePersistentCullingObjects
     || m_sceneView.shadowPackedMeshes == nullptr || m_sceneView.shadowPackedMeshCount == 0
     || m_sceneView.shadowPackedVertexBuffer == VK_NULL_HANDLE || m_sceneView.shadowPackedIndexBuffer == VK_NULL_HANDLE
     || m_shadowAtlas.image == VK_NULL_HANDLE || m_shadowAtlas.view == VK_NULL_HANDLE)
  {
    return;
  }

  shaderio::ShadowUniforms* shadowData = getShadowUniformsData();
  if(shadowData == nullptr)
  {
    return;
  }

  const PipelineHandle shadowPipeline = getCSMShadowPipelineHandle();
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(getCSMShadowPipelineLayout());
  if(shadowPipeline.isNull() || pipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(getNativeGraphicsPipeline(shadowPipeline));
  if(nativePipeline == VK_NULL_HANDLE)
  {
    return;
  }

  CSMShadowResources& csm = getCSMShadowResources();
  const uint32_t tileSize = std::max(1u, m_shadowAtlasTileSize);
  const uint32_t tilesX = m_shadowAtlasExtent.width / tileSize;
  const uint32_t tilesY = m_shadowAtlasExtent.height / tileSize;
  const uint32_t tileCapacity = tilesX * tilesY;
  const uint32_t cascadeCount = std::min(csm.getCascadeCount(), tileCapacity);
  if(cascadeCount == 0u)
  {
    return;
  }

  const uint32_t frameIndex = context.frameIndex;
  const BindGroupHandle cameraBindGroupHandle = getCameraBindGroup(frameIndex);
  const BindGroupHandle drawBindGroupHandle = getCSMShadowMDIDrawBindGroup(frameIndex, 0);
  if(cameraBindGroupHandle.isNull() || drawBindGroupHandle.isNull())
  {
    return;
  }

  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(getGraphicsMaterialDescriptorSet());
  const VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
      getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  const VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
      getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  if(textureSet == VK_NULL_HANDLE || cameraDescriptorSet == VK_NULL_HANDLE || drawDescriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenShadowAtlas");
  const rhi::DepthTargetDesc depthTarget{
      .texture = rhi::TextureHandle{kPassGPUDrivenShadowAtlasHandle.index, kPassGPUDrivenShadowAtlasHandle.generation},
      .view = rhi::TextureViewHandle::fromNative(m_shadowAtlas.view),
      .state = rhi::ResourceState::DepthStencilAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };
  const rhi::Extent2D atlasExtent{m_shadowAtlasExtent.width, m_shadowAtlasExtent.height};
  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, atlasExtent},
      .colorTargets = nullptr,
      .colorTargetCount = 0,
      .depthTarget = &depthTarget,
  });

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
  rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout,
                          shaderio::LSetTextures,
                          1,
                          &textureSet,
                          0,
                          nullptr);
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

  for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
  {
    const uint32_t tileX = cascadeIndex % tilesX;
    const uint32_t tileY = cascadeIndex / tilesX;
    const int32_t viewportX = static_cast<int32_t>(tileX * tileSize);
    const int32_t viewportY = static_cast<int32_t>(tileY * tileSize);
    const rhi::Extent2D tileExtent{tileSize, tileSize};
    context.cmd->setViewport(rhi::Viewport{static_cast<float>(viewportX),
                                           static_cast<float>(viewportY),
                                           static_cast<float>(tileSize),
                                           static_cast<float>(tileSize),
                                           0.0f,
                                           1.0f});
    context.cmd->setScissor(rhi::Rect2D{{viewportX, viewportY}, tileExtent});

    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
    shaderio::CameraUniforms cascadeCamera{};
    cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
    cascadeCamera.projection = cascadeCamera.viewProjection;
    cascadeCamera.view = glm::mat4(1.0f);
    cascadeCamera.inverseViewProjection = glm::inverse(cascadeCamera.viewProjection);
    cascadeCamera.prevView = cascadeCamera.view;
    cascadeCamera.prevProjection = cascadeCamera.projection;
    cascadeCamera.prevViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredInverseViewProjection = cascadeCamera.inverseViewProjection;
    cascadeCamera.prevUnjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.prevJitteredViewProjection = cascadeCamera.viewProjection;
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

    const uint32_t dynamicOffsets[] = {cameraAlloc.offset, 0u};
    vkCmdBindDescriptorSets(vkCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout,
                            shaderio::LSetScene,
                            1,
                            &cameraDescriptorSet,
                            2,
                            dynamicOffsets);

    for(uint32_t meshIndex = 0; meshIndex < m_sceneView.shadowPackedMeshCount; ++meshIndex)
    {
      const ShadowPackedMesh& packedMesh = m_sceneView.shadowPackedMeshes[meshIndex];
      context.cmd->drawIndexed(packedMesh.indexCount, 1, packedMesh.firstIndex, packedMesh.vertexOffset, meshIndex);
    }
  }

  context.cmd->endRenderPass();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassGPUDrivenShadowAtlasHandle.index, kPassGPUDrivenShadowAtlasHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(m_shadowAtlas.image),
      .aspect = rhi::TextureAspect::depth,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::DepthStencilAttachment,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });
  m_shadowAtlasAllocatedTiles = cascadeCount;
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
  const uint32_t sceneDynamicOffsets[] = {cameraAlloc.offset, 0u};
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
                              2,
                              sceneDynamicOffsets);
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
                              2,
                              sceneDynamicOffsets);
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
  m_activeUploadResultStorage = result;
  rebuildGPUDrivenScene(model, m_activeUploadResultStorage, cmd);
  return result;
}

SceneUploadResult GPUDrivenRenderer::commitSceneUploadPlan(const SceneAsset& asset,
                                                           const SceneUploadPlan& plan,
                                                           VkCommandBuffer cmd)
{
  SceneUploadResult result = m_renderer.commitSceneUploadPlan(asset, plan, cmd);
  m_activeUploadResultStorage = result;
  rebuildGPUDrivenScene(asset, plan, m_activeUploadResultStorage, cmd);
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
  m_activeUploadResultStorage = ioResult;
  rebuildGPUDrivenScene(model, m_activeUploadResultStorage, cmd);
}

void GPUDrivenRenderer::destroyGltfResources(const GltfUploadResult& result)
{
  clearGPUDrivenScene();
  m_activeUploadResultStorage = {};
  m_activeUploadResult = nullptr;
  m_renderer.destroyGltfResources(result);
}

void GPUDrivenRenderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  const uint64_t meshKey = packMeshHandleKey(handle);
  if(const MeshRecord* previousMeshRecord = m_renderer.getMeshPool().tryGet(handle))
  {
    m_previousTransformByMeshHandle[meshKey] = previousMeshRecord->transform;
  }
  m_renderer.updateMeshTransform(handle, transform);

  uint32_t drawIndex = 0;
  const bool hasDrawIndex = tryGetMeshDrawIndex(handle, drawIndex);
  const auto objectIdsIt = m_objectIdsByMeshHandle.find(meshKey);
  const auto objectIdIt = m_objectIdByMeshHandle.find(meshKey);
  if(objectIdsIt == m_objectIdsByMeshHandle.end() && objectIdIt == m_objectIdByMeshHandle.end())
  {
    return;
  }

  const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(handle);
  if(meshRecord == nullptr)
  {
    return;
  }

  const glm::vec4 boundsSphere(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
  if(objectIdsIt != m_objectIdsByMeshHandle.end())
  {
    for(const uint32_t objectId : objectIdsIt->second)
    {
      m_sceneRegistry.updateTransform(objectId, transform, boundsSphere);
    }
  }
  else
  {
    m_sceneRegistry.updateTransform(objectIdIt->second, transform, boundsSphere);
  }
  if(m_enableExperimentalMeshletPath && !m_meshletCullObjectsCpu.empty())
  {
    for(uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size()); ++drawIndex)
    {
      if(packMeshHandleKey(m_meshHandleByDrawIndex[drawIndex]) != packMeshHandleKey(handle)
         || drawIndex >= m_meshletDataCpu.size() || drawIndex >= m_meshletCullObjectsCpu.size())
      {
        continue;
      }
      m_meshletCullObjectsCpu[drawIndex].sphereCenterRadius =
          transformBoundsSphere(meshRecord->transform, m_meshletDataCpu[drawIndex].boundsSphere);
      markPersistentDrawDirty(drawIndex);
    }
  }
  m_sceneUploadPending = true;
  if(!m_enableExperimentalMeshletPath)
  {
    bool markedDraws = false;
    for(uint32_t candidateDrawIndex = 0; candidateDrawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size());
        ++candidateDrawIndex)
    {
      if(packMeshHandleKey(m_meshHandleByDrawIndex[candidateDrawIndex]) == meshKey)
      {
        markPersistentDrawDirty(candidateDrawIndex);
        markedDraws = true;
      }
    }
    if(!markedDraws && hasDrawIndex)
    {
      markPersistentDrawDirty(drawIndex);
    }
  }
  else
  {
    m_persistentDrawDataDirty = true;
  }
  m_runtimeStats.pendingSceneUpdates = 1;
  refreshSceneView();
}

void GPUDrivenRenderer::updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform)
{
  if(m_activeUploadResult == nullptr || instanceIndex >= m_activeUploadResult->instanceToDrawRecord.size())
  {
    return;
  }

  const uint32_t drawRecordIndex = m_activeUploadResult->instanceToDrawRecord[instanceIndex];
  if(drawRecordIndex == UINT32_MAX || drawRecordIndex >= m_sceneDrawRecords.size())
  {
    return;
  }

  SceneUploadResult::SceneDrawRecord& drawRecord = m_sceneDrawRecords[drawRecordIndex];
  const MeshHandle meshHandle = drawRecord.meshHandle;
  if(meshHandle.isNull())
  {
    return;
  }

  const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
  if(meshRecord == nullptr)
  {
    return;
  }

  const glm::mat4 previousTransform = drawRecord.worldTransform;
  drawRecord.worldTransform = transform;
  const glm::vec4 boundsSphere = computeTransformedBoundsSphere(*meshRecord, transform);

  const uint32_t objectId =
      drawRecordIndex < m_objectIdByDrawRecord.size() ? m_objectIdByDrawRecord[drawRecordIndex] : UINT32_MAX;
  const uint32_t drawIndex =
      drawRecordIndex < m_drawIndexByDrawRecord.size() ? m_drawIndexByDrawRecord[drawRecordIndex] : UINT32_MAX;
  if(objectId != UINT32_MAX)
  {
    m_sceneRegistry.updateTransform(objectId, transform, boundsSphere);
  }
  if(drawIndex != UINT32_MAX)
  {
    m_previousTransformByDrawIndex[drawIndex] = previousTransform;
    markPersistentDrawDirty(drawIndex);
  }
  m_sceneUploadPending = true;
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
  const bool firstSceneBuild = m_objectIdByMeshHandle.empty();
  if(firstSceneBuild)
  {
    clearGPUDrivenScene();
    m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
  }
  m_activeUploadResult = &uploadResult;

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

    const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
    const uint32_t objectId = m_sceneRegistry.registerObject(desc);
    m_objectIdByMeshHandle[meshKey] = objectId;
    appendedObjects = true;

    if(m_enableExperimentalMeshletPath)
    {
      MeshletConversionResult meshlets = MeshletConverter::convert(model.meshes[meshIndex]);
      const uint32_t baseMeshletIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
      const uint32_t baseIndexOffset = static_cast<uint32_t>(m_meshletIndicesCpu.size());
      const uint32_t flags = buildMeshletGPUDrivenFlags(*meshRecord);
      for(uint32_t localMeshletIndex = 0; localMeshletIndex < static_cast<uint32_t>(meshlets.meshlets.size());
          ++localMeshletIndex)
      {
        shaderio::Meshlet& meshlet = meshlets.meshlets[localMeshletIndex];
        meshlet.materialIndex = desc.materialIndex;
        meshlet.objectIndex = objectId;
        meshlet.flags = flags;
        meshlet.localIndex = localMeshletIndex;
        meshlet.indexOffset += baseIndexOffset;
      }
      if(!meshlets.meshlets.empty())
      {
        m_drawIndexByMeshHandle.emplace(meshKey, baseMeshletIndex);
        m_meshletDataCpu.insert(m_meshletDataCpu.end(), meshlets.meshlets.begin(), meshlets.meshlets.end());
        appendedMeshlets = true;
        if(m_meshHandleByDrawIndex.size() < m_meshletDataCpu.size())
        {
          m_meshHandleByDrawIndex.resize(m_meshletDataCpu.size(), kNullMeshHandle);
        }
        for(uint32_t localMeshletIndex = 0; localMeshletIndex < static_cast<uint32_t>(meshlets.meshlets.size());
            ++localMeshletIndex)
        {
          const shaderio::Meshlet& meshlet = meshlets.meshlets[localMeshletIndex];
          const uint32_t drawIndex = baseMeshletIndex + localMeshletIndex;
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

          m_meshletCullObjectsCpu.push_back(shaderio::GPUCullObject{
              .sphereCenterRadius = transformBoundsSphere(meshRecord->transform, meshlet.boundsSphere),
              .indexCount = meshlet.indexCount,
              .firstIndex = meshlet.indexOffset,
              .vertexOffset = meshRecord->vertexOffset,
              .flags = flags,
          });
        }
      }
      if(!meshlets.packedIndices.empty())
      {
        m_meshletIndicesCpu.insert(m_meshletIndicesCpu.end(), meshlets.packedIndices.begin(), meshlets.packedIndices.end());
      }
      m_runtimeStats.meshletTriangleCount += meshlets.triangleCount;
      if(!meshlets.meshlets.empty())
      {
        continue;
      }
    }

    m_drawIndexByMeshHandle[meshKey] = objectDrawIndex;
    if(objectDrawIndex >= m_meshHandleByDrawIndex.size())
    {
      m_meshHandleByDrawIndex.resize(objectDrawIndex + 1u, kNullMeshHandle);
    }
    m_meshHandleByDrawIndex[objectDrawIndex] = meshHandle;
    if(meshRecord->alphaMode == shaderio::LAlphaBlend)
    {
      m_transparentDrawIndices.push_back(objectDrawIndex);
    }
    else if(meshRecord->alphaMode == shaderio::LAlphaMask)
    {
      m_alphaTestDrawIndices.push_back(objectDrawIndex);
    }
    else
    {
      m_opaqueDrawIndices.push_back(objectDrawIndex);
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
    m_meshletBuffer.uploadMeshlets(cmd, m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
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

void GPUDrivenRenderer::appendSceneObjectDraw(uint64_t meshKey, MeshHandle meshHandle, uint32_t drawIndex, SceneDrawBucket bucket)
{
  m_drawIndexByMeshHandle[meshKey] = drawIndex;
  if(drawIndex >= m_meshHandleByDrawIndex.size())
  {
    m_meshHandleByDrawIndex.resize(drawIndex + 1u, kNullMeshHandle);
  }
  m_meshHandleByDrawIndex[drawIndex] = meshHandle;

  switch(bucket)
  {
  case SceneDrawBucket::Transparent:
    m_transparentDrawIndices.push_back(drawIndex);
    break;
  case SceneDrawBucket::AlphaMask:
    m_alphaTestDrawIndices.push_back(drawIndex);
    break;
  case SceneDrawBucket::Opaque:
  default:
    m_opaqueDrawIndices.push_back(drawIndex);
    break;
  }
}

void GPUDrivenRenderer::rebuildGPUDrivenScene(const SceneAsset& asset,
                                              const SceneUploadPlan& plan,
                                              const SceneUploadResult& uploadResult,
                                              VkCommandBuffer cmd)
{
  const bool firstSceneBuild = m_objectIdByMeshHandle.empty();
  if(firstSceneBuild)
  {
    clearGPUDrivenScene();
    m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
  }
  m_activeUploadResult = &uploadResult;

  bool appendedObjects = false;
  bool appendedMeshlets = false;

  std::vector<SceneDrawBucket> primaryBucketByMeshIndex(asset.meshes.size(), SceneDrawBucket::Opaque);
  std::vector<bool>            hasPrimaryBucketByMeshIndex(asset.meshes.size(), false);
  std::vector<glm::vec4>       primaryBoundsSphereByMeshIndex(asset.meshes.size(), glm::vec4(0.0f));
  std::vector<bool>            hasPrimaryBoundsSphereByMeshIndex(asset.meshes.size(), false);

  for(const DrawCommandBuildPlan& draw : plan.drawCommands)
  {
    if(draw.meshIndex < primaryBucketByMeshIndex.size() && !hasPrimaryBucketByMeshIndex[draw.meshIndex])
    {
      primaryBucketByMeshIndex[draw.meshIndex] = draw.bucket;
      hasPrimaryBucketByMeshIndex[draw.meshIndex] = true;
    }
  }
  for(const InstanceCullRecord& cullRecord : plan.cullRecords)
  {
    if(cullRecord.meshIndex < primaryBoundsSphereByMeshIndex.size() && !hasPrimaryBoundsSphereByMeshIndex[cullRecord.meshIndex])
    {
      primaryBoundsSphereByMeshIndex[cullRecord.meshIndex] = cullRecord.boundingSphere;
      hasPrimaryBoundsSphereByMeshIndex[cullRecord.meshIndex] = true;
    }
  }

  if(!m_enableExperimentalMeshletPath && !uploadResult.drawRecords.empty())
  {
    m_sceneDrawRecords = uploadResult.drawRecords;
    m_objectIdByDrawRecord.assign(m_sceneDrawRecords.size(), UINT32_MAX);
    m_drawIndexByDrawRecord.assign(m_sceneDrawRecords.size(), UINT32_MAX);

    for(uint32_t drawRecordIndex = 0; drawRecordIndex < static_cast<uint32_t>(uploadResult.drawRecords.size()); ++drawRecordIndex)
    {
      const SceneUploadResult::SceneDrawRecord& drawRecord = uploadResult.drawRecords[drawRecordIndex];
      if(drawRecord.meshIndex >= uploadResult.meshes.size() || drawRecord.meshIndex >= asset.meshes.size())
      {
        continue;
      }

      const MeshHandle meshHandle = drawRecord.meshHandle;
      const uint64_t meshKey = packMeshHandleKey(meshHandle);
      const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
      if(meshRecord == nullptr)
      {
        continue;
      }

      GPUSceneRegistrationDesc desc{};
      desc.meshHandle = meshHandle;
      desc.meshIndex = drawRecord.meshIndex;
      desc.materialIndex = drawRecord.materialIndex;
      desc.transform = drawRecord.worldTransform;
      desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
      desc.flags = buildGPUDrivenFlags(*meshRecord);
      desc.indexCount = meshRecord->indexCount;
      desc.firstIndex = meshRecord->firstIndex;
      desc.vertexOffset = meshRecord->vertexOffset;
      if(drawRecord.boundsSphere.w > 0.0f)
      {
        desc.boundsSphere = drawRecord.boundsSphere;
      }

      const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
      const uint32_t objectId = m_sceneRegistry.registerObject(desc);
      if(m_objectIdByMeshHandle.find(meshKey) == m_objectIdByMeshHandle.end())
      {
        m_objectIdByMeshHandle[meshKey] = objectId;
      }
      if(drawRecordIndex < m_objectIdByDrawRecord.size())
      {
        m_objectIdByDrawRecord[drawRecordIndex] = objectId;
      }
      if(drawRecordIndex < m_drawIndexByDrawRecord.size())
      {
        m_drawIndexByDrawRecord[drawRecordIndex] = objectDrawIndex;
      }
      m_objectIdsByMeshHandle[meshKey].push_back(objectId);

      SceneDrawBucket bucket = SceneDrawBucket::Opaque;
      if(drawRecord.alphaMode == shaderio::LAlphaBlend)
      {
        bucket = SceneDrawBucket::Transparent;
      }
      else if(drawRecord.alphaMode == shaderio::LAlphaMask)
      {
        bucket = SceneDrawBucket::AlphaMask;
      }
      appendSceneObjectDraw(meshKey, meshHandle, objectDrawIndex, bucket);
      appendedObjects = true;
    }

    if(appendedObjects || firstSceneBuild)
    {
      ++m_sceneTopologyVersion;
      invalidateSortedBootstrapStates();
      m_sceneRegistry.syncToGpu(cmd);
    }
    m_sceneUploadPending = false;
    m_persistentDrawDataDirty = true;
    m_dirtyPersistentDrawIndices.clear();
    m_runtimeStats.sceneUploadCount += 1;
    m_runtimeStats.pendingSceneUpdates = 0;
    m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
    m_runtimeStats.meshletCount = 0u;
    refreshSceneView();
    return;
  }

  for(size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < asset.meshes.size(); ++meshIndex)
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

    const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
    SceneDrawBucket bucket = SceneDrawBucket::Opaque;
    if(meshIndex < primaryBucketByMeshIndex.size() && hasPrimaryBucketByMeshIndex[meshIndex])
    {
      bucket = primaryBucketByMeshIndex[meshIndex];
    }

    if(meshIndex < primaryBoundsSphereByMeshIndex.size() && hasPrimaryBoundsSphereByMeshIndex[meshIndex])
    {
      desc.boundsSphere = primaryBoundsSphereByMeshIndex[meshIndex];
    }

    const uint32_t objectId = m_sceneRegistry.registerObject(desc);
    m_objectIdByMeshHandle[meshKey] = objectId;
    appendedObjects = true;

    if(m_enableExperimentalMeshletPath && meshIndex < asset.meshletPayloads.size())
    {
      const SceneAsset::MeshletPayload& meshletPayload = asset.meshletPayloads[meshIndex];
      const size_t meshletStride = sizeof(shaderio::Meshlet);
      const uint32_t meshletCount =
          meshletStride > 0 ? static_cast<uint32_t>(meshletPayload.meshletData.size() / meshletStride) : 0u;
      if(meshletCount > 0)
      {
        const uint32_t baseMeshletIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
        const uint32_t baseIndexOffset = static_cast<uint32_t>(m_meshletIndicesCpu.size());
        const uint32_t flags = buildMeshletGPUDrivenFlags(*meshRecord);
        const shaderio::Meshlet* sourceMeshlets =
            reinterpret_cast<const shaderio::Meshlet*>(meshletPayload.meshletData.data());
        const uint32_t* sourcePackedIndices =
            reinterpret_cast<const uint32_t*>(meshletPayload.indexData.data());
        const uint32_t packedIndexCount = static_cast<uint32_t>(meshletPayload.indexData.size() / sizeof(uint32_t));

        m_drawIndexByMeshHandle.emplace(meshKey, baseMeshletIndex);
        if(m_meshHandleByDrawIndex.size() < baseMeshletIndex + meshletCount)
        {
          m_meshHandleByDrawIndex.resize(baseMeshletIndex + meshletCount, kNullMeshHandle);
        }
        appendedMeshlets = true;

        for(uint32_t localMeshletIndex = 0; localMeshletIndex < meshletCount; ++localMeshletIndex)
        {
          shaderio::Meshlet meshlet = sourceMeshlets[localMeshletIndex];
          meshlet.materialIndex = desc.materialIndex;
          meshlet.objectIndex = objectId;
          meshlet.flags = flags;
          meshlet.localIndex = localMeshletIndex;
          meshlet.indexOffset += baseIndexOffset;

          const uint32_t drawIndex = baseMeshletIndex + localMeshletIndex;
          m_meshletDataCpu.push_back(meshlet);
          m_meshHandleByDrawIndex[drawIndex] = meshHandle;

          switch(bucket)
          {
          case SceneDrawBucket::Transparent:
            m_transparentDrawIndices.push_back(drawIndex);
            break;
          case SceneDrawBucket::AlphaMask:
            m_alphaTestDrawIndices.push_back(drawIndex);
            break;
          case SceneDrawBucket::Opaque:
          default:
            m_opaqueDrawIndices.push_back(drawIndex);
            break;
          }

          m_meshletCullObjectsCpu.push_back(shaderio::GPUCullObject{
              .sphereCenterRadius = transformBoundsSphere(meshRecord->transform, meshlet.boundsSphere),
              .indexCount = meshlet.indexCount,
              .firstIndex = meshlet.indexOffset,
              .vertexOffset = meshRecord->vertexOffset,
              .flags = flags,
          });
        }

        if(sourcePackedIndices != nullptr && packedIndexCount > 0)
        {
          m_meshletIndicesCpu.insert(m_meshletIndicesCpu.end(),
                                     sourcePackedIndices,
                                     sourcePackedIndices + packedIndexCount);
        }
        m_runtimeStats.meshletTriangleCount += packedIndexCount / 3u;
        continue;
      }
    }

    appendSceneObjectDraw(meshKey, meshHandle, objectDrawIndex, bucket);
  }

  if(appendedObjects || firstSceneBuild)
  {
    ++m_sceneTopologyVersion;
    invalidateSortedBootstrapStates();
    m_sceneRegistry.syncToGpu(cmd);
  }
  if(m_enableExperimentalMeshletPath && (appendedMeshlets || firstSceneBuild))
  {
    m_meshletBuffer.uploadMeshlets(cmd, m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
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
  m_meshletCullObjectsCpu.clear();
  m_sceneDrawRecords.clear();
  m_persistentDrawData.clear();
  m_visibilitySortInputObjects.clear();
  m_visibilitySortInputKeys.clear();
  m_cachedStaticVisibilitySortObjects.clear();
  m_cachedStaticVisibilitySortKeys.clear();
  m_cachedStaticVisibilitySortTopologyVersion = 0;
  m_objectIdByMeshHandle.clear();
  m_objectIdsByMeshHandle.clear();
  m_drawIndexByMeshHandle.clear();
  m_previousTransformByMeshHandle.clear();
  m_previousTransformByDrawIndex.clear();
  m_objectIdByDrawRecord.clear();
  m_drawIndexByDrawRecord.clear();
  m_meshHandleByDrawIndex.clear();
  m_opaqueDrawIndices.clear();
  m_alphaTestDrawIndices.clear();
  m_transparentDrawIndices.clear();
  m_sceneView = {};
  m_runtimeStats = {};
  m_activeUploadResult = nullptr;
  m_sceneUploadPending = false;
  m_persistentDrawDataDirty = false;
  m_previousTransformResetPending = false;
  m_dirtyPersistentDrawIndices.clear();
  m_temporalCameraUniforms = {};
  m_previousCameraUniforms = {};
  m_previousJitteredViewProjection = glm::mat4(1.0f);
  m_currentTAAJitterUv = glm::vec2(0.0f);
  m_previousTAAJitterUv = glm::vec2(0.0f);
  m_previousCameraValid = false;
  m_taaHistoryValid = false;
  m_temporalFrameCounter = 0;
}

void GPUDrivenRenderer::flushPendingSceneUploads()
{
  if(!m_sceneUploadPending || (!m_sceneRegistry.isDirty() && !m_enableExperimentalMeshletPath))
  {
    return;
  }

  m_renderer.executeUploadCommand([this](VkCommandBuffer cmd) {
    if(m_sceneRegistry.isDirty())
    {
      m_sceneRegistry.syncToGpu(cmd);
    }
    if(m_enableExperimentalMeshletPath && !m_meshletDataCpu.empty())
    {
      m_meshletBuffer.uploadMeshlets(cmd, m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
    }
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

  if(!m_persistentDrawDataDirty && !m_previousTransformResetPending && !m_persistentDrawData.empty())
  {
    return;
  }

  const bool resetPreviousTransforms = m_previousTransformResetPending && !m_persistentDrawDataDirty;
  const bool needsFullUpload = m_persistentDrawData.size() != m_sceneView.objectCount || m_persistentDrawData.empty()
                               || m_dirtyPersistentDrawIndices.empty() || resetPreviousTransforms;
  if(needsFullUpload)
  {
    m_persistentDrawData.assign(m_sceneView.objectCount, shaderio::DrawUniforms{});
  }
  if(resetPreviousTransforms)
  {
    m_previousTransformByMeshHandle.clear();
    m_previousTransformByDrawIndex.clear();
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

    const SceneUploadResult::SceneDrawRecord* drawRecord =
        drawIndex < m_sceneDrawRecords.size() ? &m_sceneDrawRecords[drawIndex] : nullptr;
    const MaterialHandle drawMaterialHandle =
        drawRecord != nullptr ? drawRecord->materialHandle : kNullMaterialHandle;
    const Renderer::MaterialTextureIndices drawMaterialTextures =
        drawMaterialHandle.isNull() ? Renderer::MaterialTextureIndices{}
                                    : m_renderer.getMaterialTextureIndices(drawMaterialHandle, m_activeUploadResult);
    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = drawRecord != nullptr ? drawRecord->worldTransform : mesh->transform;
    drawData.prevModelMatrix = drawData.modelMatrix;
    const uint64_t meshKey = packMeshHandleKey(meshHandle);
    const auto previousDrawTransformIt = m_previousTransformByDrawIndex.find(drawIndex);
    const auto previousTransformIt = m_previousTransformByMeshHandle.find(meshKey);
    if(previousDrawTransformIt != m_previousTransformByDrawIndex.end())
    {
      drawData.prevModelMatrix = previousDrawTransformIt->second;
    }
    else if(previousTransformIt != m_previousTransformByMeshHandle.end())
    {
      drawData.prevModelMatrix = previousTransformIt->second;
    }
    const MeshRecord* materialSource = mesh;
    drawData.baseColorFactor = drawMaterialHandle.isNull() ? mesh->baseColorFactor
                                                           : m_renderer.getMaterialBaseColorFactor(drawMaterialHandle);
    drawData.baseColorTextureIndex = drawMaterialHandle.isNull() ? mesh->baseColorTextureIndex
                                                                 : drawMaterialTextures.baseColor;
    drawData.normalTextureIndex = drawMaterialHandle.isNull() ? mesh->normalTextureIndex
                                                              : drawMaterialTextures.normal;
    drawData.metallicRoughnessTextureIndex = drawMaterialHandle.isNull()
                                                 ? mesh->metallicRoughnessTextureIndex
                                                 : drawMaterialTextures.metallicRoughness;
    drawData.occlusionTextureIndex = drawMaterialHandle.isNull() ? mesh->occlusionTextureIndex
                                                                 : drawMaterialTextures.occlusion;
    drawData.emissiveTextureIndex = mesh->emissiveTextureIndex;
    drawData.metallicFactor = mesh->metallicFactor;
    drawData.roughnessFactor = mesh->roughnessFactor;
    drawData.normalScale = mesh->normalScale;
    drawData.occlusionStrength = mesh->occlusionStrength;
    drawData.emissiveFactor = mesh->emissiveFactor;
    drawData.materialWorkflow = mesh->materialWorkflow;
    drawData.alphaMode = drawMaterialHandle.isNull() ? materialSource->alphaMode : drawMaterialTextures.alphaMode;
    drawData.alphaCutoff = drawMaterialHandle.isNull() ? materialSource->alphaCutoff : drawMaterialTextures.alphaCutoff;
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
  bool hasMotionPreviousTransforms = !m_previousTransformByDrawIndex.empty();
  for(uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size()); ++drawIndex)
  {
    const MeshHandle meshHandle = m_meshHandleByDrawIndex[drawIndex];
    if(meshHandle.isNull())
    {
      continue;
    }
    const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
    if(mesh == nullptr)
    {
      continue;
    }
    const uint64_t meshKey = packMeshHandleKey(meshHandle);
    hasMotionPreviousTransforms = hasMotionPreviousTransforms
                                  || (m_previousTransformByMeshHandle.find(meshKey) != m_previousTransformByMeshHandle.end());
    m_previousTransformByMeshHandle[meshKey] = mesh->transform;
  }
  m_previousTransformResetPending = hasMotionPreviousTransforms && !resetPreviousTransforms;
  m_persistentDrawDataDirty = false;
  m_dirtyPersistentDrawIndices.clear();
}

void GPUDrivenRenderer::refreshSceneView()
{
  m_sceneView.gpuSceneObjectBufferAddress = m_sceneRegistry.getBufferAddress();
  m_sceneView.gpuCullObjectBufferAddress = m_sceneRegistry.getCullBufferAddress();
  m_sceneView.gpuCullObjectBuffer = m_sceneRegistry.getCullBufferHandle();
  m_sceneView.gpuCullSceneObjectBuffer = m_sceneRegistry.getBufferHandle();
  m_sceneView.gpuCullMeshletBuffer = VK_NULL_HANDLE;
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
  if(m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
     && m_meshletBuffer.getMeshletCullObjectBuffer() != VK_NULL_HANDLE)
  {
    m_sceneView.gpuCullObjectBufferAddress = m_meshletBuffer.getMeshletCullObjectAddress();
    m_sceneView.gpuCullObjectBuffer = m_meshletBuffer.getMeshletCullObjectBuffer();
    m_sceneView.gpuCullMeshletBuffer = m_meshletBuffer.getMeshletDataBuffer();
    m_sceneView.objectCount = m_meshletBuffer.getMeshletCount();
    m_sceneView.overlayObjects = m_meshletCullObjectsCpu.empty() ? nullptr : m_meshletCullObjectsCpu.data();
    m_sceneView.overlayObjectCount = static_cast<uint32_t>(m_meshletCullObjectsCpu.size());
    m_sceneView.usePersistentCullingObjects = true;
    m_sceneView.authority = GPUDrivenSceneAuthority::persistentCullObjects;
    m_sceneView.indirectSource = GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect;
    m_sceneView.indirectCommandStride = m_renderer.getGPUCullingIndirectCommandStride();
  }
  m_sceneView.meshHandles = m_activeUploadResult != nullptr && !m_activeUploadResult->meshes.empty()
                                ? m_activeUploadResult->meshes.data()
                                : nullptr;
  m_sceneView.meshHandleCount =
      m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->meshes.size()) : 0;
  m_sceneView.drawMeshHandles = m_activeUploadResult != nullptr && !m_activeUploadResult->drawMeshHandles.empty()
                                    ? m_activeUploadResult->drawMeshHandles.data()
                                    : nullptr;
  m_sceneView.drawMeshHandleCount =
      m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->drawMeshHandles.size()) : 0;
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
  for(uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
  {
    m_sceneView.gbufferImages[i] = getSceneGBufferImage(i);
    m_sceneView.gbufferViews[i] = getSceneGBufferImageView(i);
  }
  m_sceneView.outputImage = getOutputTextureImage();
  m_sceneView.outputView = getOutputTextureView();
  m_sceneView.sceneColorHdrImage = getSceneColorHdrImage();
  m_sceneView.sceneColorHdrView = getSceneColorHdrView();
  m_sceneView.bloomHalfImage = getBloomHalfImage();
  m_sceneView.bloomHalfView = getBloomHalfView();
  m_sceneView.bloomHalfExtent = getBloomHalfExtent();
  m_sceneView.bloomQuarterImage = getBloomQuarterImage();
  m_sceneView.bloomQuarterView = getBloomQuarterView();
  m_sceneView.bloomQuarterExtent = getBloomQuarterExtent();
  m_sceneView.bloomEighthImage = getBloomEighthImage();
  m_sceneView.bloomEighthView = getBloomEighthView();
  m_sceneView.bloomEighthExtent = getBloomEighthExtent();
  m_sceneView.bloomSixteenthImage = getBloomSixteenthImage();
  m_sceneView.bloomSixteenthView = getBloomSixteenthView();
  m_sceneView.bloomSixteenthExtent = getBloomSixteenthExtent();
  m_sceneView.bloomThirtySecondImage = getBloomThirtySecondImage();
  m_sceneView.bloomThirtySecondView = getBloomThirtySecondView();
  m_sceneView.bloomThirtySecondExtent = getBloomThirtySecondExtent();
  m_sceneView.bloomUpsampleSixteenthImage = getBloomUpsampleSixteenthImage();
  m_sceneView.bloomUpsampleSixteenthView = getBloomUpsampleSixteenthView();
  m_sceneView.bloomUpsampleSixteenthExtent = getBloomUpsampleSixteenthExtent();
  m_sceneView.bloomUpsampleEighthImage = getBloomUpsampleEighthImage();
  m_sceneView.bloomUpsampleEighthView = getBloomUpsampleEighthView();
  m_sceneView.bloomUpsampleEighthExtent = getBloomUpsampleEighthExtent();
  m_sceneView.bloomUpsampleQuarterImage = getBloomUpsampleQuarterImage();
  m_sceneView.bloomUpsampleQuarterView = getBloomUpsampleQuarterView();
  m_sceneView.bloomUpsampleQuarterExtent = getBloomUpsampleQuarterExtent();
  m_sceneView.bloomOutputImage = getBloomOutputImage();
  m_sceneView.bloomOutputView = getBloomOutputView();
  m_sceneView.bloomOutputExtent = getBloomOutputExtent();
  m_sceneView.colorGradingLutImage = getColorGradingLutImage();
  m_sceneView.colorGradingLutView = getColorGradingLutView();
  m_sceneView.colorGradingLutExtent = getColorGradingLutExtent();
  m_sceneView.velocityImage = getVelocityImage();
  m_sceneView.velocityView = getVelocityView();
  m_sceneView.sceneColorHistoryReadImage = getSceneColorHistoryImage(1);
  m_sceneView.sceneColorHistoryReadView = getSceneColorHistoryView(1);
  m_sceneView.sceneColorHistoryWriteImage = getSceneColorHistoryImage(0);
  m_sceneView.sceneColorHistoryWriteView = getSceneColorHistoryView(0);
  m_sceneView.depthPyramidImage = m_hiZDepthPyramid.getImage();
  m_sceneView.depthPyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
  m_sceneView.depthPyramidMipCount = m_hiZDepthPyramid.getMipCount();
  m_sceneView.depthPyramidSourceDepth = m_hiZDepthPyramid.getSourceDepth();
  m_sceneView.depthPyramidGeneration = m_hiZDepthPyramid.getGenerationCount();
  m_sceneView.depthPyramidValid = m_hiZDepthPyramid.isValid();
  if(m_activeUploadResult != nullptr)
  {
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    bool boundsValid = false;

    if(!m_activeUploadResult->shadowPackedMeshes.empty())
    {
      for(const ShadowPackedMesh& packedMesh : m_activeUploadResult->shadowPackedMeshes)
      {
        includeBoundsSphere(boundsMin, boundsMax, boundsValid, packedMesh.boundsSphere);
      }
    }

    if(!boundsValid && !m_sceneDrawRecords.empty())
    {
      for(const SceneUploadResult::SceneDrawRecord& drawRecord : m_sceneDrawRecords)
      {
        includeBoundsSphere(boundsMin, boundsMax, boundsValid, drawRecord.boundsSphere);
      }
    }

    if(!boundsValid)
    {
      for(const MeshHandle meshHandle : m_activeUploadResult->meshes)
      {
        const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
        if(mesh == nullptr)
        {
          continue;
        }
        includeMeshBounds(boundsMin, boundsMax, boundsValid, *mesh);
      }
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
  const HiZDepthPyramid::MobilePolicy& hiZPolicy = m_hiZDepthPyramid.getMobilePolicy();
  const VkExtent2D hiZSourceExtent = m_hiZDepthPyramid.getSourceExtent();
  const VkExtent2D hiZPyramidExtent = m_hiZDepthPyramid.getExtent();
  m_runtimeStats.hiZDiagnostics = GPUDrivenHiZDiagnostics{
      .sourceWidth = hiZSourceExtent.width,
      .sourceHeight = hiZSourceExtent.height,
      .pyramidWidth = hiZPyramidExtent.width,
      .pyramidHeight = hiZPyramidExtent.height,
      .mipCount = m_hiZDepthPyramid.getMipCount(),
      .fullMipCount = m_hiZDepthPyramid.getFullMipCount(),
      .policyDownsampleDivisor = hiZPolicy.downsampleDivisor,
      .policyMaxMipCount = hiZPolicy.maxMipCount,
      .policyMinMipSize = hiZPolicy.minMipSize,
      .estimatedMemoryBytes = m_hiZDepthPyramid.getEstimatedMemoryBytes(),
      .generation = m_hiZDepthPyramid.getGenerationCount(),
      .valid = m_hiZDepthPyramid.isValid(),
      .boundForGpuCulling = false,
      .depthEpsilon = kGPUDrivenHiZDepthEpsilon,
      .conservativeRadiusScale = kGPUDrivenHiZConservativeRadiusScale,
      .conservativeRadiusBias = kGPUDrivenHiZConservativeRadiusBias,
      .nearRejectEpsilon = kGPUDrivenHiZNearRejectEpsilon,
      .largeObjectFootprintThreshold = kGPUDrivenHiZLargeObjectFootprintThreshold,
      .fastCameraFallbackDistance = kGPUDrivenHiZFastCameraFallbackDistance,
      .cameraDeltaDistance = m_lastHiZCameraDeltaDistance,
      .fastCameraFallbackTriggered = m_lastHiZFastCameraFallbackTriggered,
  };
}

uint32_t GPUDrivenRenderer::getSafePersistentObjectCount() const
{
  uint32_t safeCount = m_sceneView.objectCount;
  const bool meshletCullStreamActive =
      m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
      && m_sceneView.gpuCullObjectBuffer == m_meshletBuffer.getMeshletCullObjectBuffer();
  if(!meshletCullStreamActive && m_sceneView.drawMeshHandleCount > 0u)
  {
    safeCount = std::min(safeCount, m_sceneView.drawMeshHandleCount);
  }
  else if(!meshletCullStreamActive && m_sceneView.meshHandleCount > 0u)
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

void GPUDrivenRenderer::recordDepthPrepassVisibilitySource(bool     usedPreviousFrameIndirect,
                                                           bool     usedSortedBootstrap,
                                                           uint32_t previousObjectCount,
                                                           uint32_t opaqueMaxDrawCount,
                                                           uint32_t alphaMaxDrawCount)
{
  m_runtimeStats.visibilityDiagnostics.depthUsesPreviousFrameIndirect = usedPreviousFrameIndirect;
  m_runtimeStats.visibilityDiagnostics.depthUsesSortedBootstrap = usedSortedBootstrap;
  m_runtimeStats.visibilityDiagnostics.previousGPUCullingObjectCount = previousObjectCount;
  m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity =
      std::max(m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity, opaqueMaxDrawCount);
  m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity =
      std::max(m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity, alphaMaxDrawCount);
}

void GPUDrivenRenderer::recordGBufferVisibilityPatch(bool patched, uint32_t opaqueCapacity, uint32_t alphaCapacity)
{
  m_runtimeStats.visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched = patched;
  if(patched)
  {
    m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity = opaqueCapacity;
    m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity = alphaCapacity;
  }
}

void GPUDrivenRenderer::recordForwardVisibilityPatch(bool patched,
                                                     uint32_t transparentCapacity,
                                                     uint32_t)
{
  m_runtimeStats.visibilityDiagnostics.transparentPatchDispatched = patched;
  m_runtimeStats.visibilityDiagnostics.sameFrameTransparentCapacity = patched ? transparentCapacity : 0u;
  m_runtimeStats.visibilityDiagnostics.transparentCapacityOverflow =
      transparentCapacity > m_runtimeStats.visibilityDiagnostics.maxMobileTransparentDraws;
}

void GPUDrivenRenderer::initLightingResources()
{
  m_lightResources.init(getNativeDeviceHandle(),
                        getAllocatorHandle(),
                        GPUDrivenLightResources::CreateInfo{
                            .maxPointLights = 256,
                            .maxSpotLights = 128,
                            .frameCount = std::max(1u, getSwapchainImageCount()),
                        });

  const VkDevice nativeDevice = getNativeDeviceHandle();
  const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
  const VkSamplerCreateInfo samplerInfo{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = VK_LOD_CLAMP_NONE,
  };
  VK_CHECK(vkCreateSampler(nativeDevice, &samplerInfo, nullptr, &m_linearClampSampler));

  const std::array<VkDescriptorPoolSize, 4> poolSizes{{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount * (kGPUDrivenLightPassTextureCount + 4u)},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 12u},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount * 7u},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, frameCount * 2u},
  }};
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = frameCount * 3u,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
  };
  VK_CHECK(vkCreateDescriptorPool(nativeDevice, &poolInfo, nullptr, &m_lightingDescriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 12> lightingBindings{{
      VkDescriptorSetLayoutBinding{shaderio::LBindTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kGPUDrivenLightPassTextureCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindShadowMap, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindIBLIrradiance, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindIBLPrefiltered, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindIBLBrdfLut, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo lightingLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(lightingBindings.size()),
      .pBindings = lightingBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &lightingLayoutInfo, nullptr, &m_lightingSetLayout));

  const std::array<VkDescriptorSetLayoutBinding, 4> sceneBindings{{
      VkDescriptorSetLayoutBinding{shaderio::LBindCamera, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindLighting, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindLightCulling, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{shaderio::LBindPostProcess, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo sceneLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(sceneBindings.size()),
      .pBindings = sceneBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &sceneLayoutInfo, nullptr, &m_lightingSceneSetLayout));

  const std::array<VkDescriptorSetLayoutBinding, 9> cullingBindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo cullingLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(cullingBindings.size()),
      .pBindings = cullingBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &cullingLayoutInfo, nullptr, &m_lightCoarseCullingSetLayout));

  std::vector<VkDescriptorSetLayout> lightingLayouts(frameCount, m_lightingSetLayout);
  m_lightingDescriptorSets.resize(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo lightingAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_lightingDescriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts = lightingLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &lightingAlloc, m_lightingDescriptorSets.data()));

  // Wrap each lighting-input set (set LSetTextures) as a BindGroup so screen-space
  // passes can bind it through cmd->bindBindGroup. The adapter objects are owned by
  // the bind-group pool (deleted in destroyBindGroups), matching the convention used
  // for the camera/material bind groups.
  m_lightingInputBindGroups.assign(frameCount, BindGroupHandle{});
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    BindGroupDesc inputBindGroupDesc{
        .slot                = BindGroupSetSlot::shaderSpecific,
        .layout              = new rhi::vulkan::AdoptedBindTableLayout(reinterpret_cast<uint64_t>(m_lightingSetLayout)),
        .table               = new rhi::vulkan::AdoptedBindTable(reinterpret_cast<uint64_t>(m_lightingDescriptorSets[i])),
        .primaryLogicalIndex = shaderio::LBindTextures,
        .debugName           = "gpu-driven-lighting-input",
    };
    m_lightingInputBindGroups[i] = m_renderer.registerExternalBindGroup(inputBindGroupDesc);
  }

  std::vector<VkDescriptorSetLayout> cullingLayouts(frameCount, m_lightCoarseCullingSetLayout);
  m_lightCoarseCullingDescriptorSets.resize(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo cullingAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_lightingDescriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts = cullingLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &cullingAlloc, m_lightCoarseCullingDescriptorSets.data()));

  std::vector<VkDescriptorSetLayout> sceneLayouts(frameCount, m_lightingSceneSetLayout);
  m_lightingSceneDescriptorSets.resize(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo sceneAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_lightingDescriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts = sceneLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &sceneAlloc, m_lightingSceneDescriptorSets.data()));
  m_lightingSceneDescriptorTransientBuffers.assign(frameCount, VK_NULL_HANDLE);

  // Wrap each lighting-scene set as a BindGroup so GPUDriven fullscreen passes can
  // bind it through cmd->bindBindGroup. The adapters adopt the existing set/layout
  // and are owned here; they live until shutdownLightingResources().
  m_lightingSceneBindGroups.assign(frameCount, BindGroupHandle{});
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    BindGroupDesc sceneBindGroupDesc{
        .slot                = BindGroupSetSlot::shaderSpecific,
        .layout              = new rhi::vulkan::AdoptedBindTableLayout(reinterpret_cast<uint64_t>(m_lightingSceneSetLayout)),
        .table               = new rhi::vulkan::AdoptedBindTable(reinterpret_cast<uint64_t>(m_lightingSceneDescriptorSets[i])),
        .primaryLogicalIndex = shaderio::LBindCamera,
        .debugName           = "gpu-driven-lighting-scene",
    };
    m_lightingSceneBindGroups[i] = m_renderer.registerExternalBindGroup(sceneBindGroupDesc);
  }

  const VkPipelineLayoutCreateInfo cullingPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_lightCoarseCullingSetLayout,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &cullingPipelineLayoutInfo, nullptr, &m_lightCoarseCullingPipelineLayout));

  const std::array<VkDescriptorSetLayout, 2> lightSetLayouts{m_lightingSetLayout, m_lightingSceneSetLayout};
  const VkPipelineLayoutCreateInfo lightPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<uint32_t>(lightSetLayouts.size()),
      .pSetLayouts = lightSetLayouts.data(),
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &lightPipelineLayoutInfo, nullptr, &m_lightPipelineLayout));
}

void GPUDrivenRenderer::shutdownLightingResources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice != VK_NULL_HANDLE)
  {
    if(m_lightPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_lightPipelineLayout, nullptr);
      m_lightPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_lightCoarseCullingPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_lightCoarseCullingPipelineLayout, nullptr);
      m_lightCoarseCullingPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_lightingSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_lightingSetLayout, nullptr);
      m_lightingSetLayout = VK_NULL_HANDLE;
    }
    if(m_lightingSceneSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_lightingSceneSetLayout, nullptr);
      m_lightingSceneSetLayout = VK_NULL_HANDLE;
    }
    if(m_lightCoarseCullingSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_lightCoarseCullingSetLayout, nullptr);
      m_lightCoarseCullingSetLayout = VK_NULL_HANDLE;
    }
    if(m_lightingDescriptorPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(nativeDevice, m_lightingDescriptorPool, nullptr);
      m_lightingDescriptorPool = VK_NULL_HANDLE;
    }
    if(m_linearClampSampler != VK_NULL_HANDLE)
    {
      vkDestroySampler(nativeDevice, m_linearClampSampler, nullptr);
      m_linearClampSampler = VK_NULL_HANDLE;
    }
  }
  m_lightingDescriptorSets.clear();
  m_lightingSceneDescriptorSets.clear();
  m_lightingSceneDescriptorTransientBuffers.clear();
  // Adapter objects are deleted by m_renderer's destroyBindGroups() during
  // shutdown; here we only drop the (now-stale) handles.
  m_lightingSceneBindGroups.clear();
  m_lightingInputBindGroups.clear();
  m_lightCoarseCullingDescriptorSets.clear();
  m_lightResources.deinit();
  m_gpuDrivenPointLights.clear();
  m_gpuDrivenSpotLights.clear();
}

void GPUDrivenRenderer::initIBLResources()
{
  shutdownIBLResources();
  m_iblEnvironmentPath = kGPUDrivenDefaultIBLEnvironmentPath;
  m_iblEnvironmentStatus = "Using flat ambient fallback";
  m_iblUsingFallback = true;

  const VkDevice nativeDevice = getNativeDeviceHandle();
  const auto initFallbackSplitSumResources = [&]() {
    IBLResources::CreateInfo fallbackInfo{
        .cubeMapSize = 128,
        .dfgLUTSize = 256,
    };
    executeUploadCommand([&](VkCommandBuffer cmd) { m_iblResources.init(nativeDevice, getAllocatorHandle(), cmd, fallbackInfo); });
  };

  Ktx2Loader loader;
  Ktx2Loader::Ktx2Texture texture{};
  const std::filesystem::path path(kGPUDrivenDefaultIBLEnvironmentPath);
  if(!std::filesystem::exists(path))
  {
    m_iblEnvironmentStatus = "KTX2 environment not found: " + path.string();
    LOGW("%s", m_iblEnvironmentStatus.c_str());
    initFallbackSplitSumResources();
    return;
  }
  if(!loader.load(path, texture) || texture.data.empty() || texture.width == 0 || texture.height == 0)
  {
    m_iblEnvironmentStatus = "Failed to load GPUDriven IBL KTX2: " + loader.getLastError();
    LOGW("%s", m_iblEnvironmentStatus.c_str());
    initFallbackSplitSumResources();
    return;
  }

  const VkImageCreateInfo imageInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = texture.format,
      .extent = {texture.width, texture.height, 1},
      .mipLevels = std::max(1u, texture.mipLevels),
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  utils::Image image{};
  VK_CHECK(vmaCreateImage(getAllocatorHandle(), &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr));

  executeUploadCommand([&](VkCommandBuffer cmd) {
    BatchUploadContext upload;
    upload.init(nativeDevice, getAllocatorHandle(), static_cast<VkDeviceSize>(texture.data.size()));
    utils::cmdInitImageLayout(cmd, image.image);
    for(uint32_t level = 0; level < std::max(1u, texture.mipLevels); ++level)
    {
      if(level >= texture.mipOffsets.size() || level >= texture.mipSizes.size())
      {
        continue;
      }
      const VkDeviceSize offset = texture.mipOffsets[level];
      const VkDeviceSize size = texture.mipSizes[level];
      if(size == 0 || offset + size > texture.data.size())
      {
        continue;
      }
      const std::span<const std::byte> payload{
          reinterpret_cast<const std::byte*>(texture.data.data() + static_cast<size_t>(offset)),
          static_cast<size_t>(size)};
      const BatchUploadContext::Slice slice = upload.allocate(size, 4);
      upload.copyToSlices(std::span<const BatchUploadContext::Slice>(&slice, 1),
                          std::span<const std::span<const std::byte>>(&payload, 1));
      const VkBufferImageCopy region{
          .bufferOffset = 0,
          .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0, .layerCount = 1},
          .imageExtent = {std::max(1u, texture.width >> level), std::max(1u, texture.height >> level), 1},
      };
      upload.recordTextureUpload(slice, image.image, region);
    }
    upload.executeUploads(cmd);
    utils::Buffer staging = upload.releaseStagingBuffer();
    if(staging.buffer != VK_NULL_HANDLE)
    {
      m_gpuDrivenStagingBuffers.push_back(staging);
    }
  });

  const VkImageViewCreateInfo viewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = texture.format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = std::max(1u, texture.mipLevels), .baseArrayLayer = 0, .layerCount = 1},
  };
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(nativeDevice, &viewInfo, nullptr, &view));
  m_iblEnvironment = {};
  m_iblEnvironment.image = image.image;
  m_iblEnvironment.allocation = image.allocation;
  m_iblEnvironment.view = view;
  m_iblEnvironment.layout = VK_IMAGE_LAYOUT_GENERAL;
  m_iblEnvironmentFormat = texture.format;
  m_iblEnvironmentExtent = {texture.width, texture.height};
  m_iblEnvironmentMipCount = std::max(1u, texture.mipLevels);
  m_iblEnvironmentEstimatedBytes = static_cast<uint64_t>(texture.data.size());
  m_iblEnvironmentLoaded = true;
  m_iblUsingFallback = false;
  IBLResources::CreateInfo iblCreateInfo{
      .cubeMapSize = 128,
      .dfgLUTSize = 256,
      .sourceEnvironmentView = view,
      .sourceWidth = texture.width,
      .sourceHeight = texture.height,
      .sourceMipCount = std::max(1u, texture.mipLevels),
  };
  executeUploadCommand([&](VkCommandBuffer cmd) { m_iblResources.init(nativeDevice, getAllocatorHandle(), cmd, iblCreateInfo); });

  m_iblEnvironmentStatus = m_iblResources.isSplitSumReady()
                                ? "Loaded GPUDriven equirect HDR KTX2 with split-sum IBL"
                                : "Loaded GPUDriven equirect HDR KTX2; split-sum IBL unavailable";
  LOGI("Loaded GPUDriven equirect IBL %s (%ux%u, %u mips, %.2f MiB); split-sum %s",
       m_iblEnvironmentPath.c_str(),
       m_iblEnvironmentExtent.width,
       m_iblEnvironmentExtent.height,
       m_iblEnvironmentMipCount,
       static_cast<double>(m_iblEnvironmentEstimatedBytes) / (1024.0 * 1024.0),
       m_iblResources.isSplitSumReady() ? "ready" : "unavailable");
}

void GPUDrivenRenderer::shutdownIBLResources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  m_iblResources.deinit();
  if(nativeDevice != VK_NULL_HANDLE && (m_iblEnvironment.view != VK_NULL_HANDLE || m_iblEnvironment.image != VK_NULL_HANDLE))
  {
    if(m_iblEnvironment.view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(nativeDevice, m_iblEnvironment.view, nullptr);
    }
    if(m_iblEnvironment.image != VK_NULL_HANDLE)
    {
      vmaDestroyImage(getAllocatorHandle(), m_iblEnvironment.image, m_iblEnvironment.allocation);
    }
    m_iblEnvironment = {};
  }
  m_iblEnvironmentFormat = VK_FORMAT_UNDEFINED;
  m_iblEnvironmentExtent = {};
  m_iblEnvironmentMipCount = 0;
  m_iblEnvironmentEstimatedBytes = 0;
  m_iblEnvironmentLoaded = false;
  m_iblUsingFallback = true;
  for(utils::Buffer& buffer : m_gpuDrivenStagingBuffers)
  {
    if(buffer.buffer != VK_NULL_HANDLE)
    {
      vmaDestroyBuffer(getAllocatorHandle(), buffer.buffer, buffer.allocation);
      buffer = {};
    }
  }
  m_gpuDrivenStagingBuffers.clear();
}

void GPUDrivenRenderer::initPhase7Resources()
{
  shutdownPhase7Resources();

  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE)
  {
    return;
  }
  const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
  const std::array<VkDescriptorPoolSize, 3> poolSizes{{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, frameCount * 8u},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frameCount * 4u},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount},
  }};
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = frameCount * 3u,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
  };
  VK_CHECK(vkCreateDescriptorPool(nativeDevice, &poolInfo, nullptr, &m_phase7DescriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 3> aoBindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo aoLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(aoBindings.size()),
      .pBindings = aoBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &aoLayoutInfo, nullptr, &m_aoSetLayout));

  const std::array<VkDescriptorSetLayoutBinding, 6> ssrBindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const VkDescriptorSetLayoutCreateInfo ssrLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(ssrBindings.size()),
      .pBindings = ssrBindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &ssrLayoutInfo, nullptr, &m_ssrSetLayout));

  const VkPushConstantRange aoPushRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(shaderio::GPUDrivenAOPushConstants),
  };
  const VkPipelineLayoutCreateInfo aoPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_aoSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &aoPushRange,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &aoPipelineLayoutInfo, nullptr, &m_aoPipelineLayout));

  const VkPushConstantRange ssrPushRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(shaderio::GPUDrivenSSRPushConstants),
  };
  const VkPipelineLayoutCreateInfo ssrPipelineLayoutInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &m_ssrSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &ssrPushRange,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &ssrPipelineLayoutInfo, nullptr, &m_ssrPipelineLayout));

  m_aoDescriptorSets.resize(frameCount);
  m_aoDenoiseDescriptorSets.resize(frameCount);
  m_ssrDescriptorSets.resize(frameCount);
  std::vector<VkDescriptorSetLayout> aoLayouts(frameCount * 2u, m_aoSetLayout);
  std::vector<VkDescriptorSet> aoSets(frameCount * 2u, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo aoAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_phase7DescriptorPool,
      .descriptorSetCount = static_cast<uint32_t>(aoSets.size()),
      .pSetLayouts = aoLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &aoAlloc, aoSets.data()));
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    m_aoDescriptorSets[i] = aoSets[i * 2u + 0u];
    m_aoDenoiseDescriptorSets[i] = aoSets[i * 2u + 1u];
  }

  std::vector<VkDescriptorSetLayout> ssrLayouts(frameCount, m_ssrSetLayout);
  VkDescriptorSetAllocateInfo ssrAlloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = m_phase7DescriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts = ssrLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &ssrAlloc, m_ssrDescriptorSets.data()));
  resizePhase7Resources();
}

void GPUDrivenRenderer::shutdownPhase7Resources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  const VmaAllocator allocator = getAllocatorHandle();
  const auto destroyImage = [&](utils::ImageResource& image) {
    if(nativeDevice != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(nativeDevice, image.view, nullptr);
    }
    if(allocator != nullptr && image.image != VK_NULL_HANDLE)
    {
      vmaDestroyImage(allocator, image.image, image.allocation);
    }
    image = {};
  };
  destroyImage(m_aoRaw);
  destroyImage(m_aoDenoised);
  destroyImage(m_ssrRaw);
  destroyImage(m_shadowAtlas);

  if(nativeDevice != VK_NULL_HANDLE)
  {
    if(m_aoPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_aoPipelineLayout, nullptr);
      m_aoPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_ssrPipelineLayout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(nativeDevice, m_ssrPipelineLayout, nullptr);
      m_ssrPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_aoSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_aoSetLayout, nullptr);
      m_aoSetLayout = VK_NULL_HANDLE;
    }
    if(m_ssrSetLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(nativeDevice, m_ssrSetLayout, nullptr);
      m_ssrSetLayout = VK_NULL_HANDLE;
    }
    if(m_phase7DescriptorPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(nativeDevice, m_phase7DescriptorPool, nullptr);
      m_phase7DescriptorPool = VK_NULL_HANDLE;
    }
  }
  m_aoDescriptorSets.clear();
  m_aoDenoiseDescriptorSets.clear();
  m_ssrDescriptorSets.clear();
  m_phase7HalfExtent = {};
  m_shadowAtlasAllocatedTiles = 0u;
}

void GPUDrivenRenderer::resizePhase7Resources()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  const VmaAllocator allocator = getAllocatorHandle();
  if(nativeDevice == VK_NULL_HANDLE || allocator == nullptr)
  {
    return;
  }
  const VkExtent2D sceneExtent = getSceneExtent();
  const VkExtent2D halfExtent{std::max(1u, (sceneExtent.width + 1u) / 2u), std::max(1u, (sceneExtent.height + 1u) / 2u)};
  if(m_phase7HalfExtent.width == halfExtent.width && m_phase7HalfExtent.height == halfExtent.height
     && m_aoRaw.image != VK_NULL_HANDLE && m_aoDenoised.image != VK_NULL_HANDLE && m_ssrRaw.image != VK_NULL_HANDLE
     && m_shadowAtlas.image != VK_NULL_HANDLE)
  {
    return;
  }
  waitForIdle();
  const auto destroyOnlyImage = [&](utils::ImageResource& image) {
    if(image.view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(nativeDevice, image.view, nullptr);
    }
    if(image.image != VK_NULL_HANDLE)
    {
      vmaDestroyImage(allocator, image.image, image.allocation);
    }
    image = {};
  };
  destroyOnlyImage(m_aoRaw);
  destroyOnlyImage(m_aoDenoised);
  destroyOnlyImage(m_ssrRaw);
  destroyOnlyImage(m_shadowAtlas);
  m_phase7HalfExtent = halfExtent;

  const auto createImageResource = [&](VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
    utils::ImageResource resource{};
    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocInfo, &resource.image, &resource.allocation, nullptr));
    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = resource.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(nativeDevice, &viewInfo, nullptr, &resource.view));
    resource.layout = VK_IMAGE_LAYOUT_GENERAL;
    resource.extent = extent;
    executeUploadCommand([&](VkCommandBuffer cmd) { utils::cmdInitImageLayout(cmd, resource.image, aspect); });
    return resource;
  };

  m_aoRaw = createImageResource(kGPUDrivenAOFormat, halfExtent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  m_aoDenoised = createImageResource(kGPUDrivenAOFormat, halfExtent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  m_ssrRaw = createImageResource(kGPUDrivenSSRFormat, halfExtent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  m_shadowAtlas = createImageResource(kGPUDrivenShadowAtlasFormat,
                                      m_shadowAtlasExtent,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT);
}

void GPUDrivenRenderer::bindPhase7PassResources()
{
  if(m_shadowAtlas.image == VK_NULL_HANDLE)
  {
    return;
  }

  m_passExecutor.bindTexture({
      .handle = kPassGPUDrivenShadowAtlasHandle,
      .nativeImage = reinterpret_cast<uint64_t>(m_shadowAtlas.image),
      .aspect = rhi::TextureAspect::depth,
      .initialState = rhi::ResourceState::General,
      .isSwapchain = false,
  });
}

void GPUDrivenRenderer::initPhase7Pipelines()
{
  shutdownPhase7Pipelines();
  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE || m_aoPipelineLayout == VK_NULL_HANDLE || m_ssrPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

#ifdef USE_SLANG
  const auto createComputePipeline = [&](const void* shaderData,
                                         size_t shaderSize,
                                         const char* entryPoint,
                                         VkPipelineLayout layout,
                                         VkPipeline& outPipeline) {
    VkShaderModule shaderModule = utils::createShaderModule(nativeDevice, {static_cast<const uint32_t*>(shaderData), shaderSize});
    const VkPipelineShaderStageCreateInfo stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shaderModule,
        .pName = entryPoint,
    };
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = layout,
    };
    VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline));
    vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
  };

  createComputePipeline(gtao_slang, std::size(gtao_slang), "kernelGTAO", m_aoPipelineLayout, m_gtaoPipeline);
  createComputePipeline(ao_denoise_slang, std::size(ao_denoise_slang), "kernelAODenoise", m_aoPipelineLayout, m_aoDenoisePipeline);
  createComputePipeline(ssr_trace_slang, std::size(ssr_trace_slang), "kernelSSRTrace", m_ssrPipelineLayout, m_ssrTracePipeline);
#endif
}

void GPUDrivenRenderer::shutdownPhase7Pipelines()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE)
  {
    return;
  }
  if(m_gtaoPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gtaoPipeline, nullptr);
    m_gtaoPipeline = VK_NULL_HANDLE;
  }
  if(m_aoDenoisePipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_aoDenoisePipeline, nullptr);
    m_aoDenoisePipeline = VK_NULL_HANDLE;
  }
  if(m_ssrTracePipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_ssrTracePipeline, nullptr);
    m_ssrTracePipeline = VK_NULL_HANDLE;
  }
}

void GPUDrivenRenderer::updatePhase7Descriptors(uint32_t frameIndex)
{
  if(frameIndex >= m_aoDescriptorSets.size() || frameIndex >= m_aoDenoiseDescriptorSets.size()
     || frameIndex >= m_ssrDescriptorSets.size())
  {
    return;
  }
  const GPUDrivenSceneView& sceneView = m_sceneView;
  if(sceneView.sceneDepthView == VK_NULL_HANDLE || sceneView.gbufferViews[0] == VK_NULL_HANDLE
     || sceneView.gbufferViews[1] == VK_NULL_HANDLE
     || sceneView.sceneColorHdrView == VK_NULL_HANDLE || m_aoRaw.view == VK_NULL_HANDLE
     || m_aoDenoised.view == VK_NULL_HANDLE || m_ssrRaw.view == VK_NULL_HANDLE)
  {
    return;
  }

  const std::array<VkDescriptorImageInfo, 3> aoInfos{{
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.sceneDepthView, VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.gbufferViews[1], VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, m_aoRaw.view, VK_IMAGE_LAYOUT_GENERAL},
  }};
  const std::array<VkDescriptorImageInfo, 3> denoiseInfos{{
      VkDescriptorImageInfo{VK_NULL_HANDLE, m_aoRaw.view, VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.sceneDepthView, VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, m_aoDenoised.view, VK_IMAGE_LAYOUT_GENERAL},
  }};
  const std::array<VkDescriptorImageInfo, 5> ssrInfos{{
      VkDescriptorImageInfo{VK_NULL_HANDLE,
                            sceneView.sceneColorHistoryReadView != VK_NULL_HANDLE
                                ? sceneView.sceneColorHistoryReadView
                                : sceneView.sceneColorHdrView,
                            VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.sceneDepthView, VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.gbufferViews[1], VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, sceneView.gbufferViews[0], VK_IMAGE_LAYOUT_GENERAL},
      VkDescriptorImageInfo{VK_NULL_HANDLE, m_ssrRaw.view, VK_IMAGE_LAYOUT_GENERAL},
  }};

  std::array<VkWriteDescriptorSet, 11> writes{};
  uint32_t writeIndex = 0;
  const auto addWrite = [&](VkDescriptorSet set,
                            uint32_t binding,
                            VkDescriptorType type,
                            const VkDescriptorImageInfo* info) {
    writes[writeIndex++] = VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = info,
    };
  };
  addWrite(m_aoDescriptorSets[frameIndex], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &aoInfos[0]);
  addWrite(m_aoDescriptorSets[frameIndex], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &aoInfos[1]);
  addWrite(m_aoDescriptorSets[frameIndex], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aoInfos[2]);
  addWrite(m_aoDenoiseDescriptorSets[frameIndex], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &denoiseInfos[0]);
  addWrite(m_aoDenoiseDescriptorSets[frameIndex], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &denoiseInfos[1]);
  addWrite(m_aoDenoiseDescriptorSets[frameIndex], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &denoiseInfos[2]);
  addWrite(m_ssrDescriptorSets[frameIndex], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrInfos[0]);
  addWrite(m_ssrDescriptorSets[frameIndex], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrInfos[1]);
  addWrite(m_ssrDescriptorSets[frameIndex], 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrInfos[2]);
  addWrite(m_ssrDescriptorSets[frameIndex], 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrInfos[3]);
  addWrite(m_ssrDescriptorSets[frameIndex], 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ssrInfos[4]);

  vkUpdateDescriptorSets(getNativeDeviceHandle(), writeIndex, writes.data(), 0, nullptr);
}

void GPUDrivenRenderer::updateGPUDrivenLights(const RenderParams& params, uint32_t frameIndex)
{
  m_gpuDrivenPointLights.clear();
  m_gpuDrivenSpotLights.clear();
  if(params.debugOptions.enablePointLights)
  {
    m_gpuDrivenPointLights.reserve(std::min<size_t>(params.sceneLights.size(), m_lightResources.getMaxPointLights()));
    m_gpuDrivenSpotLights.reserve(std::min<size_t>(params.sceneLights.size(), m_lightResources.getMaxSpotLights()));
    for(const SceneLight& sceneLight : params.sceneLights)
    {
      if(!sceneLight.enabled)
      {
        continue;
      }
      if(sceneLight.type != SceneLightType::point && sceneLight.type != SceneLightType::spot)
      {
        continue;
      }

      const glm::mat4 worldTransform =
          resolveSceneLightTransform(sceneLight, params.sceneLightSceneNodes, params.sceneLightGltfNodes);
      shaderio::LightData light{};
      light.positionOrDirection = glm::vec3(worldTransform[3]);
      light.intensity = sceneLight.intensity;
      light.color = sceneLight.color;
      light.range = sceneLightEffectiveRange(sceneLight, m_sceneView);
      light.lightType = sceneLight.type == SceneLightType::spot ? shaderio::LLightTypeSpot : shaderio::LLightTypePoint;
      light.spotDirection = sceneLightTravelDirection(worldTransform);
      light.spotInnerAngle = sceneLight.innerConeAngle;
      light.spotOuterAngle = std::max(sceneLight.outerConeAngle, sceneLight.innerConeAngle + 0.001f);

      if(sceneLight.type == SceneLightType::spot)
      {
        if(m_gpuDrivenSpotLights.size() < m_lightResources.getMaxSpotLights())
        {
          m_gpuDrivenSpotLights.push_back(light);
        }
      }
      else if(m_gpuDrivenPointLights.size() < m_lightResources.getMaxPointLights())
      {
        m_gpuDrivenPointLights.push_back(light);
      }
    }
  }

  const shaderio::CameraUniforms* camera = params.cameraUniforms;
  const VkExtent2D extent = getSceneExtent();
  const glm::mat4 view = camera != nullptr ? camera->view : glm::mat4(1.0f);
  const glm::mat4 projection = camera != nullptr ? camera->projection : glm::mat4(1.0f);
  const glm::mat4 inverseView = glm::inverse(view);
  const uint32_t tileCountX = (extent.width + shaderio::LTileSizeX - 1u) / shaderio::LTileSizeX;
  const uint32_t tileCountY = (extent.height + shaderio::LTileSizeY - 1u) / shaderio::LTileSizeY;
  shaderio::LightingUniforms lightingUniforms{};
  const shaderio::ShadowUniforms* shadowData = getCSMShadowResources().getShadowUniformsData();
  if(shadowData != nullptr)
  {
    for(int i = 0; i < shaderio::LCascadeCount; ++i)
    {
      lightingUniforms.light.worldToShadow[i] = shadowData->cascadeViewProjection[i];
    }
    lightingUniforms.light.cascadeSplitDistances = shadowData->cascadeSplitDistances;
  }
  lightingUniforms.light.lightDirectionAndShadowStrength =
      glm::vec4(glm::normalize(-params.lightSettings.direction), params.lightSettings.shadowStrength);
  lightingUniforms.light.lightColorAndNormalBias =
      glm::vec4(params.lightSettings.color, params.lightSettings.normalBias);
  lightingUniforms.light.ambientColorAndTexelSize =
      glm::vec4(params.lightSettings.ambient,
                1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())));
  lightingUniforms.light.shadowMetrics =
      glm::vec4(1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())),
                params.lightSettings.depthBias,
                params.lightSettings.normalBias,
                static_cast<float>(shaderio::LCascadeCount));
  lightingUniforms.light.iblParams =
      glm::vec4(params.debugOptions.enableIBL ? 1.0f : 0.0f,
                params.debugOptions.iblIntensity,
                static_cast<float>(m_iblResources.isSplitSumReady()
                                       ? m_iblResources.getMaxMipLevel()
                                       : (getIBLEnvironmentMipCount() > 0 ? getIBLEnvironmentMipCount() - 1u : 0u)),
                getIBLEnvironmentLoaded() ? 1.0f : 0.0f);
  lightingUniforms.light.iblDebugInfo =
      glm::vec4(static_cast<float>(params.debugOptions.iblDebugMode), m_iblResources.isSplitSumReady() ? 1.0f : 0.0f, 0.0f, 0.0f);
  lightingUniforms.light.phase7Info =
      glm::vec4(params.debugOptions.enableAO && m_aoDenoised.view != VK_NULL_HANDLE ? 1.0f : 0.0f,
                params.debugOptions.enableSSR && m_ssrRaw.view != VK_NULL_HANDLE ? 1.0f : 0.0f,
                0.0f,
                0.0f);

  const shaderio::LightCoarseCullingUniforms coarseUniforms{
      .viewProjection = camera != nullptr ? camera->viewProjection : glm::mat4(1.0f),
      .cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f),
      .cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f),
      .screenTileInfo = glm::vec4(extent.width, extent.height, tileCountX, tileCountY),
      .lightCountInfo = glm::vec4(static_cast<float>(m_gpuDrivenPointLights.size()), static_cast<float>(m_gpuDrivenSpotLights.size()), 0.0f, 0.0f),
      .debugInfo = glm::vec4(params.debugOptions.showLightCoarseCullingHeatmap ? 1.0f : 0.0f,
                             params.debugOptions.showClusteredLightingHeatmap ? 1.0f : 0.0f,
                             params.debugOptions.showClusteredLightingOverflow ? 1.0f : 0.0f,
                             params.debugOptions.enableClusteredLighting ? 1.0f : 0.0f),
  };
  const shaderio::ClusteredLightUniforms clusteredUniforms{
      .screenSizeAndClusterInfo = glm::vec4(extent.width, extent.height, shaderio::LClusterGridSizeX, shaderio::LClusterGridSizeY),
      .clusterZAndLightInfo = glm::vec4(shaderio::LClusterGridSizeZ, shaderio::LMaxLightsPerCluster, static_cast<float>(m_gpuDrivenPointLights.size()), static_cast<float>(m_gpuDrivenSpotLights.size())),
      .clipInfo = glm::vec4(0.01f, 1000.0f, 1.0f, 0.0f),
      .debugInfo = glm::vec4(params.debugOptions.enableClusteredLighting ? 1.0f : 0.0f,
                             params.debugOptions.showClusteredLightingHeatmap ? 1.0f : 0.0f,
                             params.debugOptions.showClusteredLightingOverflow ? 1.0f : 0.0f,
                             0.0f),
      .viewMatrix = view,
      .projectionMatrix = projection,
      .invProjectionMatrix = glm::inverse(projection),
      .invViewMatrix = inverseView,
  };
  m_lightResources.updateLights(frameIndex, m_gpuDrivenPointLights, m_gpuDrivenSpotLights);
  m_lightResources.updateUniforms(frameIndex, lightingUniforms, coarseUniforms, clusteredUniforms);
  updateLightingDescriptorSet(frameIndex);
}

uint64_t GPUDrivenRenderer::getLightingInputDescriptorSet() const
{
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  return frameIndex < m_lightingDescriptorSets.size()
             ? reinterpret_cast<uint64_t>(m_lightingDescriptorSets[frameIndex])
             : 0;
}

uint64_t GPUDrivenRenderer::resolvePipeline(PipelineHandle handle, rhi::PipelineBindPoint bindPoint) const
{
  return bindPoint == rhi::PipelineBindPoint::compute ? getNativeComputePipeline(handle)
                                                      : getNativeGraphicsPipeline(handle);
}

uint64_t GPUDrivenRenderer::resolvePipelineLayout(PipelineHandle handle) const
{
  if(isGpuDrivenFullscreenPipeline(handle))
  {
    return reinterpret_cast<uint64_t>(m_lightPipelineLayout);
  }
  return m_renderer.resolvePipelineLayout(handle);
}

uint64_t GPUDrivenRenderer::resolveBindGroupDescriptorSet(BindGroupHandle handle) const
{
  return m_renderer.resolveBindGroupDescriptorSet(handle);
}

uint64_t GPUDrivenRenderer::getLightingSceneDescriptorSet(uint32_t frameIndex) const
{
  return frameIndex < m_lightingSceneDescriptorSets.size()
             ? reinterpret_cast<uint64_t>(m_lightingSceneDescriptorSets[frameIndex])
             : 0;
}

uint64_t GPUDrivenRenderer::getCurrentLightCullingDescriptorSet() const
{
  const uint32_t frameIndex = getCurrentFrameIndexHint();
  return frameIndex < m_lightCoarseCullingDescriptorSets.size()
             ? reinterpret_cast<uint64_t>(m_lightCoarseCullingDescriptorSets[frameIndex])
             : 0;
}

void GPUDrivenRenderer::updateLightingSceneDescriptorSet(uint32_t frameIndex, uint64_t transientBufferOpaque, uint32_t)
{
  const VkBuffer transientBuffer = reinterpret_cast<VkBuffer>(transientBufferOpaque);
  if(frameIndex >= m_lightingSceneDescriptorSets.size() || transientBuffer == VK_NULL_HANDLE)
  {
    return;
  }
  if(m_lightingSceneDescriptorTransientBuffers.size() < m_lightingSceneDescriptorSets.size())
  {
    m_lightingSceneDescriptorTransientBuffers.resize(m_lightingSceneDescriptorSets.size(), VK_NULL_HANDLE);
  }
  if(m_lightingSceneDescriptorTransientBuffers[frameIndex] == transientBuffer)
  {
    return;
  }

  const VkDescriptorBufferInfo cameraBufferInfo{
      .buffer = transientBuffer,
      .offset = 0,
      .range = sizeof(shaderio::CameraUniforms),
  };
  const VkDescriptorBufferInfo postProcessBufferInfo{
      .buffer = transientBuffer,
      .offset = 0,
      .range = sizeof(shaderio::PostProcessUniforms),
  };
  const VkDescriptorBufferInfo lightingBufferInfo{
      .buffer = m_lightResources.getLightingUniformBuffer(frameIndex),
      .offset = 0,
      .range = sizeof(shaderio::LightingUniforms),
  };
  const VkDescriptorBufferInfo lightCullingBufferInfo{
      .buffer = m_lightResources.getCoarseUniformBuffer(frameIndex),
      .offset = 0,
      .range = sizeof(shaderio::LightCoarseCullingUniforms),
  };
  const std::array<VkWriteDescriptorSet, 4> writes{{
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = m_lightingSceneDescriptorSets[frameIndex],
          .dstBinding = shaderio::LBindCamera,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &cameraBufferInfo,
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = m_lightingSceneDescriptorSets[frameIndex],
          .dstBinding = shaderio::LBindLighting,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &lightingBufferInfo,
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = m_lightingSceneDescriptorSets[frameIndex],
          .dstBinding = shaderio::LBindLightCulling,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &lightCullingBufferInfo,
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = m_lightingSceneDescriptorSets[frameIndex],
          .dstBinding = shaderio::LBindPostProcess,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &postProcessBufferInfo,
      },
  }};
  vkUpdateDescriptorSets(getNativeDeviceHandle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  m_lightingSceneDescriptorTransientBuffers[frameIndex] = transientBuffer;
}

void GPUDrivenRenderer::updateLightingDescriptorSet(uint32_t frameIndex)
{
  if(frameIndex >= m_lightingDescriptorSets.size())
  {
    return;
  }

  const GPUDrivenSceneView& sceneView = m_sceneView;
  if(sceneView.sceneDepthView == VK_NULL_HANDLE || sceneView.sceneColorHdrView == VK_NULL_HANDLE)
  {
    return;
  }
  for(uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
  {
    if(sceneView.gbufferViews[i] == VK_NULL_HANDLE)
    {
      return;
    }
  }

  const VkSampler sampler = m_linearClampSampler;
  if(sampler == VK_NULL_HANDLE)
  {
    return;
  }
  std::array<VkDescriptorImageInfo, kGPUDrivenLightPassTextureCount> imageInfos{};
  for(uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
  {
    imageInfos[i] = VkDescriptorImageInfo{sampler, sceneView.gbufferViews[i], VK_IMAGE_LAYOUT_GENERAL};
  }
  imageInfos[kGPUDrivenLightPassDepthTextureIndex] = VkDescriptorImageInfo{sampler, sceneView.sceneDepthView, VK_IMAGE_LAYOUT_GENERAL};
  const VkImageView safeColorFallback = sceneView.gbufferViews[0];
  const VkImageView safeDepthFallback = sceneView.sceneDepthView;
  imageInfos[kGPUDrivenLightPassSceneColorHdrIndex] = VkDescriptorImageInfo{sampler, sceneView.sceneColorHdrView, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomHalfIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomHalfView != VK_NULL_HANDLE ? sceneView.bloomHalfView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomQuarterIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomQuarterView != VK_NULL_HANDLE ? sceneView.bloomQuarterView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomEighthIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomEighthView != VK_NULL_HANDLE ? sceneView.bloomEighthView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomSixteenthIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomSixteenthView != VK_NULL_HANDLE ? sceneView.bloomSixteenthView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomThirtySecondIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomThirtySecondView != VK_NULL_HANDLE ? sceneView.bloomThirtySecondView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomUpsampleSixteenthIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomUpsampleSixteenthView != VK_NULL_HANDLE ? sceneView.bloomUpsampleSixteenthView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomUpsampleEighthIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomUpsampleEighthView != VK_NULL_HANDLE ? sceneView.bloomUpsampleEighthView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomUpsampleQuarterIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomUpsampleQuarterView != VK_NULL_HANDLE ? sceneView.bloomUpsampleQuarterView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassBloomOutputIndex] = VkDescriptorImageInfo{sampler, sceneView.bloomOutputView != VK_NULL_HANDLE ? sceneView.bloomOutputView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassColorGradingLutIndex] = VkDescriptorImageInfo{sampler, sceneView.colorGradingLutView != VK_NULL_HANDLE ? sceneView.colorGradingLutView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassVelocityIndex] = VkDescriptorImageInfo{sampler, sceneView.velocityView != VK_NULL_HANDLE ? sceneView.velocityView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassHistoryReadIndex] = VkDescriptorImageInfo{sampler, sceneView.sceneColorHistoryReadView != VK_NULL_HANDLE ? sceneView.sceneColorHistoryReadView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassHistoryWriteIndex] = VkDescriptorImageInfo{sampler, sceneView.sceneColorHistoryWriteView != VK_NULL_HANDLE ? sceneView.sceneColorHistoryWriteView : safeColorFallback, VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassIBLEnvironmentIndex] =
      VkDescriptorImageInfo{sampler,
                            m_iblEnvironment.view != VK_NULL_HANDLE ? m_iblEnvironment.view : safeColorFallback,
                            VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassAOIndex] =
      VkDescriptorImageInfo{sampler,
                            m_aoDenoised.view != VK_NULL_HANDLE ? m_aoDenoised.view : safeDepthFallback,
                            VK_IMAGE_LAYOUT_GENERAL};
  imageInfos[kGPUDrivenLightPassSSRIndex] =
      VkDescriptorImageInfo{sampler,
                            m_ssrRaw.view != VK_NULL_HANDLE ? m_ssrRaw.view : safeColorFallback,
                            VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo shadowInfo{
      .sampler = sampler,
      .imageView = getCSMShadowResources().getCascadeView() != VK_NULL_HANDLE ? getCSMShadowResources().getCascadeView() : sceneView.sceneDepthView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo iblIrradianceInfo{
      .sampler = m_iblResources.getCubeMapSampler(),
      .imageView = m_iblResources.getIrradianceMapView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo iblPrefilteredInfo{
      .sampler = m_iblResources.getCubeMapSampler(),
      .imageView = m_iblResources.getPrefilteredMapView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo iblBrdfLutInfo{
      .sampler = m_iblResources.getLUTSampler(),
      .imageView = m_iblResources.getDFGLUTView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const std::array<VkDescriptorBufferInfo, 7> bufferInfos{{
      VkDescriptorBufferInfo{m_lightResources.getPointLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_lightResources.getPointCoarseBoundsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_lightResources.getCoarseUniformBuffer(frameIndex), 0, sizeof(shaderio::LightCoarseCullingUniforms)},
      VkDescriptorBufferInfo{m_lightResources.getClusterCountsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_lightResources.getClusterIndicesBuffer(frameIndex), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_lightResources.getClusteredUniformBuffer(frameIndex), 0, sizeof(shaderio::ClusteredLightUniforms)},
      VkDescriptorBufferInfo{m_lightResources.getSpotLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
  }};
  const std::array<VkWriteDescriptorSet, 12> writes{{
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = shaderio::LBindTextures, .descriptorCount = kGPUDrivenLightPassTextureCount, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = imageInfos.data()},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = shaderio::LBindShadowMap, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &shadowInfo},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfos[0]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfos[1]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bufferInfos[2]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfos[3]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 6, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfos[4]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 7, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bufferInfos[5]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = 8, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfos[6]},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = shaderio::LBindIBLIrradiance, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &iblIrradianceInfo},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = shaderio::LBindIBLPrefiltered, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &iblPrefilteredInfo},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_lightingDescriptorSets[frameIndex], .dstBinding = shaderio::LBindIBLBrdfLut, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &iblBrdfLutInfo},
  }};
  vkUpdateDescriptorSets(getNativeDeviceHandle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

  if(frameIndex < m_lightCoarseCullingDescriptorSets.size())
  {
    const std::array<VkDescriptorBufferInfo, 9> cullBuffers{{
        VkDescriptorBufferInfo{m_lightResources.getPointLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getSpotLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getPointCoarseBoundsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getSpotCoarseBoundsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getCoarseUniformBuffer(frameIndex), 0, sizeof(shaderio::LightCoarseCullingUniforms)},
        VkDescriptorBufferInfo{m_lightResources.getClusteredUniformBuffer(frameIndex), 0, sizeof(shaderio::ClusteredLightUniforms)},
        VkDescriptorBufferInfo{m_lightResources.getClusterCountsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getClusterIndicesBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getClusterStatsBuffer(frameIndex), 0, sizeof(GPUDrivenLightResources::ClusterStats)},
    }};
    std::array<VkWriteDescriptorSet, 9> cullWrites{};
    for(uint32_t i = 0; i < static_cast<uint32_t>(cullWrites.size()); ++i)
    {
      cullWrites[i] = VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = m_lightCoarseCullingDescriptorSets[frameIndex],
          .dstBinding = i,
          .descriptorCount = 1,
          .descriptorType = (i == 4 || i == 5) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &cullBuffers[i],
      };
    }
    vkUpdateDescriptorSets(getNativeDeviceHandle(), static_cast<uint32_t>(cullWrites.size()), cullWrites.data(), 0, nullptr);
  }
}



void GPUDrivenRenderer::initLightingPipelines()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE || m_lightPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

#ifdef USE_SLANG
  VkShaderModule lightShaderModule =
      utils::createShaderModule(nativeDevice, {shader_light_gpu_driven_slang, std::size(shader_light_gpu_driven_slang)});
  const auto createFullscreenPipeline = [&](const char* fragmentEntry,
                                            VkFormat colorFormat,
                                            bool depthTest,
                                            uint32_t variant) -> PipelineHandle {
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                        .module = lightShaderModule,
                                        .pName = "vertexMain"},
        VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                        .module = lightShaderModule,
                                        .pName = fragmentEntry},
    }};
    const VkPipelineVertexInputStateCreateInfo vertexInput{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    const VkPipelineDynamicStateCreateInfo dynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_EQUAL,
    };
    const VkPipelineViewportStateCreateInfo viewportState{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    const VkPipelineRenderingCreateInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = depthTest ? getSceneDepthFormat() : VK_FORMAT_UNDEFINED,
    };
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = m_lightPipelineLayout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    if(variant == 0x6101u)
    {
      m_gpuDrivenLightHdrVkPipeline = pipeline;
    }
    else if(variant == 0x6102u)
    {
      m_gpuDrivenSkyboxVkPipeline = pipeline;
    }
    else if(variant == 0x6103u)
    {
      m_gpuDrivenTAAResolveVkPipeline = pipeline;
    }
    else if(variant == 0x6104u)
    {
      m_gpuDrivenBloomPrefilterVkPipeline = pipeline;
    }
    else if(variant == 0x6105u)
    {
      m_gpuDrivenBloomDownsampleVkPipeline = pipeline;
    }
    else if(variant == 0x6106u)
    {
      m_gpuDrivenFinalColorVkPipeline = pipeline;
    }
    else if(variant == 0x6107u)
    {
      m_gpuDrivenVelocityVkPipeline = pipeline;
    }
    else if(variant == 0x6108u)
    {
      m_gpuDrivenBloomUpsampleVkPipeline = pipeline;
    }
    return PipelineHandle{variant, 1u};
  };

  m_gpuDrivenLightHdrPipeline = createFullscreenPipeline("fragmentHdrMain", getSceneColorHdrFormat(), false, 0x6101u);
  m_gpuDrivenSkyboxPipeline = createFullscreenPipeline("fragmentSkyboxMain", getSceneColorHdrFormat(), true, 0x6102u);
  m_gpuDrivenTAAResolvePipeline = createFullscreenPipeline("fragmentTAAResolveMain", getSceneColorHdrFormat(), false, 0x6103u);
  m_gpuDrivenBloomPrefilterPipeline = createFullscreenPipeline("fragmentBloomPrefilterMain", SceneResources::kBloomFormat, false, 0x6104u);
  m_gpuDrivenBloomDownsamplePipeline = createFullscreenPipeline("fragmentBloomDownsampleMain", SceneResources::kBloomFormat, false, 0x6105u);
  m_gpuDrivenFinalColorPipeline = createFullscreenPipeline("fragmentFinalColorMain", getOutputTextureFormat(), false, 0x6106u);
  m_gpuDrivenVelocityPipeline = createFullscreenPipeline("fragmentVelocityMain", getVelocityFormat(), false, 0x6107u);
  m_gpuDrivenBloomUpsamplePipeline = createFullscreenPipeline("fragmentBloomUpsampleMain", SceneResources::kBloomFormat, false, 0x6108u);

  if(m_lightCoarseCullingPipelineLayout != VK_NULL_HANDLE)
  {
    VkShaderModule cullingShaderModule =
        utils::createShaderModule(nativeDevice, {shader_light_culling_slang, std::size(shader_light_culling_slang)});
    const auto createComputePipeline = [&](const char* entryPoint, uint32_t variant, VkPipeline& outPipeline) {
      const VkPipelineShaderStageCreateInfo stage{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
          .module = cullingShaderModule,
          .pName = entryPoint,
      };
      const VkComputePipelineCreateInfo pipelineInfo{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = stage,
          .layout = m_lightCoarseCullingPipelineLayout,
      };
      VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline));
      return PipelineHandle{variant, 1u};
    };
    m_pointLightCoarseCullingPipeline =
        createComputePipeline("kernelPointLightCoarseCulling", 0x6201u, m_pointLightCoarseCullingVkPipeline);
    m_spotLightCoarseCullingPipeline =
        createComputePipeline("kernelSpotLightCoarseCulling", 0x6202u, m_spotLightCoarseCullingVkPipeline);
    vkDestroyShaderModule(nativeDevice, cullingShaderModule, nullptr);

    VkShaderModule clusteredShaderModule =
        utils::createShaderModule(nativeDevice, {clustered_light_cull_slang, std::size(clustered_light_cull_slang)});
    const VkPipelineShaderStageCreateInfo clusteredStage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = clusteredShaderModule,
        .pName = "kernelClusteredLightCulling",
    };
    const VkComputePipelineCreateInfo clusteredPipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = clusteredStage,
        .layout = m_lightCoarseCullingPipelineLayout,
    };
    VK_CHECK(vkCreateComputePipelines(nativeDevice,
                                      VK_NULL_HANDLE,
                                      1,
                                      &clusteredPipelineInfo,
                                      nullptr,
                                      &m_clusteredLightCullingVkPipeline));
    m_clusteredLightCullingPipeline = PipelineHandle{0x6203u, 1u};
    vkDestroyShaderModule(nativeDevice, clusteredShaderModule, nullptr);
  }
  vkDestroyShaderModule(nativeDevice, lightShaderModule, nullptr);
#endif
}

void GPUDrivenRenderer::shutdownLightingPipelines()
{
  const VkDevice nativeDevice = getNativeDeviceHandle();
  if(nativeDevice == VK_NULL_HANDLE)
  {
    return;
  }
  const auto destroyGraphicsPipeline = [&](PipelineHandle& handle) {
    if(!handle.isNull())
    {
      handle = {};
    }
  };
  if(m_gpuDrivenLightHdrVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenLightHdrVkPipeline, nullptr);
    m_gpuDrivenLightHdrVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenSkyboxVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenSkyboxVkPipeline, nullptr);
    m_gpuDrivenSkyboxVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenTAAResolveVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenTAAResolveVkPipeline, nullptr);
    m_gpuDrivenTAAResolveVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenBloomPrefilterVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenBloomPrefilterVkPipeline, nullptr);
    m_gpuDrivenBloomPrefilterVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenBloomDownsampleVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenBloomDownsampleVkPipeline, nullptr);
    m_gpuDrivenBloomDownsampleVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenBloomUpsampleVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenBloomUpsampleVkPipeline, nullptr);
    m_gpuDrivenBloomUpsampleVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenFinalColorVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenFinalColorVkPipeline, nullptr);
    m_gpuDrivenFinalColorVkPipeline = VK_NULL_HANDLE;
  }
  if(m_gpuDrivenVelocityVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_gpuDrivenVelocityVkPipeline, nullptr);
    m_gpuDrivenVelocityVkPipeline = VK_NULL_HANDLE;
  }
  if(m_pointLightCoarseCullingVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_pointLightCoarseCullingVkPipeline, nullptr);
    m_pointLightCoarseCullingVkPipeline = VK_NULL_HANDLE;
  }
  if(m_spotLightCoarseCullingVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_spotLightCoarseCullingVkPipeline, nullptr);
    m_spotLightCoarseCullingVkPipeline = VK_NULL_HANDLE;
  }
  if(m_clusteredLightCullingVkPipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(nativeDevice, m_clusteredLightCullingVkPipeline, nullptr);
    m_clusteredLightCullingVkPipeline = VK_NULL_HANDLE;
  }
  destroyGraphicsPipeline(m_gpuDrivenLightHdrPipeline);
  destroyGraphicsPipeline(m_gpuDrivenSkyboxPipeline);
  destroyGraphicsPipeline(m_gpuDrivenTAAResolvePipeline);
  destroyGraphicsPipeline(m_gpuDrivenBloomPrefilterPipeline);
  destroyGraphicsPipeline(m_gpuDrivenBloomDownsamplePipeline);
  destroyGraphicsPipeline(m_gpuDrivenBloomUpsamplePipeline);
  destroyGraphicsPipeline(m_gpuDrivenFinalColorPipeline);
  destroyGraphicsPipeline(m_gpuDrivenVelocityPipeline);
  destroyGraphicsPipeline(m_pointLightCoarseCullingPipeline);
  destroyGraphicsPipeline(m_spotLightCoarseCullingPipeline);
  destroyGraphicsPipeline(m_clusteredLightCullingPipeline);
}

void GPUDrivenRenderer::updateOwnershipDiagnostics(uint32_t frameIndex,
                                                   bool     sceneRenderingSuspended,
                                                   uint32_t safeObjectCount)
{
  const bool hasSceneAttachments = m_sceneView.sceneDepthImage != VK_NULL_HANDLE
                                   && m_sceneView.sceneDepthView != VK_NULL_HANDLE
                                   && m_sceneView.outputImage != VK_NULL_HANDLE
                                   && m_sceneView.outputView != VK_NULL_HANDLE
                                   && m_sceneView.sceneColorHdrImage != VK_NULL_HANDLE
                                   && m_sceneView.sceneColorHdrView != VK_NULL_HANDLE
                                   && m_sceneView.gbufferImages[0] != VK_NULL_HANDLE
                                   && m_sceneView.gbufferViews[0] != VK_NULL_HANDLE;
  const bool hasLightingResources = !getGPUDrivenLightHdrPipelineHandle().isNull()
                                    && getLightPipelineLayout() != 0
                                    && getLightingInputDescriptorSet() != 0;
  const bool hasShadowResources = !getCSMShadowPipelineHandle().isNull()
                                  && getCSMShadowPipelineLayout() != 0
                                  && getCSMShadowResources().getCascadeImage() != VK_NULL_HANDLE;
  const bool hasMaterialDescriptors = getGraphicsMaterialDescriptorSet() != 0;
  const bool hasCurrentVisibility = m_sceneView.usePersistentCullingObjects
                                    && getGPUCullingObjectCount(frameIndex) == safeObjectCount
                                    && safeObjectCount > 0u;
  m_runtimeStats.visibilityDiagnostics.currentGPUCullingObjectCount = getGPUCullingObjectCount(frameIndex);
  m_runtimeStats.visibilityDiagnostics.safeObjectCount = safeObjectCount;

  m_runtimeStats.resourceOwnership.sceneAttachments =
      hasSceneAttachments ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
  m_runtimeStats.resourceOwnership.depthPyramid =
      m_runtimeStats.ownsHiZVisibilityChain
          ? GPUDrivenOwnershipState::gpuOwned
          : (m_hiZDepthPyramid.isValid() ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled);
  m_runtimeStats.resourceOwnership.visibility =
      hasCurrentVisibility ? GPUDrivenOwnershipState::gpuOwned
                           : (m_sceneView.usePersistentCullingObjects ? GPUDrivenOwnershipState::bridged
                                                                      : GPUDrivenOwnershipState::disabled);
  m_runtimeStats.resourceOwnership.lightingResources =
      hasLightingResources && m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
          ? GPUDrivenOwnershipState::gpuOwned
          : (hasLightingResources ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled);
  m_runtimeStats.resourceOwnership.shadowResources =
      hasShadowResources ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
  m_runtimeStats.resourceOwnership.materialDescriptors =
      hasMaterialDescriptors ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
  const bool transparentVisibilityAuthoritative =
      m_runtimeStats.visibilityDiagnostics.transparentCapacity == 0u
      || m_runtimeStats.visibilityDiagnostics.transparentPatchDispatched;
  if(hasCurrentVisibility && m_runtimeStats.visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched
     && transparentVisibilityAuthoritative)
  {
    m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
  }
  else if(m_visibilitySortPass != nullptr && m_runtimeStats.visibilityDiagnostics.sortInputCount > 0u)
  {
    m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuSortCpuFeedback;
  }
  else
  {
    m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::cpuBootstrap;
  }

  m_runtimeStats.passDiagnostics.clear();
  m_runtimeStats.passDiagnostics.reserve(m_passExecutor.getPassCount());
  const auto addPassDiagnostic = [this](const char* name, GPUDrivenOwnershipState ownership, const char* note) {
    m_runtimeStats.passDiagnostics.push_back(GPUDrivenPassDiagnostic{
        .name = name != nullptr ? name : "",
        .ownership = ownership,
        .note = note != nullptr ? note : "",
    });
  };

  addPassDiagnostic("GPUDrivenDepthPrepass",
                    m_depthPrepass != nullptr ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
                    hasSceneAttachments ? "MDI submission; scene attachments still bridged"
                                        : "Disabled until scene attachments are valid");
  addPassDiagnostic("GPUDrivenDepthPyramid",
                    m_runtimeStats.resourceOwnership.depthPyramid,
                    m_runtimeStats.ownsHiZVisibilityChain ? "GPU-driven Hi-Z is bound to GPU culling"
                                                          : "Hi-Z resource exists but ownership is not complete");
  addPassDiagnostic("GPUDrivenCulling",
                    hasCurrentVisibility ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::bridged,
                    hasCurrentVisibility ? "Current GPU culling object stream is authoritative"
                                         : "Bootstrap or suspended scene path is active");
  addPassDiagnostic("GPUDrivenVisibilitySortPass",
                    m_visibilitySortPass != nullptr ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
                    m_visibilitySortPass != nullptr ? "GPU sort executes; transparent distance keys are still CPU-seeded"
                                                    : "Visibility sort pass is not registered");
  addPassDiagnostic("GPUDrivenLightCulling",
                    m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
                        ? "GPUDriven-owned local light buffers and culling descriptors"
                        : "GPUDriven light resources are unavailable");
  addPassDiagnostic("GPUDrivenCSMShadow",
                    hasShadowResources ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
                    hasShadowResources ? "GPU-driven submission; CSM atlas/resources are still shared"
                                       : "CSM resources are unavailable");
  addPassDiagnostic("GPUDrivenGBuffer",
                    hasSceneAttachments && hasMaterialDescriptors ? GPUDrivenOwnershipState::gpuOwned
                                                                  : GPUDrivenOwnershipState::disabled,
                    hasSceneAttachments && hasMaterialDescriptors
                        ? "MDI submission; attachments/material descriptors are still bridged"
                        : "Missing scene attachments or material descriptors");
  addPassDiagnostic("GPUDrivenLightPass",
                    hasLightingResources ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
                    hasLightingResources ? "Deferred lighting writes FP16 HDR scene color using GPUDriven-owned lighting descriptors"
                                         : "Lighting resources are unavailable");
  addPassDiagnostic("GPUDrivenSkybox",
                    m_skyboxPass != nullptr && m_runtimeStats.iblDiagnostics.enabled
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.iblDiagnostics.enabled
                        ? "Depth-tested HDRI skybox fills reverse-Z background pixels"
                        : "Skybox is disabled with IBL");
  addPassDiagnostic("GPUDrivenForwardPass",
                    hasSceneAttachments ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
                    hasSceneAttachments ? "Transparent visibility is GPU-patched into HDR scene color; ordering seed is still CPU-generated"
                                        : "Disabled until scene attachments are valid");
  addPassDiagnostic("GPUDrivenVelocity",
                    m_velocityPass != nullptr && m_runtimeStats.postProcessDiagnostics.velocityBufferActive
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.postProcessDiagnostics.velocityBufferActive
                        ? "Fullscreen camera velocity writes R16G16 motion vectors for temporal passes"
                        : "Velocity buffer or pipeline is unavailable");
  addPassDiagnostic("GPUDrivenTAAResolve",
                    m_taaResolvePass != nullptr && m_runtimeStats.postProcessDiagnostics.taaPassActive
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.postProcessDiagnostics.taaPassActive
                        ? "Mobile temporal resolve writes the current HDR history target"
                        : "TAA is disabled or pipeline is unavailable");
  addPassDiagnostic("GPUDrivenBloomPrefilter",
                    m_bloomPrefilterPass != nullptr && m_runtimeStats.postProcessDiagnostics.bloomPassActive
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.postProcessDiagnostics.bloomPassActive
                        ? "Half-resolution mobile bloom prefilter is active"
                        : "Bloom is disabled or pipeline is unavailable");
  addPassDiagnostic("GPUDrivenBloomDownsample",
                    m_bloomDownsamplePass != nullptr && m_runtimeStats.postProcessDiagnostics.bloomPassActive
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.postProcessDiagnostics.bloomPassActive
                        ? "Quarter-resolution mobile bloom downsample is active"
                        : "Bloom is disabled or pipeline is unavailable");
  addPassDiagnostic("GPUDrivenFinalColor",
                    m_finalColorPass != nullptr && m_runtimeStats.postProcessDiagnostics.finalColorPassActive
                        ? GPUDrivenOwnershipState::gpuOwned
                        : GPUDrivenOwnershipState::disabled,
                    m_runtimeStats.postProcessDiagnostics.finalColorPassActive
                        ? "Final color pass owns exposure, bloom composite, tone mapping, and SDR output"
                        : "Final color pipeline is unavailable");
  addPassDiagnostic("GPUDrivenDebug",
                    GPUDrivenOwnershipState::disabled,
                    "Pass is registered but debug execution is currently disabled");
  addPassDiagnostic("GPUDrivenPresent",
                    GPUDrivenOwnershipState::gpuOwned,
                    "GPU-driven present path owns the copy/blit step");
  addPassDiagnostic("GPUDrivenImgui",
                    sceneRenderingSuspended ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::bridged,
                    "ImGui rendering is intentionally shared with the app UI backend");
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
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 12u},
  }};
  const VkDescriptorPoolCreateInfo poolInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = frameCount * 2u,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
  };
  VK_CHECK(vkCreateDescriptorPool(nativeDevice, &poolInfo, nullptr, &m_transparentVisibilityPatchDescriptorPool));

  const std::array<VkDescriptorSetLayoutBinding, 6> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
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
  const VmaAllocator allocator = getAllocatorHandle();
  for(TransparentVisibilityFrameResources& frameResources : m_transparentVisibilityPatchFrames)
  {
    destroyBuffer(allocator, frameResources.prefixBuffers[0]);
    destroyBuffer(allocator, frameResources.prefixBuffers[1]);
  }
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
  const uint64_t prefixABufferHandle = reinterpret_cast<uint64_t>(frameResources.prefixBuffers[0].buffer);
  const uint64_t prefixBBufferHandle = reinterpret_cast<uint64_t>(frameResources.prefixBuffers[1].buffer);
  if(nativeDevice == VK_NULL_HANDLE || frameResources.descriptorSets[descriptorSetIndex] == VK_NULL_HANDLE || sortKeyBufferHandle == 0
     || sortValueBufferHandle == 0 || sourceIndirectBufferHandle == 0 || targetIndirectBufferHandle == 0
     || prefixABufferHandle == 0 || prefixBBufferHandle == 0)
  {
    return;
  }

  if(frameResources.boundSortKeyHandles[descriptorSetIndex] == sortKeyBufferHandle
     && frameResources.boundSortValueHandles[descriptorSetIndex] == sortValueBufferHandle
     && frameResources.boundSourceIndirectHandles[descriptorSetIndex] == sourceIndirectBufferHandle
     && frameResources.boundTargetIndirectHandles[descriptorSetIndex] == targetIndirectBufferHandle
     && frameResources.boundPrefixAHandles[descriptorSetIndex] == prefixABufferHandle
     && frameResources.boundPrefixBHandles[descriptorSetIndex] == prefixBBufferHandle)
  {
    return;
  }

  const std::array<VkDescriptorBufferInfo, 6> bufferInfos{{
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sortKeyBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sortValueBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(sourceIndirectBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{reinterpret_cast<VkBuffer>(targetIndirectBufferHandle), 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameResources.prefixBuffers[0].buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameResources.prefixBuffers[1].buffer, 0, VK_WHOLE_SIZE},
  }};
  const std::array<VkWriteDescriptorSet, 6> writes{{
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
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 4,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[4],
      },
      VkWriteDescriptorSet{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frameResources.descriptorSets[descriptorSetIndex],
          .dstBinding = 5,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bufferInfos[5],
      },
  }};
  vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  frameResources.boundSortKeyHandles[descriptorSetIndex] = sortKeyBufferHandle;
  frameResources.boundSortValueHandles[descriptorSetIndex] = sortValueBufferHandle;
  frameResources.boundSourceIndirectHandles[descriptorSetIndex] = sourceIndirectBufferHandle;
  frameResources.boundTargetIndirectHandles[descriptorSetIndex] = targetIndirectBufferHandle;
  frameResources.boundPrefixAHandles[descriptorSetIndex] = prefixABufferHandle;
  frameResources.boundPrefixBHandles[descriptorSetIndex] = prefixBBufferHandle;
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
  if(frameResources.descriptorSets[descriptorSetIndex] == VK_NULL_HANDLE
     || frameResources.prefixBuffers[0].buffer == VK_NULL_HANDLE
     || frameResources.prefixBuffers[1].buffer == VK_NULL_HANDLE
     || frameResources.prefixCapacity < sortResources.paddedElementCount)
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
  const auto barrierComputeToCompute = [&]() {
    vkCmdPipelineBarrier2(vkCmd, &computeToComputeDependency);
  };

  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transparentVisibilityPatchPipeline);
  vkCmdBindDescriptorSets(vkCmd,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_transparentVisibilityPatchPipelineLayout,
                          0,
                          1,
                          &frameResources.descriptorSets[descriptorSetIndex],
                          0,
                          nullptr);

  const uint32_t elementCount = sortResources.paddedElementCount;
  const uint32_t groupCount = (elementCount + 63u) / 64u;

  shaderio::TransparentVisibilityPatchPushConstants pushConstants{
      .elementCount = sortResources.paddedElementCount,
      .categoryMask = kVisibilitySortCategoryMask,
      .categoryValue = categoryValue,
      .outputOffset = outputOffset,
      .mode = 0u,
      .scanOffset = 0u,
      .scanBufferIndex = 0u,
      ._padding0 = 0u,
  };
  vkCmdPushConstants(vkCmd,
                     m_transparentVisibilityPatchPipelineLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT,
                     0,
                     sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(vkCmd, groupCount, 1u, 1u);
  barrierComputeToCompute();

  uint32_t scanBufferIndex = 0u;
  for(uint32_t scanOffset = 1u; scanOffset < elementCount; scanOffset <<= 1u)
  {
    pushConstants.mode = 1u;
    pushConstants.scanOffset = scanOffset;
    pushConstants.scanBufferIndex = scanBufferIndex;
    vkCmdPushConstants(vkCmd,
                       m_transparentVisibilityPatchPipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(pushConstants),
                       &pushConstants);
    vkCmdDispatch(vkCmd, groupCount, 1u, 1u);
    barrierComputeToCompute();
    scanBufferIndex = 1u - scanBufferIndex;
  }

  pushConstants.mode = 2u;
  pushConstants.scanOffset = 0u;
  pushConstants.scanBufferIndex = scanBufferIndex;
  vkCmdPushConstants(vkCmd,
                     m_transparentVisibilityPatchPipelineLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT,
                     0,
                     sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(vkCmd, groupCount, 1u, 1u);

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
  TransparentVisibilityFrameResources* patchFrameResources = frameIndex < m_transparentVisibilityPatchFrames.size()
                                                               ? &m_transparentVisibilityPatchFrames[frameIndex]
                                                               : nullptr;
  const bool growSortBuffers = frameResources.capacity < requiredCount;
  const bool growPatchBuffers = patchFrameResources != nullptr && patchFrameResources->prefixCapacity < requiredCount;
  if(!growSortBuffers && !growPatchBuffers)
  {
    return;
  }

  const VkDevice nativeDevice = getNativeDeviceHandle();
  const VmaAllocator allocator = getAllocatorHandle();
  if((growSortBuffers && frameResources.capacity > 0) || (growPatchBuffers && patchFrameResources->prefixCapacity > 0))
  {
    waitForIdle();
  }

  const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(requiredCount) * sizeof(uint32_t);
  if(growSortBuffers)
  {
    destroyBuffer(allocator, frameResources.uploadKeyBuffer);
    destroyBuffer(allocator, frameResources.uploadValueBuffer);
    destroyBuffer(allocator, frameResources.keyBuffer);
    destroyBuffer(allocator, frameResources.valueBuffer);

    frameResources.uploadKeyBuffer = upload::createMappedUploadStagingBuffer(nativeDevice, allocator, bufferSize);
    frameResources.uploadValueBuffer = upload::createMappedUploadStagingBuffer(nativeDevice, allocator, bufferSize);
    frameResources.keyBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
    frameResources.valueBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
    frameResources.capacity = requiredCount;
    updateVisibilitySortDescriptorSet(frameIndex);
  }

  if(growPatchBuffers)
  {
    destroyBuffer(allocator, patchFrameResources->prefixBuffers[0]);
    destroyBuffer(allocator, patchFrameResources->prefixBuffers[1]);
    patchFrameResources->prefixBuffers[0] = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
    patchFrameResources->prefixBuffers[1] = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
    patchFrameResources->prefixCapacity = requiredCount;
    patchFrameResources->boundPrefixAHandles = {};
    patchFrameResources->boundPrefixBHandles = {};
  }
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

  auto* mappedKeys = static_cast<uint32_t*>(frameResources.uploadKeyBuffer.mapped);
  auto* mappedValues = static_cast<uint32_t*>(frameResources.uploadValueBuffer.mapped);
  const uint32_t activeCount = static_cast<uint32_t>(m_visibilitySortInputObjects.size());
  std::memcpy(mappedKeys, m_visibilitySortInputKeys.data(), activeCount * sizeof(uint32_t));
  std::memcpy(mappedValues, m_visibilitySortInputObjects.data(), activeCount * sizeof(uint32_t));
  if(paddedCount > activeCount)
  {
    std::fill(mappedKeys + activeCount, mappedKeys + paddedCount, 0xffffffffu);
    std::fill(mappedValues + activeCount, mappedValues + paddedCount, 0xffffffffu);
  }
  VK_CHECK(vmaFlushAllocation(getAllocatorHandle(), frameResources.uploadKeyBuffer.allocation, 0, VK_WHOLE_SIZE));
  VK_CHECK(vmaFlushAllocation(getAllocatorHandle(), frameResources.uploadValueBuffer.allocation, 0, VK_WHOLE_SIZE));

  frameResources.activeElementCount = activeCount;
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
