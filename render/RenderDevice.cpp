#include "RenderDevice.h"
#include "../rhi/vulkan/VulkanCommandBuffer.h"
#include "../rhi/vulkan/VulkanSwapchain.h"
#include "../rhi/vulkan/VulkanDevice.h"
#include "../rhi/vulkan/VulkanFrameContext.h"
#include "../rhi/vulkan/VulkanSurface.h"
#include "FrameSubmission.h"
#include "RHIFormatBridge.h"
#include "ClipSpaceConvention.h"
#include "ImguiAxis.h"
#include "BatchUploadContext.h"
#include "CSMShadowResources.h"
#include "MipmapGenerator.h"
#include "../common/ProfilerMarkers.h"
#include "../third_party/LegitProfiler/ImGuiProfilerRenderer.h"
#include "../loader/Ktx2Loader.h"
#include "../scene/SceneAssetView.h"
#include "../scene/SceneUploadPlanner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <type_traits>
#include <unordered_map>

namespace demo {

namespace {

constexpr uint32_t kPerFrameTransientAllocatorSize = 4u << 20;
constexpr uint32_t kLightPassTextureCount         = kPackedGBufferTargetCount + 18u;
constexpr uint32_t kLightPassDepthTextureIndex    = kPackedGBufferTargetCount;
constexpr uint32_t kLightPassSceneColorHdrIndex   = kPackedGBufferTargetCount + 1u;
constexpr uint32_t kLightPassBloomHalfIndex       = kPackedGBufferTargetCount + 2u;
constexpr uint32_t kLightPassBloomQuarterIndex    = kPackedGBufferTargetCount + 3u;
constexpr uint32_t kLightPassVelocityIndex        = kPackedGBufferTargetCount + 4u;
constexpr uint32_t kLightPassHistoryReadIndex     = kPackedGBufferTargetCount + 5u;
constexpr uint32_t kLightPassHistoryWriteIndex    = kPackedGBufferTargetCount + 6u;
constexpr uint32_t kLightPassIBLEnvironmentIndex  = kPackedGBufferTargetCount + 7u;
constexpr uint32_t kLightPassAOIndex              = kPackedGBufferTargetCount + 8u;
constexpr uint32_t kLightPassSSRIndex             = kPackedGBufferTargetCount + 9u;
constexpr uint32_t kLightPassBloomEighthIndex     = kPackedGBufferTargetCount + 10u;
constexpr uint32_t kLightPassBloomSixteenthIndex  = kPackedGBufferTargetCount + 11u;
constexpr uint32_t kLightPassBloomThirtySecondIndex = kPackedGBufferTargetCount + 12u;
constexpr uint32_t kLightPassBloomUpsampleSixteenthIndex = kPackedGBufferTargetCount + 13u;
constexpr uint32_t kLightPassBloomUpsampleEighthIndex = kPackedGBufferTargetCount + 14u;
constexpr uint32_t kLightPassBloomUpsampleQuarterIndex = kPackedGBufferTargetCount + 15u;
constexpr uint32_t kLightPassBloomOutputIndex     = kPackedGBufferTargetCount + 16u;
constexpr uint32_t kLightPassColorGradingLutIndex = kPackedGBufferTargetCount + 17u;
constexpr uint32_t kLightCoarseCullingThreadCount = 64;
constexpr uint32_t kTestPointLightCount           = 128;
constexpr const char* kDefaultIBLEnvironmentPath  = "resources/environment/lilienstein_4k.ktx2";

struct DebugUnitLineSegment
{
  glm::vec3 a{0.0f};
  glm::vec3 b{0.0f};
};

[[nodiscard]] const std::vector<DebugUnitLineSegment>& getCachedDebugSphereSegments(uint32_t segments)
{
  static std::unordered_map<uint32_t, std::vector<DebugUnitLineSegment>> cache;

  const auto it = cache.find(segments);
  if(it != cache.end())
  {
    return it->second;
  }

  auto& unitSegments = cache[segments];
  unitSegments.reserve(static_cast<size_t>(segments) * 3u);

  const float delta = 6.28318530718f / static_cast<float>(segments);
  for(uint32_t i = 0; i < segments; ++i)
  {
    const float angle0 = delta * static_cast<float>(i);
    const float angle1 = delta * static_cast<float>(i + 1);

    const float cos0 = std::cos(angle0);
    const float sin0 = std::sin(angle0);
    const float cos1 = std::cos(angle1);
    const float sin1 = std::sin(angle1);

    unitSegments.push_back(DebugUnitLineSegment{
        .a = glm::vec3(cos0, 0.0f, sin0),
        .b = glm::vec3(cos1, 0.0f, sin1),
    });
    unitSegments.push_back(DebugUnitLineSegment{
        .a = glm::vec3(0.0f, cos0, sin0),
        .b = glm::vec3(0.0f, cos1, sin1),
    });
    unitSegments.push_back(DebugUnitLineSegment{
        .a = glm::vec3(cos0, sin0, 0.0f),
        .b = glm::vec3(cos1, sin1, 0.0f),
    });
  }

  return unitSegments;
}

[[nodiscard]] bool needsCpuDebugLineBuild(const DebugPassOptions& debugOptions)
{
  return debugOptions.showSceneBounds || debugOptions.showShadowFrustum || debugOptions.showViewFrustum
         || debugOptions.showLightDirection || (debugOptions.enablePointLights && debugOptions.showPointLights)
         || debugOptions.showCullDistance;
}

[[nodiscard]] bool hasStencilComponent(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] rhi::TextureAspect sceneDepthTextureAspect(VkFormat format)
{
  return hasStencilComponent(format) ? rhi::TextureAspect::depthStencil : rhi::TextureAspect::depth;
}

[[nodiscard]] uint32_t alignUp(uint32_t value, uint32_t alignment)
{
  const uint32_t safeAlignment = alignment == 0 ? 1u : alignment;
  const uint32_t mask          = safeAlignment - 1u;
  return (value + mask) & ~mask;
}

[[nodiscard]] bool isUploadableGltfImage(const GltfImageData& imageData)
{
  return (!imageData.pixels.empty() && imageData.width > 0 && imageData.height > 0)
         || (imageData.isKtx2 && (!imageData.ktx2Data.empty() || !imageData.uri.empty()));
}

[[nodiscard]] bool supportsSampledImageFormat(VkPhysicalDevice physicalDevice, VkFormat format)
{
  if(physicalDevice == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED)
  {
    return false;
  }

  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
  return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

[[nodiscard]] bool tryLoadKtx2Texture(const GltfModel&         model,
                                      const GltfImageData&     imageData,
                                      Ktx2Loader&              loader,
                                      Ktx2Loader::Ktx2Texture& outTexture,
                                      std::filesystem::path*   outPath = nullptr)
{
  if(imageData.isKtx2 && !imageData.ktx2Data.empty())
  {
    if(outPath != nullptr)
    {
      *outPath = imageData.uri.empty() ? std::filesystem::path("<embedded KTX2>") : std::filesystem::path(imageData.uri);
    }
    return loader.loadFromMemory(imageData.ktx2Data.data(), imageData.ktx2Data.size(), outTexture);
  }

  const std::filesystem::path sourceDirectory(model.sourceDirectory);
  const std::filesystem::path imagePath = imageData.isKtx2 && !imageData.uri.empty()
                                            ? sourceDirectory / std::filesystem::path(imageData.uri)
                                            : Ktx2Loader::buildSidecarPath(sourceDirectory, imageData.uri);
  if(outPath != nullptr)
  {
    *outPath = imagePath;
  }

  return !imagePath.empty() && std::filesystem::exists(imagePath) && loader.load(imagePath, outTexture);
}

[[nodiscard]] const GltfImageData* findRawImageFallback(const GltfModel& model, const GltfImageData& imageData)
{
  if(!imageData.pixels.empty() && imageData.width > 0 && imageData.height > 0)
  {
    return &imageData;
  }

  if(imageData.fallbackImage >= 0 && static_cast<size_t>(imageData.fallbackImage) < model.images.size())
  {
    const GltfImageData& fallback = model.images[static_cast<size_t>(imageData.fallbackImage)];
    if(!fallback.pixels.empty() && fallback.width > 0 && fallback.height > 0)
    {
      return &fallback;
    }
  }

  return nullptr;
}

[[nodiscard]] VkDeviceSize rawImageUploadBytes(const GltfModel& model, const GltfImageData& imageData)
{
  const GltfImageData* rawImage = findRawImageFallback(model, imageData);
  return rawImage != nullptr ? static_cast<VkDeviceSize>(rawImage->pixels.size()) : 0;
}

[[nodiscard]] VkDeviceSize computeSelectedBatchUploadSize(const GltfModel&              model,
                                                          std::span<const uint32_t>     textureIndices,
                                                          std::span<const uint32_t>     meshIndices)
{
  VkDeviceSize totalSize = 0;
  Ktx2Loader ktx2Loader;

  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex >= model.images.size() || !isUploadableGltfImage(model.images[textureIndex]))
    {
      continue;
    }

    const GltfImageData& imageData = model.images[textureIndex];
    Ktx2Loader::Ktx2Texture ktxTexture;
    const bool hasKtx2Sidecar = tryLoadKtx2Texture(model, imageData, ktx2Loader, ktxTexture);

    if(hasKtx2Sidecar)
    {
      totalSize += std::max(static_cast<VkDeviceSize>(ktxTexture.data.size()),
                            rawImageUploadBytes(model, imageData));
      totalSize += 16;  // alignment padding for KTX allocations
    }
    else
    {
      totalSize += rawImageUploadBytes(model, imageData);
      totalSize += 4;  // alignment padding for raw pixel allocations
    }
  }

  for(const uint32_t meshIndex : meshIndices)
  {
    if(meshIndex >= model.meshes.size())
    {
      continue;
    }

    const auto& meshData = model.meshes[meshIndex];
    if(meshData.positions.empty() || meshData.indices.empty())
    {
      continue;
    }

    const VkDeviceSize vertexCount = static_cast<VkDeviceSize>(meshData.positions.size() / 3);
    totalSize += vertexCount * 48ull;
    totalSize += static_cast<VkDeviceSize>(meshData.indices.size()) * sizeof(uint32_t);
    totalSize += 8;  // alignment padding for vertex (alignof(float)) + index (alignof(uint32_t)) allocations
  }

  return totalSize;
}

[[nodiscard]] VkDeviceSize computeSelectedTextureUploadSize(const GltfModel& model,
                                                            std::span<const uint32_t> textureIndices)
{
  VkDeviceSize totalSize = 0;
  Ktx2Loader ktx2Loader;

  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex >= model.images.size() || !isUploadableGltfImage(model.images[textureIndex]))
    {
      continue;
    }

    const GltfImageData& imageData = model.images[textureIndex];
    Ktx2Loader::Ktx2Texture ktxTexture;
    const bool hasKtx2Sidecar = tryLoadKtx2Texture(model, imageData, ktx2Loader, ktxTexture);

    if(hasKtx2Sidecar)
    {
      totalSize += std::max(static_cast<VkDeviceSize>(ktxTexture.data.size()),
                            rawImageUploadBytes(model, imageData));
      totalSize += 16;
    }
    else
    {
      totalSize += rawImageUploadBytes(model, imageData);
      totalSize += 4;
    }
  }

  return totalSize;
}

[[nodiscard]] VkDeviceSize computeSelectedMeshUploadSize(const GltfModel& model,
                                                         std::span<const uint32_t> meshIndices)
{
  VkDeviceSize totalSize = 0;

  for(const uint32_t meshIndex : meshIndices)
  {
    if(meshIndex >= model.meshes.size())
    {
      continue;
    }

    const auto& meshData = model.meshes[meshIndex];
    if(meshData.positions.empty() || meshData.indices.empty())
    {
      continue;
    }

    const VkDeviceSize vertexCount = static_cast<VkDeviceSize>(meshData.positions.size() / 3);
    totalSize += vertexCount * 48ull;
    totalSize += static_cast<VkDeviceSize>(meshData.indices.size()) * sizeof(uint32_t);
    totalSize += 8;
  }

  return totalSize;
}

struct TextureUploadDiagnostics
{
  uint32_t mipGenerationCount{0};
  uint32_t maxWidth{0};
  uint32_t maxHeight{0};
};

[[nodiscard]] TextureUploadDiagnostics gatherTextureUploadDiagnostics(const GltfModel& model,
                                                                     std::span<const uint32_t> textureIndices)
{
  TextureUploadDiagnostics diagnostics{};
  Ktx2Loader ktx2Loader;
  const VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  (void)physicalDevice;

  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex >= model.images.size() || !isUploadableGltfImage(model.images[textureIndex]))
    {
      continue;
    }

    const GltfImageData& imageData = model.images[textureIndex];
    diagnostics.maxWidth = std::max(diagnostics.maxWidth, static_cast<uint32_t>(imageData.width));
    diagnostics.maxHeight = std::max(diagnostics.maxHeight, static_cast<uint32_t>(imageData.height));

    Ktx2Loader::Ktx2Texture ktxTexture;
    const bool hasKtx2Sidecar = tryLoadKtx2Texture(model, imageData, ktx2Loader, ktxTexture);

    if(!hasKtx2Sidecar)
    {
      const uint32_t width = static_cast<uint32_t>(imageData.width);
      const uint32_t height = static_cast<uint32_t>(imageData.height);
      if(MipmapGenerator::calculateMipLevelCount(width, height) > 1u)
      {
        ++diagnostics.mipGenerationCount;
      }
    }
  }

  return diagnostics;
}

[[nodiscard]] glm::vec4 normalizePlane(glm::vec4 plane)
{
  const float length = glm::length(glm::vec3(plane));
  return length > 0.0f ? plane / length : plane;
}

[[nodiscard]] std::array<glm::vec4, shaderio::LGPUCullingFrustumPlaneCount> extractFrustumPlanes(const glm::mat4& viewProjection)
{
  const glm::mat4 transposed = glm::transpose(viewProjection);
  return {
      normalizePlane(transposed[3] + transposed[0]),
      normalizePlane(transposed[3] - transposed[0]),
      normalizePlane(transposed[3] + transposed[1]),
      normalizePlane(transposed[3] - transposed[1]),
      normalizePlane(transposed[3] + transposed[2]),
      normalizePlane(transposed[3] - transposed[2]),
  };
}

[[nodiscard]] shaderio::DrawUniforms buildShadowDrawUniforms(const MeshRecord& mesh)
{
  shaderio::DrawUniforms drawData{};
  drawData.modelMatrix = mesh.transform;
  drawData.prevModelMatrix = mesh.transform;
  drawData.baseColorFactor = mesh.baseColorFactor;
  drawData.baseColorTextureIndex = mesh.baseColorTextureIndex;
  drawData.normalTextureIndex = mesh.normalTextureIndex;
  drawData.metallicRoughnessTextureIndex = mesh.metallicRoughnessTextureIndex;
  drawData.occlusionTextureIndex = mesh.occlusionTextureIndex;
  drawData.emissiveTextureIndex = mesh.emissiveTextureIndex;
  drawData.metallicFactor = mesh.metallicFactor;
  drawData.roughnessFactor = mesh.roughnessFactor;
  drawData.normalScale = mesh.normalScale;
  drawData.occlusionStrength = mesh.occlusionStrength;
  drawData.emissiveFactor = mesh.emissiveFactor;
  drawData.materialWorkflow = mesh.materialWorkflow;
  drawData.alphaMode = mesh.alphaMode;
  drawData.alphaCutoff = mesh.alphaCutoff;
  return drawData;
}

[[nodiscard]] shaderio::DrawUniforms buildShadowDrawUniforms(const MeshRecord& mesh,
                                                             const SceneUploadResult::SceneDrawRecord& drawRecord)
{
  shaderio::DrawUniforms drawData = buildShadowDrawUniforms(mesh);
  drawData.modelMatrix = drawRecord.worldTransform;
  drawData.prevModelMatrix = drawRecord.worldTransform;
  drawData.alphaMode = static_cast<int32_t>(drawRecord.alphaMode);
  drawData.alphaCutoff = drawRecord.alphaCutoff;
  return drawData;
}

[[nodiscard]] glm::vec4 computeBoundsSphere(const MeshRecord& mesh, const glm::mat4& transform)
{
  const std::array<glm::vec3, 8> corners{{
      {mesh.localBoundsMin.x, mesh.localBoundsMin.y, mesh.localBoundsMin.z},
      {mesh.localBoundsMax.x, mesh.localBoundsMin.y, mesh.localBoundsMin.z},
      {mesh.localBoundsMin.x, mesh.localBoundsMax.y, mesh.localBoundsMin.z},
      {mesh.localBoundsMax.x, mesh.localBoundsMax.y, mesh.localBoundsMin.z},
      {mesh.localBoundsMin.x, mesh.localBoundsMin.y, mesh.localBoundsMax.z},
      {mesh.localBoundsMax.x, mesh.localBoundsMin.y, mesh.localBoundsMax.z},
      {mesh.localBoundsMin.x, mesh.localBoundsMax.y, mesh.localBoundsMax.z},
      {mesh.localBoundsMax.x, mesh.localBoundsMax.y, mesh.localBoundsMax.z},
  }};

  glm::vec3 boundsMin(std::numeric_limits<float>::max());
  glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
  for(const glm::vec3& corner : corners)
  {
    const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corner, 1.0f));
    boundsMin = glm::min(boundsMin, worldCorner);
    boundsMax = glm::max(boundsMax, worldCorner);
  }

  const glm::vec3 center = 0.5f * (boundsMin + boundsMax);
  return glm::vec4(center, glm::length(boundsMax - center));
}

struct OverlayCircle
{
  ImVec2 center{};
  float  radius{0.0f};
};

[[nodiscard]] bool projectWorldToViewportCircle(const shaderio::CameraUniforms& camera,
                                                const glm::vec4&               viewportRect,
                                                const glm::vec3&               worldCenter,
                                                float                          worldRadius,
                                                OverlayCircle&                 outCircle)
{
  if(viewportRect.z <= 1.0f || viewportRect.w <= 1.0f)
  {
    return false;
  }

  const glm::mat4 inverseView = glm::inverse(camera.view);
  const glm::vec3 right = glm::normalize(glm::vec3(inverseView[0]));

  const auto projectPoint = [&](const glm::vec3& point, ImVec2& screenPoint) -> bool {
    const glm::vec4 clip = camera.viewProjection * glm::vec4(point, 1.0f);
    if(std::abs(clip.w) <= 1e-5f)
    {
      return false;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    screenPoint.x = viewportRect.x + (ndc.x * 0.5f + 0.5f) * viewportRect.z;
    screenPoint.y = viewportRect.y + (ndc.y * 0.5f + 0.5f) * viewportRect.w;
    return true;
  };

  ImVec2 centerPoint{};
  ImVec2 radiusPoint{};
  if(!projectPoint(worldCenter, centerPoint) || !projectPoint(worldCenter + right * worldRadius, radiusPoint))
  {
    return false;
  }

  outCircle.center = centerPoint;
  outCircle.radius = std::max(1.0f, std::sqrt((radiusPoint.x - centerPoint.x) * (radiusPoint.x - centerPoint.x)
                                              + (radiusPoint.y - centerPoint.y) * (radiusPoint.y - centerPoint.y)));
  return true;
}

}  // namespace

rhi::ArgumentBinding makeArgumentBinding(uint32_t logicalIndex,
                                         rhi::BindlessResourceType resourceType,
                                         uint32_t descriptorCount,
                                         rhi::ResourceVisibility visibility);

static VkFormat selectSwapchainImageFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
  const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = surface};

  uint32_t formatCount = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, nullptr));
  ASSERT(formatCount > 0, "RenderDevice::init requires at least one surface format");

  std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, formats.data()));

  for(const VkSurfaceFormat2KHR& format2 : formats)
  {
    if(format2.surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM && format2.surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
      return format2.surfaceFormat.format;
    }
  }

  return formats.front().surfaceFormat.format;
}

template <typename T>
static T fromNativeHandle(uint64_t handle)
{
  // Always reinterpret the 64-bit handle as the target Vulkan handle type.
  // This avoids issues when VkInstance/VkDevice/etc. are opaque handles (pointer-like)
  // and cannot be created via static_cast from a integer.
  return reinterpret_cast<T>(static_cast<uintptr_t>(handle));
}

static bool isValidExtent(rhi::Extent2D extent)
{
  return extent.width > 0 && extent.height > 0;
}

static bool isValidExtent(VkExtent2D extent)
{
  return extent.width > 0 && extent.height > 0;
}

[[nodiscard]] rhi::Extent2D toRhiExtent(VkExtent2D extent)
{
  return {extent.width, extent.height};
}

[[nodiscard]] VkExtent2D toVkExtent(rhi::Extent2D extent)
{
  return {extent.width, extent.height};
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

[[nodiscard]] uint64_t resolveNativeImage(const rhi::Device& device, rhi::TextureHandle handle)
{
  return handle.isNull() ? 0u : device.resolveTextureBackendHandle(handle);
}

[[nodiscard]] rhi::ResourceVisibility toResourceVisibility(rhi::ShaderStage stages)
{
  return static_cast<rhi::ResourceVisibility>(static_cast<uint32_t>(stages));
}

[[nodiscard]] rhi::TextureAspect sceneDepthTextureAspect(rhi::TextureFormat format)
{
  switch(format)
  {
    case rhi::TextureFormat::d24UnormS8:
    case rhi::TextureFormat::d32SfloatS8:
      return rhi::TextureAspect::depthStencil;
    case rhi::TextureFormat::d16Unorm:
    case rhi::TextureFormat::d32Sfloat:
    default:
      return rhi::TextureAspect::depth;
  }
}

static bool extentChanged(rhi::Extent2D a, rhi::Extent2D b)
{
  return a.width != b.width || a.height != b.height;
}

static bool extentChanged(VkExtent2D a, VkExtent2D b)
{
  return a.width != b.width || a.height != b.height;
}

static VmaAllocator createAllocator(const VkPhysicalDevice physicalDevice, const VkDevice device, const VkInstance instance, const uint32_t apiVersion)
{
  VmaAllocatorCreateInfo allocatorInfo{
      .physicalDevice   = physicalDevice,
      .device           = device,
      .instance         = instance,
      .vulkanApiVersion = apiVersion,
  };
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

  const VmaVulkanFunctions functions{
      .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
  };
  allocatorInfo.pVulkanFunctions = &functions;

  VmaAllocator allocator{nullptr};
  VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator));
  return allocator;
}

static utils::Buffer createBuffer(const VkDevice               device,
                                  const VmaAllocator           allocator,
                                  const VkDeviceSize           size,
                                  const VkBufferUsageFlags2KHR usage,
                                  const VmaMemoryUsage         memoryUsage = VMA_MEMORY_USAGE_AUTO,
                                  VmaAllocationCreateFlags     flags       = {},
                                  bool                         enableExternalHostMemory = false)
{
  const bool hostAccessibleBuffer = memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY || memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU
                                    || memoryUsage == VMA_MEMORY_USAGE_GPU_TO_CPU
                                    || (flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                 | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                 | VMA_ALLOCATION_CREATE_MAPPED_BIT)) != 0;

  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };

  VkExternalMemoryBufferCreateInfo externalMemoryBufferCreateInfo{
      .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
  };

  const void* bufferCreatePNext = (enableExternalHostMemory && hostAccessibleBuffer)
                                      ? static_cast<const void*>(&externalMemoryBufferCreateInfo)
                                      : static_cast<const void*>(&usageInfo);

  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = bufferCreatePNext,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};
  if((allocInfo.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) != 0)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }
  if(size > 64ULL * 1024)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  }

  utils::Buffer     buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;

  const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.buffer};
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  return buffer;
}

static void destroyBuffer(const VmaAllocator allocator, utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

static utils::Buffer toUtilsBuffer(const UploadBufferRecord& buffer);

static void destroyBuffer(const VmaAllocator allocator, UploadBufferRecord& buffer)
{
  ASSERT(buffer.rhiHandle.isNull(), "RHI-owned UploadBufferRecord must be destroyed through destroyUploadBufferRecord");
  utils::Buffer nativeBuffer = toUtilsBuffer(buffer);
  destroyBuffer(allocator, nativeBuffer);
  buffer = {};
}

static void writeHostVisibleBuffer(const VmaAllocator allocator, utils::Buffer& buffer, const void* data, const VkDeviceSize size);
static void writeHostVisibleBufferRange(const VmaAllocator allocator,
                                        utils::Buffer&      buffer,
                                        VkDeviceSize        offset,
                                        const void*         data,
                                        VkDeviceSize        size);

static utils::Buffer toUtilsBuffer(const UploadBufferRecord& buffer)
{
  return utils::Buffer{
      .buffer     = reinterpret_cast<VkBuffer>(buffer.buffer),
      .allocation = reinterpret_cast<VmaAllocation>(buffer.allocation),
      .address    = static_cast<VkDeviceAddress>(buffer.address),
      .mapped     = buffer.mapped,
  };
}

static UploadBufferRecord toUploadBufferRecord(const utils::Buffer& buffer)
{
  return UploadBufferRecord{
      .buffer     = reinterpret_cast<uintptr_t>(buffer.buffer),
      .allocation = reinterpret_cast<uintptr_t>(buffer.allocation),
      .address    = static_cast<uintptr_t>(buffer.address),
      .mapped     = buffer.mapped,
  };
}

static UploadBufferRecord toUploadBufferRecord(const rhi::vulkan::BufferRecord& buffer, rhi::BufferHandle handle)
{
  return UploadBufferRecord{
      .buffer    = static_cast<uintptr_t>(buffer.nativeBuffer),
      .allocation = static_cast<uintptr_t>(buffer.nativeAllocation),
      .address   = static_cast<uintptr_t>(buffer.gpuAddress),
      .mapped    = buffer.mapped,
      .rhiHandle = handle,
  };
}

static void destroyUploadBufferRecord(rhi::Device* device, const VmaAllocator allocator, UploadBufferRecord& buffer)
{
  if(!buffer.rhiHandle.isNull())
  {
    if(device != nullptr)
    {
      device->destroyBuffer(buffer.rhiHandle);
    }
    buffer = {};
    return;
  }

  destroyBuffer(allocator, buffer);
}

static void destroyUploadBufferRecord(rhi::Device* device, const VmaAllocator allocator, const UploadBufferRecord& buffer)
{
  if(buffer.isNull())
  {
    return;
  }
  if(!buffer.rhiHandle.isNull())
  {
    if(device != nullptr)
    {
      device->destroyBuffer(buffer.rhiHandle);
    }
    return;
  }

  utils::Buffer nativeBuffer = toUtilsBuffer(buffer);
  destroyBuffer(allocator, nativeBuffer);
}

static utils::Buffer createBufferAndUploadData(const VkDevice               device,
                                               const VmaAllocator           allocator,
                                               rhi::Device&                 rhiDevice,
                                               std::vector<rhi::BufferHandle>& stagingBuffers,
                                               rhi::CommandBuffer&          cmd,
                                               std::span<const std::byte>   data,
                                               const VkBufferUsageFlags2KHR usage,
                                               const upload::StaticBufferUploadPolicy& uploadPolicy)
{
  (void)uploadPolicy;
  (void)device;
  utils::Buffer nativeBuffer{};
  const VkBufferCreateInfo bufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size  = static_cast<VkDeviceSize>(data.size_bytes()),
      .usage = static_cast<VkBufferUsageFlags>(static_cast<VkBufferUsageFlags2KHR>(usage) | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR),
  };
  const VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &nativeBuffer.buffer, &nativeBuffer.allocation, nullptr));

  const rhi::BufferHandle bufferHandle = rhiDevice.registerExternalBuffer(reinterpret_cast<uintptr_t>(nativeBuffer.buffer));

  BatchUploadContext upload;
  upload.init(rhiDevice, static_cast<uint64_t>(data.size_bytes()));
  const BatchUploadContext::Slice slice = upload.allocate(static_cast<uint64_t>(data.size_bytes()), 16);
  std::memcpy(slice.cpuPtr, data.data(), data.size_bytes());
  upload.recordBufferUpload(slice, bufferHandle, 0, static_cast<uint64_t>(data.size_bytes()));
  upload.executeUploads(cmd);
  rhiDevice.destroyBuffer(bufferHandle);

  rhi::BufferHandle stagingBuffer = upload.releaseStagingBuffer();
  if(!stagingBuffer.isNull())
  {
    stagingBuffers.push_back(stagingBuffer);
  }
  return nativeBuffer;
}

static utils::Image createImage(const VmaAllocator allocator, const VkImageCreateInfo& imageInfo)
{
  const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  utils::Image                  image{};
  VmaAllocationInfo             allocInfo{};
  VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image.image, &image.allocation, &allocInfo));
  return image;
}

static void destroyImageResource(const VkDevice device, const VmaAllocator allocator, utils::ImageResource& image)
{
  if(image.view != VK_NULL_HANDLE)
  {
    vkDestroyImageView(device, image.view, nullptr);
  }
  if(image.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(allocator, image.image, image.allocation);
  }
  image = {};
}

static void freeRhiStagingBuffers(rhi::Device* device, std::vector<rhi::BufferHandle>& stagingBuffers)
{
  if(device != nullptr)
  {
    for(rhi::BufferHandle buffer : stagingBuffers)
    {
      if(!buffer.isNull())
      {
        device->destroyBuffer(buffer);
      }
    }
  }
  stagingBuffers.clear();
}

static rhi::ShaderReflectionData buildRasterShaderReflection()
{
  rhi::ShaderReflectionData reflection{};
  reflection.name        = "shader.rast";
  reflection.entryPoints = {
      rhi::ShaderEntryPoint{"vertexMain", rhi::ShaderStageFlagBits::vertex},
      rhi::ShaderEntryPoint{"fragmentMain", rhi::ShaderStageFlagBits::fragment},
  };
  reflection.resourceBindings = {
      rhi::ShaderResourceBinding{"textures", rhi::ShaderResourceType::sampler, rhi::DescriptorType::combinedImageSampler,
                                 rhi::ShaderStageFlagBits::fragment, static_cast<uint32_t>(shaderio::LSetTextures),
                                 static_cast<uint32_t>(shaderio::LBindTextures), RenderDevice::kDemoMaterialSlotCount, 0},
      rhi::ShaderResourceBinding{"sceneInfo", rhi::ShaderResourceType::uniformBuffer, rhi::DescriptorType::uniformBufferDynamic,
                                 rhi::ShaderStageFlagBits::vertex | rhi::ShaderStageFlagBits::fragment,
                                 static_cast<uint32_t>(shaderio::LSetScene), static_cast<uint32_t>(shaderio::LBindSceneInfo), 1, 0},
  };
  reflection.pushConstantRanges = {
      rhi::PushConstantRange{rhi::ShaderStageFlagBits::fragment, 0, sizeof(shaderio::PushConstant)},
  };
  reflection.pushConstantSize        = sizeof(shaderio::PushConstant);
  reflection.specializationConstants = {
      rhi::SpecializationConstant{0, 0, sizeof(VkBool32)},
  };
  return reflection;
}

static rhi::ShaderReflectionData buildComputeShaderReflection()
{
  rhi::ShaderReflectionData reflection{};
  reflection.name        = "shader.comp";
  reflection.entryPoints = {
      rhi::ShaderEntryPoint{"main", rhi::ShaderStageFlagBits::compute},
  };
  reflection.pushConstantRanges = {
      rhi::PushConstantRange{rhi::ShaderStageFlagBits::compute, 0, sizeof(shaderio::PushConstantCompute)},
  };
  reflection.pushConstantSize = sizeof(shaderio::PushConstantCompute);
  return reflection;
}

std::optional<uint32_t> RenderDevice::mapSetSlotToLegacyShaderSet(ArgumentSlot slot)
{
  switch(slot)
  {
    case ArgumentSlot::material:
      return static_cast<uint32_t>(shaderio::LSetTextures);
    case ArgumentSlot::drawDynamic:
      return static_cast<uint32_t>(shaderio::LSetScene);
    default:
      return std::nullopt;
  }
}

void RenderDevice::init(void* window, rhi::Surface& surface, bool vSync)
{
  VK_CHECK(volkInitialize());

  m_swapchainDependent.vSync = vSync;
  m_materials                = MaterialResources{};

  VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
  VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayoutsFeature{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};

  rhi::DeviceCreateInfo deviceCreateInfo;
#ifdef _DEBUG
  deviceCreateInfo.enableValidationLayers = true;
#else
  deviceCreateInfo.enableValidationLayers = false;
#endif
#ifdef __ANDROID__
  deviceCreateInfo.enableValidationLayers = false;
  deviceCreateInfo.instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  deviceCreateInfo.instanceExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
  uint32_t     glfwExtensionCount = 0;
  const char** glfwExtensions     = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
  for(uint32_t i = 0; i < glfwExtensionCount; ++i)
  {
    deviceCreateInfo.instanceExtensions.push_back(glfwExtensions[i]);
  }
#endif
  // Debug utils for event markers (RenderDoc, PIX, etc.)
  deviceCreateInfo.instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  deviceCreateInfo.deviceExtensions.push_back({VK_KHR_SWAPCHAIN_EXTENSION_NAME, true, nullptr});
  deviceCreateInfo.deviceExtensions.push_back({VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, false, nullptr});
  deviceCreateInfo.deviceExtensions.push_back({VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, false, &unifiedImageLayoutsFeature});
  deviceCreateInfo.deviceExtensions.push_back({VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, false, &dynamicState3Features});
  deviceCreateInfo.deviceExtensions.push_back({"VK_EXT_full_screen_exclusive", false, nullptr});

  m_device.device = std::make_unique<rhi::vulkan::VulkanDevice>();
  m_device.device->init(deviceCreateInfo);

  // The device backs its texture-view/image handles with the render-layer-owned resource
  // table (the VMA allocator is injected later, once it is created).
  static_cast<rhi::vulkan::VulkanDevice&>(*m_device.device).setResourceTable(&m_device.resourceTable);

  const rhi::CapabilityReport capabilityReport = m_device.device->queryCapabilities();
  ASSERT(m_device.device->supports(rhi::CapabilityTier::Core), "RenderDevice::init requires RHI Core capability tier");
  ASSERT(capabilityReport.coreGraphics && capabilityReport.coreCompute && capabilityReport.coreBindless,
         "RenderDevice::init requires graphics+compute+bindless capability floor");

  const VkInstance nativeInstance = fromNativeHandle<VkInstance>(m_device.device->getBackendInstanceHandle());
  const VkPhysicalDevice nativePhysicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle());
  const VkDevice       nativeDevice        = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const rhi::QueueInfo graphicsQueueInfo   = m_device.device->getGraphicsQueue();
  const VkQueue        nativeGraphicsQueue = fromNativeHandle<VkQueue>(graphicsQueueInfo.backendHandle);

  rhi::WindowHandle windowHandle{window};
  surface.init(static_cast<void*>(nativeInstance), static_cast<void*>(nativePhysicalDevice), windowHandle);

  m_device.allocator = createAllocator(nativePhysicalDevice, nativeDevice, nativeInstance, m_device.device->getApiVersion());
  static_cast<rhi::vulkan::VulkanDevice&>(*m_device.device).setAllocator(m_device.allocator);
  m_device.staticBufferUploadPolicy =
      upload::buildStaticBufferUploadPolicy(m_device.device->getPhysicalMemoryProperties());
  if(m_device.staticBufferUploadPolicy.allowDirectHostVisibleDeviceLocalUpload)
  {
    LOGI("Static buffer upload: staging fallback with ReBAR direct-write for buffers up to %.1f MiB",
         static_cast<double>(m_device.staticBufferUploadPolicy.directUploadThreshold) / (1024.0 * 1024.0));
  }
  else
  {
    LOGI("Static buffer upload: host staging to device-local buffers");
  }


  m_meshPool.init(reinterpret_cast<uintptr_t>(nativeDevice),
                  reinterpret_cast<uintptr_t>(m_device.allocator),
                  m_device.device.get(),
                  m_device.staticBufferUploadPolicy);

  const VkSurfaceKHR nativeSurface = reinterpret_cast<VkSurfaceKHR>(surface.getBackendHandle());
  ASSERT(nativeSurface != VK_NULL_HANDLE, "RenderDevice::init requires a valid initialized surface");
  DBG_VK_NAME(nativeSurface);
  m_swapchainDependent.swapchainImageFormat = toPortableTextureFormat(selectSwapchainImageFormat(nativePhysicalDevice, nativeSurface));

  createTransientCommandPool();

  auto nativeSwapchain = std::make_unique<rhi::vulkan::VulkanSwapchain>();
  nativeSwapchain->init(static_cast<void*>(nativePhysicalDevice), static_cast<void*>(nativeDevice),
                        static_cast<void*>(nativeGraphicsQueue), static_cast<void*>(nativeSurface),
                        static_cast<void*>(m_device.transientCmdPool), m_swapchainDependent.vSync);
  m_swapchainDependent.swapchain = std::move(nativeSwapchain);
  m_swapchainDependent.swapchain->rebuild();
  const rhi::Extent2D swapchainExtent = m_swapchainDependent.swapchain->getExtent();
  m_swapchainDependent.windowSize     = swapchainExtent;

  m_swapchainDependent.currentImageIndex = 0;
  m_swapchainDependent.imageStates.assign(
      m_swapchainDependent.swapchain->getMaxFramesInFlight(),
      rhi::ResourceState::Undefined);

  // Create material bind group BEFORE createFrameSubmission() because it's needed for pipeline layout
  createMaterialArgumentTable();
  createFrameSubmission(m_swapchainDependent.swapchain->getRequestedImageCount());
  createDescriptorPool();
  initImGui(window);

  // Scene sampler: linear mag/min, nearest mip, repeat, maxLod 0 (matches the prior
  // zero-initialized VkSamplerCreateInfo). Created through the RHI; held as a handle.
  m_device.sceneLinearSamplerHandle = m_device.device->createSampler(rhi::SamplerDesc{
      .magFilter    = rhi::Filter::linear,
      .minFilter    = rhi::Filter::linear,
      .mipmapMode   = rhi::MipmapMode::nearest,
      .addressModeU = rhi::AddressMode::repeat,
      .addressModeV = rhi::AddressMode::repeat,
      .addressModeW = rhi::AddressMode::repeat,
      .maxLod       = 0.0f,
      .debugName    = "SceneLinearSampler",
  });
  const VkSampler linearSampler =
      reinterpret_cast<VkSampler>(static_cast<uintptr_t>(m_device.resourceTable.resolveSampler(m_device.sceneLinearSamplerHandle)));
  DBG_VK_NAME(linearSampler);

  {
    VkCommandBuffer cmd = utils::beginSingleTimeCommands(nativeDevice, m_device.transientCmdPool);
    rhi::vulkan::VulkanCommandBuffer rhiCmd;
    rhiCmd.setTarget(cmd, &m_device.resourceTable);

    const VkFormat             depthFormat = utils::findDepthFormat(nativePhysicalDevice);
    SceneResources::CreateInfo sceneResourcesInit{
        .size          = m_swapchainDependent.windowSize,
        .color         = {
            rhi::TextureFormat::rgba8Unorm,  // GBuffer0: BaseColor.rgb + roughness
            rhi::TextureFormat::rgba8Unorm,  // GBuffer1: oct normal.xy + metallic + AO
            rhi::TextureFormat::rgba16Sfloat,  // GBuffer2: emissive.rgb + material flags
        },
        .depth         = toPortableTextureFormat(depthFormat),
        .linearSampler = m_device.sceneLinearSamplerHandle,
    };
    LOGI("RenderDevice::init: scene resources begin");
    m_swapchainDependent.sceneResources.init(*m_device.device, rhiCmd, sceneResourcesInit);
    LOGI("RenderDevice::init: scene resources completed");
    LOGI("RenderDevice::init: IBL resources begin");
    createIBLResources(rhiCmd);
    LOGI("RenderDevice::init: IBL resources completed");
    LOGI("RenderDevice::init: GPU culling resources begin");
    createGPUCullingResources();
    LOGI("RenderDevice::init: GPU culling resources completed");
    LOGI("RenderDevice::init: shadow culling resources begin");
    createShadowCullingResources();
    LOGI("RenderDevice::init: shadow culling resources completed");
    LOGI("RenderDevice::init: light resources begin");
    createLightResources();
    LOGI("RenderDevice::init: light resources completed");

    // Initialize CSM shadow cascade resources
    CSMShadowResources::CreateInfo csmInfo{
        .cascadeCount      = 4,
        .cascadeResolution = 1024,
        .shadowFormat      = rhi::TextureFormat::d32Sfloat,
    };
    LOGI("RenderDevice::init: CSM resources begin");
    m_csmShadowResources.init(*m_device.device, rhiCmd, csmInfo);
    LOGI("RenderDevice::init: CSM resources completed");

    // Create the per-cascade depth render-target views through the RHI texture-view
    // registry (a single 2D layer of the cascade array image each). CSMShadowResources
    // no longer owns these; the CSM pass binds them by handle.
    for(uint32_t cascadeIndex = 0; cascadeIndex < m_csmShadowResources.getCascadeCount(); ++cascadeIndex)
    {
      m_csmCascadeViewHandles[cascadeIndex] = createTextureView(rhi::TextureViewCreateDesc{
          .image          = m_csmShadowResources.getCascadeImage(),
          .format         = m_csmShadowResources.getShadowFormat(),
          .viewType       = rhi::ImageViewType::e2D,
          .aspect         = rhi::TextureAspect::depth,
          .baseArrayLayer = cascadeIndex,
          .layerCount     = 1,
          .debugName      = "CSM_CascadeLayerView",
      });
    }
    // Full-array sampling view (all cascades), used by the lighting GBuffer descriptor set.
    m_csmCascadeArrayViewHandle = createTextureView(rhi::TextureViewCreateDesc{
        .image        = m_csmShadowResources.getCascadeImage(),
        .format       = m_csmShadowResources.getShadowFormat(),
        .viewType     = rhi::ImageViewType::e2DArray,
        .aspect       = rhi::TextureAspect::depth,
        .baseArrayLayer = 0,
        .layerCount     = m_csmShadowResources.getCascadeCount(),
        .debugName      = "CSM_CascadeArrayView",
    });

    utils::endSingleTimeCommands(cmd, nativeDevice, m_device.transientCmdPool, nativeGraphicsQueue);
  }

  createGraphicsArgumentTables();
  prebuildRequiredPipelineVariants();

  {
    VkCommandBuffer cmd = utils::beginSingleTimeCommands(nativeDevice, m_device.transientCmdPool);
    rhi::vulkan::VulkanCommandBuffer rhiCmd;
    rhiCmd.setTarget(cmd, &m_device.resourceTable);
    m_device.vertexBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, *m_device.device, m_device.rhiStagingBuffers, rhiCmd,
                                                      std::as_bytes(std::span{s_vertices}),
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      m_device.staticBufferUploadPolicy);
    DBG_VK_NAME(m_device.vertexBuffer.buffer);

    m_device.pointsBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, *m_device.device, m_device.rhiStagingBuffers, rhiCmd,
                                                      std::as_bytes(std::span{s_points}), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      m_device.staticBufferUploadPolicy);
    DBG_VK_NAME(m_device.pointsBuffer.buffer);

    const std::vector<std::string> searchPaths = {".", "resources", "../resources", "../../resources"};
    std::string                    filename    = utils::findFile("image1.jpg", searchPaths);
#ifdef __ANDROID__
    if(filename.empty())
    {
      filename = "image1.jpg";
    }
#endif
    ASSERT(!filename.empty(), "Could not load texture image!");
    utils::ImageResource materialImage0   = loadAndCreateImage(rhiCmd, filename);
    const TextureHandle  materialTexture0 = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = materialImage0.view,
                .sampledImageLayout = materialImage0.layout,
                .sampledViewHandle  = registerExternalTextureView(reinterpret_cast<uint64_t>(materialImage0.view)),
            },
        .cold =
            {
                .ownedImage   = materialImage0,
                .sourceExtent = materialImage0.extent,
            },
    });

    filename = utils::findFile("image2.jpg", searchPaths);
#ifdef __ANDROID__
    if(filename.empty())
    {
      filename = "image2.jpg";
    }
#endif
    ASSERT(!filename.empty(), "Could not load texture image!");
    utils::ImageResource materialImage1   = loadAndCreateImage(rhiCmd, filename);
    const TextureHandle  materialTexture1 = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = materialImage1.view,
                .sampledImageLayout = materialImage1.layout,
                .sampledViewHandle  = registerExternalTextureView(reinterpret_cast<uint64_t>(materialImage1.view)),
            },
        .cold =
            {
                .ownedImage   = materialImage1,
                .sourceExtent = materialImage1.extent,
            },
    });

    m_materials.sampleMaterials[0]    = m_materials.materialPool.emplace(MaterialResources::MaterialRecord{
        .sampledTexture  = materialTexture0,
        .descriptorIndex = 0,
        .debugName       = "image1-material",
    });
    m_materials.sampleMaterials[1]    = m_materials.materialPool.emplace(MaterialResources::MaterialRecord{
        .sampledTexture  = materialTexture1,
        .descriptorIndex = 1,
        .debugName       = "image2-material",
    });
    m_materials.viewportTextureHandle = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind             = MaterialResources::TextureRuntimeKind::outputTexture,
                .viewportAttachmentIndex = 0,
            },
    });

    utils::endSingleTimeCommands(cmd, nativeDevice, m_device.transientCmdPool, nativeGraphicsQueue);
  }
  freeRhiStagingBuffers(m_device.device.get(), m_device.rhiStagingBuffers);

  updateGraphicsArgumentTables();
}

std::unique_ptr<rhi::Surface> RenderDevice::createSurface() const
{
  return std::make_unique<rhi::vulkan::VulkanSurface>();
}

void RenderDevice::shutdown(rhi::Surface& surface)
{
  m_device.device->waitIdle();
  VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());

  if(m_swapchainDependent.swapchain)
  {
    auto* vkSwapchain = static_cast<rhi::vulkan::VulkanSwapchain*>(m_swapchainDependent.swapchain.get());
    vkSwapchain->deinit();
    m_swapchainDependent.swapchain.reset();
  }
  if(m_device.device)
  {
    m_device.device->destroySampler(m_device.sceneLinearSamplerHandle);
    m_device.device->destroySampler(m_device.gbufferLinearSamplerHandle);
    m_device.device->destroySampler(m_materials.materialSamplerHandle);
    m_device.sceneLinearSamplerHandle   = {};
    m_device.gbufferLinearSamplerHandle = {};
    m_materials.materialSamplerHandle   = {};
  }

  // Destroy bind groups FIRST (they use descriptor pools)
  destroyArgumentTablesAndLayouts();

  // Shutdown ImGui Vulkan backend BEFORE destroying uiDescriptorPool
  // ImGui_ImplVulkan_Shutdown() frees descriptor sets from uiDescriptorPool
  ImGui_ImplVulkan_Shutdown();
#ifdef __ANDROID__
  ImGui_ImplAndroid_Shutdown();
#else
  ImGui_ImplGlfw_Shutdown();
#endif
  ImGui::DestroyContext();

  // Destroy Vulkan objects that are not managed by smart pointers
  m_device.gpuCullingArgumentTables.clear();
  m_device.shadowCullingArgumentTables.clear();
  destroyPassGpuProfileResources();
  m_lightResources.deinit();
  destroyIBLResources();
  if(m_device.uiDescriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, m_device.uiDescriptorPool, nullptr);
    m_device.uiDescriptorPool = VK_NULL_HANDLE;
  }
  if(m_device.descriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, m_device.descriptorPool, nullptr);
    m_device.descriptorPool = VK_NULL_HANDLE;
  }
  if(m_device.transientCmdPool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool(device, m_device.transientCmdPool, nullptr);
    m_device.transientCmdPool = VK_NULL_HANDLE;
  }
  destroyPipelines();
  // Destroy any owned texture views still registered (adopted/external ones are left to
  // their owners). Route through destroyTextureView so each view is both freed and removed
  // from the registry: later per-subsystem destroyTextureView() calls then hit a generation
  // mismatch and become safe no-ops instead of double-freeing the same VkImageView.
  {
    std::vector<rhi::TextureViewHandle> ownedViewHandles;
    m_device.resourceTable.forEachTextureView([&](rhi::TextureViewHandle handle, const rhi::vulkan::TextureViewRecord& record) {
      if(record.owned && record.nativeView != 0)
      {
        ownedViewHandles.push_back(handle);
      }
    });
    for(const rhi::TextureViewHandle handle : ownedViewHandles)
    {
      m_device.device->destroyTextureView(handle);
    }
  }
  // Per-frame argument tables already destroyed by destroyArgumentTablesAndLayouts() above
  // Just cleanup the transient allocators
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    destroyBuffer(m_device.allocator, frameUserData.lightingBuffer);
    destroyBuffer(m_device.allocator, frameUserData.lightCullingBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingObjectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingIndirectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingDrawCountBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingStatsBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingUniformBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingResultBuffer);
    destroyBuffer(m_device.allocator, frameUserData.shadowCullingObjectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.shadowCullingIndirectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.shadowCullingDrawDataBuffer);
    destroyBuffer(m_device.allocator, frameUserData.mdiDrawDataBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gbufferMdiDrawDataBuffer);
    destroyBuffer(m_device.allocator, frameUserData.depthMdiDrawDataBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuDrivenPersistentIndirectStreamBuffer);
    frameUserData.transientAllocator.destroy();
  }
  m_perFrame.frameUserData.clear();
  if(m_perFrame.frameContext)
  {
    m_perFrame.frameContext->deinit();
    m_perFrame.frameContext.reset();
  }

  destroyBuffer(m_device.allocator, m_device.vertexBuffer);
  destroyBuffer(m_device.allocator, m_device.pointsBuffer);
  std::vector<TextureHandle> texturesToDestroy;
  m_materials.texturePool.forEachActive(
      [&](TextureHandle handle, const MaterialResources::TextureRecord&) { texturesToDestroy.push_back(handle); });
  for(const TextureHandle handle : texturesToDestroy)
  {
    if(const MaterialResources::TextureHotData* hot = tryGetTextureHot(handle); hot != nullptr && !hot->sampledViewHandle.isNull())
    {
      m_device.resourceTable.removeTextureView(hot->sampledViewHandle);
    }
    const MaterialResources::TextureColdData* textureCold = tryGetTextureCold(handle);
    if(textureCold != nullptr && textureCold->ownedImage.image != VK_NULL_HANDLE)
    {
      utils::ImageResource image = textureCold->ownedImage;
      destroyImageResource(device, m_device.allocator, image);
    }
    m_materials.texturePool.destroy(handle);
  }

  std::vector<MaterialHandle> materialsToDestroy;
  m_materials.materialPool.forEachActive(
      [&](MaterialHandle handle, const MaterialResources::MaterialRecord&) { materialsToDestroy.push_back(handle); });
  for(const MaterialHandle handle : materialsToDestroy)
  {
    m_materials.materialPool.destroy(handle);
  }

  m_csmShadowResources.deinit();
  m_swapchainDependent.sceneResources.deinit();
  m_meshPool.deinit();
  freeRhiStagingBuffers(m_device.device.get(), m_device.rhiStagingBuffers);
  if(m_device.device)
  {
    // RHI destroy calls above enqueue owned native resources for delayed physical
    // destruction. Drain them while the VMA allocator is still alive.
    m_device.device->waitIdle();
  }
  if(m_device.allocator != nullptr)
  {
    vmaDestroyAllocator(m_device.allocator);
    m_device.allocator = nullptr;
  }
  surface.deinit();
  if(m_device.device)
  {
    m_device.device->deinit();
    m_device.device.reset();
  }
}

void RenderDevice::resize(rhi::Extent2D size)
{
  rebuildSwapchainDependentResources(size);
}

void RenderDevice::setVSync(bool enabled)
{
  m_swapchainDependent.vSync = enabled;
  if(m_swapchainDependent.swapchain == nullptr)
  {
    return;
  }

  auto* vkSwapchain = static_cast<rhi::vulkan::VulkanSwapchain*>(m_swapchainDependent.swapchain.get());
  vkSwapchain->setVSync(enabled);
}

void RenderDevice::setFullscreen(bool enabled, void* platformHandle)
{
  if(m_swapchainDependent.swapchain == nullptr)
  {
    return;
  }

  auto* vkSwapchain = static_cast<rhi::vulkan::VulkanSwapchain*>(m_swapchainDependent.swapchain.get());
  vkSwapchain->set_fullscreen(enabled, platformHandle);
  vkSwapchain->requestRebuild();
}

const char* RenderDevice::getSwapchainPresentModeName() const
{
  if(m_swapchainDependent.swapchain == nullptr)
  {
    return "Unavailable";
  }

  const auto* vkSwapchain = static_cast<const rhi::vulkan::VulkanSwapchain*>(m_swapchainDependent.swapchain.get());
  switch(vkSwapchain->getPresentMode())
  {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return "Immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:
      return "Mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:
      return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      return "FIFO Relaxed";
    default:
      return "Other";
  }
}

TextureHandle RenderDevice::getViewportTextureHandle() const
{
  return m_materials.viewportTextureHandle;
}

ImTextureID RenderDevice::getViewportTextureID(TextureHandle handle) const
{
  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(handle);
  if(textureHot == nullptr)
  {
    LOGW("RenderDevice::getViewportTextureID rejected stale/invalid texture handle (index=%u generation=%u)", handle.index,
         handle.generation);
    return ImTextureID{};
  }

  if(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::viewportAttachment)
  {
    return m_swapchainDependent.sceneResources.getImTextureID(textureHot->viewportAttachmentIndex);
  }

  if(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::outputTexture)
  {
    return m_swapchainDependent.sceneResources.getOutputTextureImID();
  }

  return ImTextureID{};
}

MaterialHandle RenderDevice::getMaterialHandle(uint32_t slot) const
{
  if(slot < kDemoMaterialSlotCount)
  {
    return m_materials.sampleMaterials[slot];
  }
  return kNullMaterialHandle;
}

PipelineHandle RenderDevice::getLightPipelineHandle() const
{
  return m_lightPipeline;
}

PipelineHandle RenderDevice::getGPUDrivenLightHdrPipelineHandle() const
{
  return m_gpuDrivenLightHdrPipeline.isNull() ? m_lightPipeline : m_gpuDrivenLightHdrPipeline;
}

PipelineHandle RenderDevice::getGPUDrivenSkyboxPipelineHandle() const
{
  return m_gpuDrivenSkyboxPipeline;
}

PipelineHandle RenderDevice::getBloomPrefilterPipelineHandle() const
{
  return m_bloomPrefilterPipeline;
}

PipelineHandle RenderDevice::getBloomDownsamplePipelineHandle() const
{
  return m_bloomDownsamplePipeline;
}

PipelineHandle RenderDevice::getFinalColorPipelineHandle() const
{
  return m_finalColorPipeline;
}

PipelineHandle RenderDevice::getVelocityPipelineHandle() const
{
  return m_velocityPipeline;
}

PipelineHandle RenderDevice::getTAAResolvePipelineHandle() const
{
  return m_taaResolvePipeline;
}

PipelineHandle RenderDevice::getDepthPrepassOpaquePipelineHandle() const
{
  return m_depthPrepassOpaquePipeline;
}

PipelineHandle RenderDevice::getDepthPrepassAlphaTestPipelineHandle() const
{
  return m_depthPrepassAlphaTestPipeline;
}

PipelineHandle RenderDevice::getDepthPrepassOpaqueMDIPipelineHandle() const
{
  return m_depthPrepassOpaqueMDIPipeline;
}

PipelineHandle RenderDevice::getDepthPrepassAlphaTestMDIPipelineHandle() const
{
  return m_depthPrepassAlphaTestMDIPipeline;
}

PipelineHandle RenderDevice::getGBufferOpaquePipelineHandle() const
{
  return m_gbufferOpaquePipeline;
}

PipelineHandle RenderDevice::getGBufferAlphaTestPipelineHandle() const
{
  return m_gbufferAlphaTestPipeline;
}

PipelineHandle RenderDevice::getGBufferOpaqueMDIPipelineHandle() const
{
  return m_gbufferOpaqueMDIPipeline;
}

PipelineHandle RenderDevice::getGBufferAlphaTestMDIPipelineHandle() const
{
  return m_gbufferAlphaTestMDIPipeline;
}

PipelineHandle RenderDevice::getForwardPipelineHandle() const
{
  return m_forwardPipeline;
}

PipelineHandle RenderDevice::getForwardMDIPipelineHandle() const
{
  return m_forwardMDIPipeline;
}

PipelineHandle RenderDevice::getShadowPipelineHandle() const
{
  return m_shadowPipeline;
}

PipelineHandle RenderDevice::getDebugPipelineHandle() const
{
  return m_debugPipeline;
}

PipelineHandle RenderDevice::getGPUCullingDebugPipelineHandle() const
{
  return m_gpuCullingDebugPipeline;
}

PipelineHandle RenderDevice::getCSMShadowPipelineHandle() const
{
  return m_csmShadowPipeline.isNull() ? m_shadowPipeline : m_csmShadowPipeline;
}

PipelineHandle RenderDevice::getShadowCullingPipelineHandle() const
{
  return m_shadowCullingPipeline;
}

PipelineHandle RenderDevice::getGPUCullingPipelineHandle() const
{
  return m_gpuCullingPipeline;
}

void RenderDevice::updateGPUCullingDepthPyramidArgumentTable(uint32_t frameIndex,
                                                            const rhi::TextureViewHandle* mipViews,
                                                            uint32_t mipCount)
{
  if(frameIndex >= m_device.gpuCullingArgumentTables.size()
     || m_device.gpuCullingArgumentTables[frameIndex].isNull()
     || mipViews == nullptr || mipCount == 0)
  {
    return;
  }

  std::array<rhi::ArgumentWrite, shaderio::LDepthPyramidMaxMips> writes{};
  for(uint32_t i = 0; i < static_cast<uint32_t>(writes.size()); ++i)
  {
    writes[i] = rhi::ArgumentWrite{
        .binding      = 5,
        .arrayElement = i,
        .type         = rhi::ArgumentType::sampledTexture,
        .textureView  = mipViews[std::min(i, mipCount - 1u)],
        .accessIntent = rhi::ArgumentAccessIntent::readWrite,
    };
  }
  m_device.device->updateArgumentTable(m_device.gpuCullingArgumentTables[frameIndex],
                                       static_cast<uint32_t>(writes.size()),
                                       writes.data());
}

uint64_t RenderDevice::getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].shadowCullingIndirectBuffer.buffer);
}

uint32_t RenderDevice::getShadowCullingMeshCapacity(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return m_perFrame.frameUserData[frameIndex].shadowCullingMeshCapacity;
}

rhi::TextureHandle RenderDevice::getCurrentSwapchainTextureHandle() const
{
  if(m_swapchainDependent.swapchain == nullptr || !m_swapchainDependent.hasAcquiredImage)
  {
    return {};
  }
  // Swapchain::currentTexture() is a swapchain-local synthetic handle that is NOT in
  // the device resource table the command buffer resolves against. Mirror the current
  // backbuffer's native image into that table instead, caching one handle per image
  // index and re-registering only when the native image changes (swapchain rebuild).
  const uint32_t idx    = m_swapchainDependent.currentImageIndex;
  const uint64_t native = m_swapchainDependent.swapchain->getBackendImageHandle(idx);
  if(native == 0)
  {
    return {};
  }
  auto& table = const_cast<rhi::vulkan::VulkanResourceTable&>(m_device.resourceTable);
  if(idx >= m_swapchainTextureHandles.size())
  {
    m_swapchainTextureHandles.resize(idx + 1);
    m_swapchainTextureNatives.resize(idx + 1, 0);
  }
  if(m_swapchainTextureNatives[idx] != native)
  {
    if(!m_swapchainTextureHandles[idx].isNull())
    {
      table.removeTexture(m_swapchainTextureHandles[idx]);
    }
    m_swapchainTextureHandles[idx] = table.registerTexture(native, 0, /*owned=*/false);
    m_swapchainTextureNatives[idx] = native;
  }
  return m_swapchainTextureHandles[idx];
}

rhi::TextureViewHandle RenderDevice::getOutputTextureView() const
{
  return m_swapchainDependent.sceneResources.getOutputTextureView();
}

rhi::TextureViewHandle RenderDevice::getShadowMapView() const
{
  return m_csmCascadeArrayViewHandle;
}

shaderio::ShadowUniforms* RenderDevice::getShadowUniformsData()
{
  return m_csmShadowResources.getShadowUniformsData();
}

RuntimeProfileSnapshot RenderDevice::getRuntimeProfileSnapshot() const
{
  RuntimeProfileSnapshot snapshot{};
  static constexpr size_t kMaxReasonableProfilePassCount = 256;
  const size_t safePassCount = std::min({
      m_passGpuProfile.passNames.size(),
      m_passGpuProfile.latestCpuPassDurationsMs.size(),
      m_passGpuProfile.latestPassDurationsMs.size(),
      kMaxReasonableProfilePassCount,
  });

  snapshot.passNames.assign(m_passGpuProfile.passNames.begin(),
                            m_passGpuProfile.passNames.begin() + static_cast<ptrdiff_t>(safePassCount));
  snapshot.cpuPassDurationsMs.assign(m_passGpuProfile.latestCpuPassDurationsMs.begin(),
                                     m_passGpuProfile.latestCpuPassDurationsMs.begin() + static_cast<ptrdiff_t>(safePassCount));
  snapshot.gpuPassDurationsMs.assign(m_passGpuProfile.latestPassDurationsMs.begin(),
                                     m_passGpuProfile.latestPassDurationsMs.begin() + static_cast<ptrdiff_t>(safePassCount));
  snapshot.gpuValid = m_passGpuProfile.latestValid;
  return snapshot;
}

void RenderDevice::beginUiFrame()
{
  ImGui_ImplVulkan_NewFrame();
#ifdef __ANDROID__
  ImGui_ImplAndroid_NewFrame();
#else
  ImGui_ImplGlfw_NewFrame();
#endif
  ImGui::NewFrame();
}

void RenderDevice::renderWithPassExecutor(const RenderParams& params, PassExecutor& passExecutor)
{

  m_presentPassActive = false;

  {
  demo::profiling::ScopedCpuRange rendererPreRecordRange("RendererPreRecord");

  {
    demo::profiling::ScopedCpuRange profileSetupRange("RendererPreRecord.ProfileSetup");
    if(m_passGpuProfile.passNames.size() != passExecutor.getPassCount())
    {
      createPassGpuProfileResources(passExecutor);
    }
    else
    {
      bool needsProfileRebuild = false;
      for(size_t passIndex = 0; passIndex < passExecutor.getPassCount(); ++passIndex)
      {
        const PassNode* pass = passExecutor.getPass(passIndex);
        const char* passName = pass != nullptr ? pass->getName() : "Unknown";
        if(m_passGpuProfile.passNames[passIndex] != passName)
        {
          needsProfileRebuild = true;
          break;
        }
      }
      if(needsProfileRebuild)
      {
        createPassGpuProfileResources(passExecutor);
      }
    }
  }

  {
    demo::profiling::ScopedCpuRange resizeCheckRange("RendererPreRecord.ResizeCheck");
    if(params.viewportSize.width > 0 && params.viewportSize.height > 0
       && (params.viewportSize.width != m_swapchainDependent.viewportSize.width
           || params.viewportSize.height != m_swapchainDependent.viewportSize.height))
    {
      resize(params.viewportSize);
    }
  }

  if(!prepareFrameResources())
    return;

  {
    demo::profiling::ScopedCpuRange cacheStatsRange("RendererPreRecord.CacheGPUCullingStats");
    cacheGPUCullingStats(m_perFrame.frameContext->getCurrentFrameIndex(), params.debugOptions.showGPUCullingOverlay);
  }
  }

  rhi::CommandBuffer* cmdBuffer = nullptr;
  {
    demo::profiling::ScopedCpuRange recordCommandBufferRange("RecordCommandBuffer");
    cmdBuffer = &beginCommandRecording();
    ASSERT(cmdBuffer != nullptr, "RenderDevice::renderWithPassExecutor requires a command buffer");
    drawFrame(*cmdBuffer, params, passExecutor);
  }
  ASSERT(cmdBuffer != nullptr, "RenderDevice::renderWithPassExecutor requires a command buffer");
  endFrame(*cmdBuffer);
}

// Pass execution wrappers (used by PassNode implementations)
void RenderDevice::executeImGuiPass(rhi::CommandBuffer& cmdBuffer, const RenderParams& params)
{
  if(!m_presentPassActive)
  {
    return;
  }

  if(params.debugOptions.showViewportAxis && params.cameraUniforms != nullptr)
  {
    ui::DrawAxisInRect(params.viewportImageRect, params.cameraUniforms->view);
  }

  if(params.debugOptions.showGPUCullingOverlay || params.debugOptions.showPassGpuProfile)
  {
    drawGPUInfoOverlay(params);
  }

  if(params.recordUi)
  {
    params.recordUi();
  }

  const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmdBuffer.getBackendHandle());
  if(vkCmd != VK_NULL_HANDLE)
  {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd);
  }
}

void RenderDevice::beginPresentPass(rhi::CommandBuffer& cmdBuffer)
{
  m_presentPassActive = false;

  // Move the swapchain image into a color-attachment layout for the UI pass via the
  // resource-barrier verb. The backend emits a conservative ALL_COMMANDS barrier; the
  // layout transition (General -> ColorAttachment) matches the previous explicit barrier.
  const rhi::TextureHandle swapchainHandle = getCurrentSwapchainTextureHandle();
  if(!swapchainHandle.isNull())
  {
    const rhi::TextureBarrier barrier{
        .texture = swapchainHandle,
        .before  = rhi::ResourceState::General,
        .after   = rhi::ResourceState::ColorAttachment,
    };
    cmdBuffer.resourceBarrier(&barrier, 1, nullptr, 0);
  }

  const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmdBuffer.getBackendHandle());
  if(vkCmd == VK_NULL_HANDLE)
  {
    return;
  }
  const VkExtent2D swapchainExtent = toVkExtent(m_swapchainDependent.windowSize);
  const VkRenderingAttachmentInfo colorAttachment{
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView   = fromNativeHandle<VkImageView>(
          m_swapchainDependent.swapchain->getBackendImageViewHandle(m_swapchainDependent.currentImageIndex)),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
  };
  const VkRenderingInfo renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{0, 0}, swapchainExtent},
      .layerCount           = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments    = &colorAttachment,
  };
  vkCmdBeginRendering(vkCmd, &renderingInfo);
  const VkViewport viewport{
      .x        = 0.0f,
      .y        = 0.0f,
      .width    = static_cast<float>(swapchainExtent.width),
      .height   = static_cast<float>(swapchainExtent.height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, swapchainExtent};
  vkCmdSetViewportWithCount(vkCmd, 1, &viewport);
  vkCmdSetScissorWithCount(vkCmd, 1, &scissor);
  m_presentPassActive = true;
}

void RenderDevice::endPresentPass(rhi::CommandBuffer& cmdBuffer)
{
  if(!m_presentPassActive)
  {
    return;
  }

  m_presentPassActive = false;
  const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmdBuffer.getBackendHandle());
  if(vkCmd != VK_NULL_HANDLE)
  {
    vkCmdEndRendering(vkCmd);
  }

  // Close the swapchain image layout explicitly at the end of the final UI pass.
  // This keeps presentation correctness local to the presentation path instead of
  // relying on PassExecutor's state inference after the pass graph has completed.
  const rhi::TextureHandle swapchainHandle = getCurrentSwapchainTextureHandle();
  if(swapchainHandle.isNull())
  {
    return;
  }

  // ColorAttachment -> Present via the resource-barrier verb (layout transition preserved;
  // backend emits the conservative ALL_COMMANDS sync used across the present path).
  const rhi::TextureBarrier barrier{
      .texture = swapchainHandle,
      .before  = rhi::ResourceState::ColorAttachment,
      .after   = rhi::ResourceState::Present,
  };
  cmdBuffer.resourceBarrier(&barrier, 1, nullptr, 0);
}

void RenderDevice::createTransientCommandPool()
{
  const rhi::QueueInfo          graphicsQueueInfo = m_device.device->getGraphicsQueue();
  const VkDevice                device            = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const VkCommandPoolCreateInfo commandPoolCreateInfo{
      .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = graphicsQueueInfo.familyIndex,
  };
  VK_CHECK(vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, &m_device.transientCmdPool));
  DBG_VK_NAME(m_device.transientCmdPool);
  // NOTE: uploadCmdPool has been sunk into VulkanDevice::init (UPL-02).
}

void RenderDevice::createFrameSubmission(uint32_t numFrames)
{
  const rhi::QueueInfo graphicsQueueInfo = m_device.device->getGraphicsQueue();
  const VkDevice       device            = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());

  m_perFrame.frameContext = std::make_unique<rhi::vulkan::VulkanFrameContext>();
  m_perFrame.frameContext->init(static_cast<void*>(device), graphicsQueueInfo.familyIndex, numFrames);
  m_perFrame.frameContext->setSwapchain(m_swapchainDependent.swapchain.get());
  static_cast<rhi::vulkan::VulkanDevice&>(*m_device.device)
      .setFrameContext(static_cast<rhi::vulkan::VulkanFrameContext*>(m_perFrame.frameContext.get()));
  // Inject the resource table so the new CommandBuffer facade (getCommandBuffer) can resolve RHI handles.
  static_cast<rhi::vulkan::VulkanFrameContext&>(*m_perFrame.frameContext).setResourceTable(&m_device.resourceTable);

  m_perFrame.frameUserData.resize(numFrames);
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    frameUserData.transientAllocator.init(*m_device.device, kPerFrameTransientAllocatorSize);
    frameUserData.lightingBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::LightingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    frameUserData.lightCullingBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::LightCullingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    frameUserData.gpuCullingUniformBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::GPUCullingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    // Wave 8: mirror the per-frame UBO buffers (and the stable transient allocator
    // buffer) as RHI BufferHandles for ArgumentWrite-based bind-group updates.
    rebindFrameBufferHandle(frameUserData.lightingBufferRHI, frameUserData.lightingBuffer);
    rebindFrameBufferHandle(frameUserData.lightCullingBufferRHI, frameUserData.lightCullingBuffer);
    rebindFrameBufferHandle(frameUserData.gpuCullingUniformBufferRHI, frameUserData.gpuCullingUniformBuffer);
    frameUserData.transientBufferRHI = frameUserData.transientAllocator.getBufferHandle();
  }

  m_perFrame.frameCounter = 1;

}

bool RenderDevice::prepareFrameResources()
{
  demo::profiling::ScopedCpuRange prepareFrameRange("RendererPreRecord.PrepareFrameResources");

  {
    demo::profiling::ScopedCpuRange rebuildSwapchainRange("PrepareFrameResources.RebuildSwapchainDependentResources");
    rebuildSwapchainDependentResources();
  }

  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  // Wait for the frame slot we are about to reuse. beginFrame()/submitFrame()
  // operate on the current slot, and endFrame() advances after submission.
  // Keeping the order as wait(current) -> begin(current) -> submit(current) ->
  // advance(next) preserves actual CPU/GPU overlap across the frame ring.
  {
    demo::profiling::ScopedCpuRange waitFrameRange("PrepareFrameResources.WaitForFrameCompletion");
    m_perFrame.frameContext->waitForFrameCompletion();
    static_cast<rhi::vulkan::VulkanDevice&>(*m_device.device)
        .processRetirements(m_perFrame.frameContext->getCurrentFrameValue());
  }

  {
    demo::profiling::ScopedCpuRange beginFrameRange("PrepareFrameResources.BeginFrame");
    m_perFrame.frameContext->beginFrame();
  }

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  ASSERT(currentFrameIndex < m_perFrame.frameUserData.size(), "Current frame index must map to frame user data");
  {
    demo::profiling::ScopedCpuRange syncMaterialRange("PrepareFrameResources.SyncMaterialArgumentTable");
    syncMaterialArgumentTable(currentFrameIndex);
  }

  {
    demo::profiling::ScopedCpuRange flushUploadsRange("PrepareFrameResources.FlushPendingUploads");
    flushPendingUploadCommands(false);
  }
  auto& frameUserData = m_perFrame.frameUserData[currentFrameIndex];
  {
    demo::profiling::ScopedCpuRange resolveProfileRange("PrepareFrameResources.ResolvePassGpuProfileResults");
    resolvePassGpuProfileResults(currentFrameIndex);
  }
  {
    demo::profiling::ScopedCpuRange resetTransientRange("PrepareFrameResources.ResetTransientAllocator");
    frameUserData.transientAllocator.reset();
    // This frame index's fence has been waited on, so last cycle's temporary bind
    // groups are idle and safe to recycle before new ones are created this frame.
    for(const rhi::ArgumentTableHandle handle : frameUserData.transientArgumentTables)
    {
      destroyArgumentTable(handle);
    }
    frameUserData.transientArgumentTables.clear();
  }
  m_swapchainDependent.hasAcquiredImage = false;

  return true;
}

bool RenderDevice::acquireSwapchainImageForPresent()
{

  ASSERT(m_swapchainDependent.swapchain != nullptr, "Swapchain must exist before late acquire");

  m_swapchainDependent.hasAcquiredImage = false;
  if(!acquireSwapchainImage(*m_swapchainDependent.swapchain, m_swapchainDependent.currentImageIndex))
  {
    if(m_swapchainDependent.swapchain->needsRebuild())
    {
      m_swapchainDependent.swapchain->requestRebuild();
    }
    return false;
  }

  if(m_swapchainDependent.currentImageIndex >= m_swapchainDependent.imageStates.size())
  {
    m_swapchainDependent.imageStates.resize(m_swapchainDependent.currentImageIndex + 1u, rhi::ResourceState::Undefined);
  }

  m_swapchainDependent.hasAcquiredImage = true;
  return true;
}

void RenderDevice::createIBLResources(rhi::CommandBuffer& cmd)
{
  destroyIBLResources();

  m_device.iblEnvironmentPath = kDefaultIBLEnvironmentPath;
  m_device.iblUsingFallback = true;
  m_device.iblEnvironmentStatus = "Using flat ambient fallback";

  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const VkPhysicalDevice physicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle());
  const std::filesystem::path environmentPath(kDefaultIBLEnvironmentPath);

  Ktx2Loader loader;
  Ktx2Loader::Ktx2Texture ktxTexture;
  LOGI("RenderDevice::createIBLResources: checking %s", environmentPath.string().c_str());
  if(!std::filesystem::exists(environmentPath))
  {
    m_device.iblEnvironmentStatus = "KTX2 environment not found: " + environmentPath.string();
    LOGW("%s", m_device.iblEnvironmentStatus.c_str());
    return;
  }
  if(!loader.load(environmentPath, ktxTexture))
  {
    m_device.iblEnvironmentStatus = "Failed to load KTX2 environment: " + loader.getLastError();
    LOGW("%s", m_device.iblEnvironmentStatus.c_str());
    return;
  }
  if(!supportsSampledImageFormat(physicalDevice, toNativeFormat(ktxTexture.format)))
  {
    m_device.iblEnvironmentStatus = "Unsupported IBL KTX2 format: " + std::string(string_VkFormat(toNativeFormat(ktxTexture.format)));
    LOGW("%s", m_device.iblEnvironmentStatus.c_str());
    return;
  }
  if(ktxTexture.width == 0 || ktxTexture.height == 0 || ktxTexture.data.empty())
  {
    m_device.iblEnvironmentStatus = "Invalid IBL KTX2 payload";
    LOGW("%s", m_device.iblEnvironmentStatus.c_str());
    return;
  }
  LOGI("RenderDevice::createIBLResources: loaded KTX2 payload");

  const uint32_t mipLevels = std::max(ktxTexture.mipLevels, 1u);
  const VkImageCreateInfo imageInfo{
      .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType   = VK_IMAGE_TYPE_2D,
      .format      = toNativeFormat(ktxTexture.format),
      .extent      = {ktxTexture.width, ktxTexture.height, 1},
      .mipLevels   = mipLevels,
      .arrayLayers = 1,
      .samples     = VK_SAMPLE_COUNT_1_BIT,
      .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
  };

  utils::Image environmentImage = createImage(m_device.allocator, imageInfo);
  LOGI("RenderDevice::createIBLResources: image created");
  const rhi::TextureHandle environmentHandle =
      m_device.device->registerExternalTexture(reinterpret_cast<uint64_t>(environmentImage.image));
  const rhi::TextureSubresourceRange environmentRange{
      .aspect = rhi::TextureAspect::color,
      .baseMipLevel = 0,
      .levelCount = mipLevels,
      .baseArrayLayer = 0,
      .layerCount = 1,
  };
  const rhi::TextureBarrier uploadBeginBarrier{
      .texture = environmentHandle,
      .before = rhi::ResourceState::Undefined,
      .after = rhi::ResourceState::TransferDst,
      .range = environmentRange,
  };
  cmd.resourceBarrier(&uploadBeginBarrier, 1, nullptr, 0);
  LOGI("RenderDevice::createIBLResources: image layout initialized");

  BatchUploadContext batchUpload;
  batchUpload.init(*m_device.device, static_cast<uint64_t>(ktxTexture.data.size()) + 16u);
  LOGI("RenderDevice::createIBLResources: upload context initialized");
  const BatchUploadContext::Slice slice = batchUpload.allocate(ktxTexture.data.size(), 16);
  std::memcpy(slice.cpuPtr, ktxTexture.data.data(), ktxTexture.data.size());

  for(uint32_t mip = 0; mip < mipLevels; ++mip)
  {
    const rhi::BufferTextureCopyDesc region{
        .bufferOffset = ktxTexture.mipOffsets[mip],
        .texture = environmentHandle,
        .aspect = rhi::TextureAspect::color,
        .mipLevel = mip,
        .baseArrayLayer = 0,
        .layerCount = 1,
        .width = std::max(ktxTexture.width >> mip, 1u),
        .height = std::max(ktxTexture.height >> mip, 1u),
        .depth = 1,
    };
    batchUpload.recordTextureUpload(slice, environmentHandle, region);
  }
  batchUpload.executeUploads(cmd);
  LOGI("RenderDevice::createIBLResources: upload recorded");

  const rhi::TextureBarrier uploadEndBarrier{
      .texture = environmentHandle,
      .before = rhi::ResourceState::TransferDst,
      .after = rhi::ResourceState::General,
      .range = environmentRange,
  };
  cmd.resourceBarrier(&uploadEndBarrier, 1, nullptr, 0);
  m_device.device->destroyImage(environmentHandle);

  rhi::BufferHandle batchStagingBuffer = batchUpload.releaseStagingBuffer();
  if(!batchStagingBuffer.isNull())
  {
    m_device.rhiStagingBuffers.push_back(batchStagingBuffer);
  }

  const VkImageViewCreateInfo viewInfo{
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image            = environmentImage.image,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = toNativeFormat(ktxTexture.format),
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = mipLevels, .layerCount = 1},
  };
  VkImageView environmentView = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(nativeDevice, &viewInfo, nullptr, &environmentView));
  LOGI("RenderDevice::createIBLResources: view created");

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();
  dutil.setObjectName(environmentImage.image, "IBL_EquirectEnvironment");
  dutil.setObjectName(environmentView, "IBL_EquirectEnvironmentView");

  m_device.iblEnvironment.image = environmentImage.image;
  m_device.iblEnvironment.allocation = environmentImage.allocation;
  m_device.iblEnvironment.view = environmentView;
  m_device.iblEnvironment.layout = VK_IMAGE_LAYOUT_GENERAL;
  m_device.iblEnvironmentFormat = ktxTexture.format;  // already rhi::TextureFormat post-02-02
  m_device.iblEnvironmentExtent = {ktxTexture.width, ktxTexture.height};
  m_device.iblEnvironmentMipCount = mipLevels;
  m_device.iblEnvironmentEstimatedBytes = static_cast<uint64_t>(ktxTexture.data.size());
  m_device.iblEnvironmentLoaded = true;
  m_device.iblUsingFallback = false;
  m_device.iblEnvironmentStatus = "Loaded equirect HDR KTX2";
  LOGI("Loaded IBL environment %s (%ux%u, %u mips, %s, %.2f MiB)",
       environmentPath.string().c_str(),
       ktxTexture.width,
       ktxTexture.height,
       mipLevels,
       string_VkFormat(toNativeFormat(ktxTexture.format)),
       static_cast<double>(ktxTexture.data.size()) / (1024.0 * 1024.0));
}

void RenderDevice::destroyIBLResources()
{
  const VkDevice nativeDevice = m_device.device
                                    ? fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle())
                                    : VK_NULL_HANDLE;
  if(m_device.iblEnvironment.view != VK_NULL_HANDLE || m_device.iblEnvironment.image != VK_NULL_HANDLE)
  {
    destroyImageResource(nativeDevice, m_device.allocator, m_device.iblEnvironment);
  }
  m_device.iblEnvironmentFormat = rhi::TextureFormat::undefined;
  m_device.iblEnvironmentExtent = {};
  m_device.iblEnvironmentMipCount = 0;
  m_device.iblEnvironmentEstimatedBytes = 0;
  m_device.iblEnvironmentLoaded = false;
  m_device.iblUsingFallback = true;
}

void RenderDevice::updateLightingUniformBuffer(uint32_t frameIndex, const shaderio::LightingUniforms& lightingUniforms)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  writeHostVisibleBuffer(m_device.allocator, frameUserData.lightingBuffer, &lightingUniforms, sizeof(lightingUniforms));
}

void RenderDevice::updateLightCullingUniformBuffer(uint32_t frameIndex, const shaderio::LightCullingUniforms& cullingUniforms)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  writeHostVisibleBuffer(m_device.allocator, frameUserData.lightCullingBuffer, &cullingUniforms, sizeof(cullingUniforms));
}

void RenderDevice::waitForAllFrameSlots()
{
  if(m_perFrame.frameContext == nullptr)
  {
    return;
  }

  const uint32_t frameCount = m_perFrame.frameContext->getFrameCount();
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    m_perFrame.frameContext->waitForFrame(i);
  }
}

void RenderDevice::ensureGPUCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount)
{
  const uint32_t requiredCapacity = std::max(requiredMeshCount, 1u);
  if(frameUserData.gpuCullingMeshCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.gpuCullingObjectBuffer.buffer != VK_NULL_HANDLE || frameUserData.gpuCullingIndirectBuffer.buffer != VK_NULL_HANDLE
     || frameUserData.gpuCullingDrawCountBuffer.buffer != VK_NULL_HANDLE
     || frameUserData.gpuCullingStatsBuffer.buffer != VK_NULL_HANDLE || frameUserData.gpuCullingResultBuffer.buffer != VK_NULL_HANDLE)
  {
    // These per-frame buffers are consumed by submitted graphics work and some
    // passes read the previous frame's culling outputs. When scene switches force
    // a capacity jump, retire the whole frame ring before destroying buffers.
    waitForAllFrameSlots();
  }
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingObjectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingIndirectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingDrawCountBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingStatsBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingResultBuffer);

  frameUserData.gpuCullingObjectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullObject) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingIndirectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullIndirectCommand) * requiredCapacity * 4u,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_GPU_ONLY);
  frameUserData.gpuCullingDrawCountBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullDrawCounts),
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingStatsBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullStats),
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingResultBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(uint32_t) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingMeshCapacity = requiredCapacity;
  frameUserData.gpuCullingResults.resize(requiredCapacity, shaderio::LGPUCullResultVisible);
  frameUserData.gpuCullingScratchObjects.resize(requiredCapacity);

  rebindFrameBufferHandle(frameUserData.gpuCullingIndirectBufferRHI, frameUserData.gpuCullingIndirectBuffer);
  rebindFrameBufferHandle(frameUserData.gpuCullingDrawCountBufferRHI, frameUserData.gpuCullingDrawCountBuffer);
  rebindFrameBufferHandle(frameUserData.gpuCullingObjectBufferRHI, frameUserData.gpuCullingObjectBuffer);
  rebindFrameBufferHandle(frameUserData.gpuCullingStatsBufferRHI, frameUserData.gpuCullingStatsBuffer);
  rebindFrameBufferHandle(frameUserData.gpuCullingResultBufferRHI, frameUserData.gpuCullingResultBuffer);
}

void RenderDevice::updateGPUCullingBuffers(uint32_t frameIndex, const RenderParams& params)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  const bool useExternalPersistentObjects =
      params.gpuDrivenSceneView != nullptr
      && params.gpuDrivenSceneView->usePersistentCullingObjects
      && params.gpuDrivenSceneView->gpuCullObjectBuffer != 0
      && params.gpuDrivenSceneView->objectCount > 0;
  const uint32_t objectCount = useExternalPersistentObjects
                                   ? params.gpuDrivenSceneView->objectCount
                                   : (params.gltfModel != nullptr ? static_cast<uint32_t>(params.gltfModel->meshes.size()) : 0u);

  ensureGPUCullingBuffers(frameUserData, objectCount);

  if(frameUserData.gpuCullingObjectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingIndirectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingDrawCountBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingStatsBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  frameUserData.useExternalGPUCullingObjectBuffer = useExternalPersistentObjects;
  frameUserData.externalGPUCullingObjectBuffer =
      useExternalPersistentObjects
          ? reinterpret_cast<VkBuffer>(params.gpuDrivenSceneView->gpuCullObjectBuffer)
          : VK_NULL_HANDLE;
  frameUserData.externalGPUCullingMeshletBuffer =
      useExternalPersistentObjects
          ? reinterpret_cast<VkBuffer>(params.gpuDrivenSceneView->gpuCullMeshletBuffer)
          : VK_NULL_HANDLE;
  frameUserData.externalGPUCullingSceneObjectBuffer =
      useExternalPersistentObjects
          ? reinterpret_cast<VkBuffer>(params.gpuDrivenSceneView->gpuCullSceneObjectBuffer)
          : VK_NULL_HANDLE;
  frameUserData.externalGPUCullingObjectBufferAddress =
      useExternalPersistentObjects ? params.gpuDrivenSceneView->gpuCullObjectBufferAddress : 0;
  frameUserData.useExternalGPUCullingMeshletData =
      useExternalPersistentObjects
      && frameUserData.externalGPUCullingMeshletBuffer != VK_NULL_HANDLE
      && frameUserData.externalGPUCullingSceneObjectBuffer != VK_NULL_HANDLE;
  rebindFrameBufferHandle(frameUserData.externalGPUCullingObjectBufferRHI,
                          frameUserData.externalGPUCullingObjectBuffer);
  rebindFrameBufferHandle(frameUserData.externalGPUCullingMeshletBufferRHI,
                          frameUserData.externalGPUCullingMeshletBuffer);
  rebindFrameBufferHandle(frameUserData.externalGPUCullingSceneObjectBufferRHI,
                          frameUserData.externalGPUCullingSceneObjectBuffer);
  frameUserData.gpuCullingSourceModel = useExternalPersistentObjects ? nullptr : params.gltfModel;
  frameUserData.gpuCullingObjectCount = objectCount;
  m_externalGPUCullingOverlayObjects =
      useExternalPersistentObjects ? params.gpuDrivenSceneView->overlayObjects : nullptr;
  m_externalGPUCullingOverlayObjectCount =
      useExternalPersistentObjects ? params.gpuDrivenSceneView->overlayObjectCount : 0;
  updateGPUCullingArgumentTable(frameIndex);

  frameUserData.gpuCullingScratchObjects.resize(objectCount);
  auto& objects = frameUserData.gpuCullingScratchObjects;
  std::fill(objects.begin(), objects.end(), shaderio::GPUCullObject{});
  if(!useExternalPersistentObjects && params.gltfModel != nullptr)
  {
    for(uint32_t meshIndex = 0; meshIndex < objectCount; ++meshIndex)
    {
      const MeshRecord* meshRecord = m_meshPool.tryGet(params.gltfModel->meshes[meshIndex]);
      if(meshRecord == nullptr)
      {
        continue;
      }

      // Use pre-computed alphaMode from mesh
      uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
      if(meshRecord->alphaMode == shaderio::LAlphaBlend)
      {
        flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
      }
      else if(meshRecord->alphaMode == shaderio::LAlphaMask)
      {
        flags |= shaderio::LGPUCullFlagAlphaMask;
      }

      objects[meshIndex] = shaderio::GPUCullObject{
          .sphereCenterRadius = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius),
          .indexCount         = meshRecord->indexCount,
          .firstIndex         = meshRecord->firstIndex,
          .vertexOffset       = meshRecord->vertexOffset,
          .flags              = flags,
      };
    }
  }

  if(!useExternalPersistentObjects && !objects.empty())
  {
    writeHostVisibleBuffer(m_device.allocator, frameUserData.gpuCullingObjectBuffer, objects.data(),
                           sizeof(shaderio::GPUCullObject) * objects.size());
  }

  const shaderio::GPUCullStats zeroStats{};
  writeHostVisibleBuffer(m_device.allocator, frameUserData.gpuCullingStatsBuffer, &zeroStats, sizeof(zeroStats));
  const shaderio::GPUCullDrawCounts zeroDrawCounts{};
  writeHostVisibleBuffer(m_device.allocator,
                         frameUserData.gpuCullingDrawCountBuffer,
                         &zeroDrawCounts,
                         sizeof(zeroDrawCounts));

  const shaderio::GPUCullingUniforms uniforms = buildGPUCullingUniforms(params, objectCount);
  writeHostVisibleBuffer(m_device.allocator, frameUserData.gpuCullingUniformBuffer, &uniforms, sizeof(uniforms));
}

void RenderDevice::ensureShadowCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount)
{
  const uint32_t requiredCapacity = std::max(requiredMeshCount, 1u);
  if(frameUserData.shadowCullingMeshCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.shadowCullingObjectBuffer.buffer != VK_NULL_HANDLE
     || frameUserData.shadowCullingIndirectBuffer.buffer != VK_NULL_HANDLE
     || frameUserData.shadowCullingDrawDataBuffer.buffer != VK_NULL_HANDLE)
  {
    // Mirror the GPU culling rule: shadow culling buffers participate in
    // submitted work across the frame ring, so expand them only after all
    // frame slots have retired.
    waitForAllFrameSlots();
  }
  destroyBuffer(m_device.allocator, frameUserData.shadowCullingObjectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.shadowCullingIndirectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.shadowCullingDrawDataBuffer);

  frameUserData.shadowCullingObjectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::ShadowCullObject) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.shadowCullingIndirectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullIndirectCommand) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.shadowCullingDrawDataBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::DrawUniforms) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.shadowCullingMeshCapacity = requiredCapacity;
  frameUserData.shadowCullingScratchObjects.resize(requiredCapacity);
  frameUserData.shadowCullingScratchDrawData.resize(requiredCapacity);
  rebindFrameBufferHandle(frameUserData.shadowCullingObjectBufferRHI, frameUserData.shadowCullingObjectBuffer);
  rebindFrameBufferHandle(frameUserData.shadowCullingIndirectBufferRHI, frameUserData.shadowCullingIndirectBuffer);
  rebindFrameBufferHandle(frameUserData.shadowCullingDrawDataBufferRHI, frameUserData.shadowCullingDrawDataBuffer);
  const uint32_t frameIndex = static_cast<uint32_t>(&frameUserData - m_perFrame.frameUserData.data());
  updateShadowCullingArgumentTable(frameIndex);
  updateShadowCullingDrawDataArgumentTable(frameIndex);
}

void RenderDevice::updateShadowCullingArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || frameIndex >= m_device.shadowCullingArgumentTables.size()
     || m_device.shadowCullingArgumentTables[frameIndex].isNull())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.shadowCullingObjectBufferRHI.isNull() || frameUserData.shadowCullingIndirectBufferRHI.isNull())
  {
    return;
  }

  const std::array<rhi::ArgumentWrite, 2> writes{{
      rhi::ArgumentWrite{.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.shadowCullingObjectBufferRHI},
      rhi::ArgumentWrite{.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.shadowCullingIndirectBufferRHI},
  }};
  m_device.device->updateArgumentTable(m_device.shadowCullingArgumentTables[frameIndex],
                                       static_cast<uint32_t>(writes.size()), writes.data());
}

void RenderDevice::updateShadowCullingDrawDataArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  rhi::BufferHandle drawDataBuffer = frameUserData.shadowCullingDrawDataBufferRHI;
  if(drawDataBuffer.isNull())
  {
    drawDataBuffer = frameUserData.transientBufferRHI;
  }
  if(drawDataBuffer.isNull())
  {
    return;
  }

  const rhi::ArgumentWrite drawDataWrite{
      .binding = shaderio::LBindDrawModelMdi,
      .type    = rhi::ArgumentType::storageBuffer,
      .buffer  = drawDataBuffer,
      .offset  = 0,
      .size    = 0,
  };
  for(uint32_t cascadeIndex = 0; cascadeIndex < shaderio::LCascadeCount; ++cascadeIndex)
  {
    const rhi::ArgumentTableHandle drawArgumentTableHandle = getCSMShadowMDIDrawArgumentTable(frameIndex, cascadeIndex);
    if(drawArgumentTableHandle.isNull())
    {
      continue;
    }
    updateArgumentTable(drawArgumentTableHandle, &drawDataWrite, 1);
  }
}

void RenderDevice::ensureGBufferMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount)
{
  const uint32_t requiredCapacity = std::max(requiredDrawCount, 1u);
  if(frameUserData.gbufferMdiDrawCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.gbufferMdiDrawDataBuffer.buffer != VK_NULL_HANDLE)
  {
    waitForAllFrameSlots();
  }

  destroyBuffer(m_device.allocator, frameUserData.gbufferMdiDrawDataBuffer);
  frameUserData.gbufferMdiDrawDataBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::DrawUniforms) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gbufferMdiDrawCapacity = requiredCapacity;
  rebindFrameBufferHandle(frameUserData.gbufferMdiDrawDataBufferRHI, frameUserData.gbufferMdiDrawDataBuffer);

  const uint32_t frameIndex = static_cast<uint32_t>(&frameUserData - m_perFrame.frameUserData.data());
  updateGBufferMdiDrawDataArgumentTable(frameIndex);
}

void RenderDevice::ensureMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount)
{
  const uint32_t requiredCapacity = std::max(requiredDrawCount, 1u);
  if(frameUserData.mdiDrawCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.mdiDrawDataBuffer.buffer != VK_NULL_HANDLE)
  {
    waitForAllFrameSlots();
  }

  destroyBuffer(m_device.allocator, frameUserData.mdiDrawDataBuffer);
  frameUserData.mdiDrawDataBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::DrawUniforms) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.mdiDrawCapacity = requiredCapacity;
  rebindFrameBufferHandle(frameUserData.mdiDrawDataBufferRHI, frameUserData.mdiDrawDataBuffer);

  const uint32_t frameIndex = static_cast<uint32_t>(&frameUserData - m_perFrame.frameUserData.data());
  updateMdiDrawDataArgumentTable(frameIndex);
}

void RenderDevice::ensureDepthMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount)
{
  const uint32_t requiredCapacity = std::max(requiredDrawCount, 1u);
  if(frameUserData.depthMdiDrawCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.depthMdiDrawDataBuffer.buffer != VK_NULL_HANDLE)
  {
    waitForAllFrameSlots();
  }

  destroyBuffer(m_device.allocator, frameUserData.depthMdiDrawDataBuffer);
  frameUserData.depthMdiDrawDataBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::DrawUniforms) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.depthMdiDrawCapacity = requiredCapacity;
  rebindFrameBufferHandle(frameUserData.depthMdiDrawDataBufferRHI, frameUserData.depthMdiDrawDataBuffer);

  const uint32_t frameIndex = static_cast<uint32_t>(&frameUserData - m_perFrame.frameUserData.data());
  updateDepthMdiDrawDataArgumentTable(frameIndex);
}

void RenderDevice::ensureGPUDrivenPersistentIndirectStreamBuffer(PerFrameResources::FrameUserData& frameUserData,
                                                             uint32_t                          requiredDrawCount)
{
  const uint32_t requiredCapacity = std::max(requiredDrawCount, 1u);
  if(frameUserData.gpuDrivenPersistentIndirectStreamCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  if(frameUserData.gpuDrivenPersistentIndirectStreamBuffer.buffer != VK_NULL_HANDLE)
  {
    waitForAllFrameSlots();
  }

  destroyBuffer(m_device.allocator, frameUserData.gpuDrivenPersistentIndirectStreamBuffer);
  frameUserData.gpuDrivenPersistentIndirectStreamBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullIndirectCommand) * requiredCapacity,
                   VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_GPU_ONLY);
  frameUserData.gpuDrivenPersistentIndirectStreamCapacity = requiredCapacity;
  rebindFrameBufferHandle(frameUserData.gpuDrivenPersistentIndirectStreamBufferRHI,
                          frameUserData.gpuDrivenPersistentIndirectStreamBuffer);
}

void RenderDevice::updateGBufferMdiDrawDataArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.gbufferMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const rhi::ArgumentTableHandle drawArgumentTableHandle = getGBufferMDIDrawArgumentTable(frameIndex);
  if(drawArgumentTableHandle.isNull())
  {
    return;
  }

  const rhi::ArgumentWrite drawDataWrite{
      .binding = shaderio::LBindDrawModelMdi,
      .type    = rhi::ArgumentType::storageBuffer,
      .buffer  = frameUserData.gbufferMdiDrawDataBufferRHI,
      .offset  = 0,
      .size    = 0,
  };
  updateArgumentTable(drawArgumentTableHandle, &drawDataWrite, 1);
}

void RenderDevice::updateMdiDrawDataArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.mdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const rhi::ArgumentTableHandle drawArgumentTableHandle = getMDIDrawArgumentTable(frameIndex);
  if(drawArgumentTableHandle.isNull())
  {
    return;
  }

  const rhi::ArgumentWrite drawDataWrite{
      .binding = shaderio::LBindDrawModelMdi,
      .type    = rhi::ArgumentType::storageBuffer,
      .buffer  = frameUserData.mdiDrawDataBufferRHI,
      .offset  = 0,
      .size    = 0,
  };
  updateArgumentTable(drawArgumentTableHandle, &drawDataWrite, 1);
}

void RenderDevice::updateDepthMdiDrawDataArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.depthMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const rhi::ArgumentTableHandle drawArgumentTableHandle = getDepthMDIDrawArgumentTable(frameIndex);
  if(drawArgumentTableHandle.isNull())
  {
    return;
  }

  const rhi::ArgumentWrite drawDataWrite{
      .binding = shaderio::LBindDrawModelMdi,
      .type    = rhi::ArgumentType::storageBuffer,
      .buffer  = frameUserData.depthMdiDrawDataBufferRHI,
      .offset  = 0,
      .size    = 0,
  };
  updateArgumentTable(drawArgumentTableHandle, &drawDataWrite, 1);
}

void RenderDevice::updateShadowCullingBuffers(uint32_t frameIndex, const RenderParams& params)
{
  struct ShadowCullingSource
  {
    const MeshHandle* meshHandles{nullptr};
    uint32_t meshHandleCount{0};
    const ShadowPackedMesh* shadowPackedMeshes{nullptr};
    uint32_t shadowPackedMeshCount{0};
  };

  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ShadowCullingSource source{};
  if(params.gpuDrivenSceneView != nullptr)
  {
    if(params.gpuDrivenSceneView->meshHandles == nullptr || params.gpuDrivenSceneView->shadowPackedMeshes == nullptr)
    {
      return;
    }

    source.meshHandles = params.gpuDrivenSceneView->meshHandles;
    source.meshHandleCount = params.gpuDrivenSceneView->meshHandleCount;
    source.shadowPackedMeshes = params.gpuDrivenSceneView->shadowPackedMeshes;
    source.shadowPackedMeshCount = params.gpuDrivenSceneView->shadowPackedMeshCount;
  }
  else if(params.gltfModel != nullptr)
  {
    source.meshHandles = params.gltfModel->meshes.data();
    source.meshHandleCount = static_cast<uint32_t>(params.gltfModel->meshes.size());
    source.shadowPackedMeshes = params.gltfModel->shadowPackedMeshes.data();
    source.shadowPackedMeshCount = static_cast<uint32_t>(params.gltfModel->shadowPackedMeshes.size());
  }

  const uint32_t objectCount = source.shadowPackedMeshCount;

  ensureShadowCullingBuffers(frameUserData, objectCount);

  if(frameUserData.shadowCullingObjectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.shadowCullingDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  if(objectCount == 0 || source.meshHandles == nullptr || source.shadowPackedMeshes == nullptr)
  {
    return;
  }

  frameUserData.shadowCullingScratchObjects.resize(objectCount);
  frameUserData.shadowCullingScratchDrawData.resize(objectCount);
  auto& objects = frameUserData.shadowCullingScratchObjects;
  auto& drawData = frameUserData.shadowCullingScratchDrawData;
  std::vector<shaderio::GPUCullIndirectCommand> bootstrapCommands(objectCount, shaderio::GPUCullIndirectCommand{});
  std::fill(objects.begin(), objects.end(), shaderio::ShadowCullObject{});
  std::fill(drawData.begin(), drawData.end(), shaderio::DrawUniforms{});

  for(uint32_t meshIndex = 0; meshIndex < objectCount; ++meshIndex)
  {
    const ShadowPackedMesh& packedMesh = source.shadowPackedMeshes[meshIndex];
    if(packedMesh.meshIndex >= source.meshHandleCount)
    {
      continue;
    }

    const MeshRecord* meshRecord = m_meshPool.tryGet(source.meshHandles[packedMesh.meshIndex]);
    if(meshRecord == nullptr)
    {
      continue;
    }

    const glm::vec4 casterBounds = packedMesh.boundsSphere.w > 0.0f
        ? packedMesh.boundsSphere
        : glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
    objects[meshIndex] = shaderio::ShadowCullObject{
        .sphereCenterRadius = casterBounds,
        .indexCount         = packedMesh.indexCount,
        .firstIndex         = packedMesh.firstIndex,
        .vertexOffset       = packedMesh.vertexOffset,
        .firstInstance      = meshIndex,
    };
    bootstrapCommands[meshIndex] = shaderio::GPUCullIndirectCommand{
        .indexCount    = packedMesh.indexCount,
        .instanceCount = 1u,
        .firstIndex    = packedMesh.firstIndex,
        .vertexOffset  = packedMesh.vertexOffset,
        .firstInstance = meshIndex,
    };
    drawData[meshIndex] = packedMesh.boundsSphere.w > 0.0f ? packedMesh.drawData : buildShadowDrawUniforms(*meshRecord);
  }

  if(objectCount > 0)
  {
    writeHostVisibleBuffer(m_device.allocator, frameUserData.shadowCullingObjectBuffer, objects.data(),
                           sizeof(shaderio::ShadowCullObject) * objectCount);
    writeHostVisibleBuffer(m_device.allocator, frameUserData.shadowCullingIndirectBuffer, bootstrapCommands.data(),
                           sizeof(shaderio::GPUCullIndirectCommand) * objectCount);
    writeHostVisibleBuffer(m_device.allocator, frameUserData.shadowCullingDrawDataBuffer, drawData.data(),
                           sizeof(shaderio::DrawUniforms) * objectCount);
  }
}

void RenderDevice::cacheGPUCullingStats(uint32_t frameIndex, bool readOverlayObjects)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.gpuCullingStatsBuffer.allocation == nullptr || frameUserData.gpuCullingStatsBuffer.mapped == nullptr)
  {
    return;
  }

  {
    VK_CHECK(vmaInvalidateAllocation(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation, 0, sizeof(shaderio::GPUCullStats)));
    std::memcpy(&m_lastGPUCullingStats, frameUserData.gpuCullingStatsBuffer.mapped, sizeof(m_lastGPUCullingStats));
  }
  m_lastGPUCullingDrawCounts = {};
  if(frameUserData.gpuCullingDrawCountBuffer.allocation != nullptr && frameUserData.gpuCullingDrawCountBuffer.mapped != nullptr)
  {
    VK_CHECK(vmaInvalidateAllocation(m_device.allocator,
                                     frameUserData.gpuCullingDrawCountBuffer.allocation,
                                     0,
                                     sizeof(shaderio::GPUCullDrawCounts)));
    std::memcpy(&m_lastGPUCullingDrawCounts,
                frameUserData.gpuCullingDrawCountBuffer.mapped,
                sizeof(m_lastGPUCullingDrawCounts));
  }

  m_lastGPUCullingOverlayObjects.clear();
  if(!readOverlayObjects)
  {
    return;
  }
  if(frameUserData.gpuCullingResultBuffer.allocation == nullptr || frameUserData.gpuCullingResultBuffer.mapped == nullptr
     || frameUserData.gpuCullingResults.empty())
  {
    return;
  }

  {
    VK_CHECK(vmaInvalidateAllocation(m_device.allocator,
                                     frameUserData.gpuCullingResultBuffer.allocation,
                                     0,
                                     sizeof(uint32_t) * frameUserData.gpuCullingResults.size()));
    std::memcpy(frameUserData.gpuCullingResults.data(),
                frameUserData.gpuCullingResultBuffer.mapped,
                sizeof(uint32_t) * frameUserData.gpuCullingResults.size());
  }

  const size_t objectCount = std::min<size_t>(m_lastGPUCullingStats.totalCount, frameUserData.gpuCullingResults.size());
  m_lastGPUCullingOverlayObjects.reserve(objectCount);
  if(objectCount == 0)
  {
    return;
  }

  const shaderio::GPUCullObject* objectData = nullptr;
  if(frameUserData.useExternalGPUCullingObjectBuffer)
  {
    objectData = m_externalGPUCullingOverlayObjects;
    if(objectData == nullptr)
    {
      return;
    }
  }
  else
  {
    if(frameUserData.gpuCullingObjectBuffer.allocation == nullptr || frameUserData.gpuCullingObjectBuffer.mapped == nullptr)
    {
      return;
    }

    {
      VK_CHECK(vmaInvalidateAllocation(m_device.allocator,
                                       frameUserData.gpuCullingObjectBuffer.allocation,
                                       0,
                                       sizeof(shaderio::GPUCullObject) * objectCount));
    }
    objectData = static_cast<const shaderio::GPUCullObject*>(frameUserData.gpuCullingObjectBuffer.mapped);
  }

  const size_t safeObjectCount = frameUserData.useExternalGPUCullingObjectBuffer
                                     ? std::min<size_t>(objectCount, m_externalGPUCullingOverlayObjectCount)
                                     : objectCount;
  {
    for(size_t objectIndex = 0; objectIndex < safeObjectCount; ++objectIndex)
    {
      const shaderio::GPUCullObject& object = objectData[objectIndex];
      if(object.indexCount == 0u && object.sphereCenterRadius.w == 0.0f)
      {
        continue;
      }

      m_lastGPUCullingOverlayObjects.push_back(GPUCullOverlayObject{
          .center = glm::vec3(object.sphereCenterRadius),
          .radius = object.sphereCenterRadius.w,
          .flags  = object.flags,
          .result = frameUserData.gpuCullingResults[objectIndex],
      });
    }
  }
}

void RenderDevice::drawGPUCullingOverlay(const RenderParams& params) const
{
  ImGui::Text("Visible %u / %u", m_lastGPUCullingStats.visibleCount, m_lastGPUCullingStats.totalCount);
  ImGui::Text("Opaque %u / %u", m_lastGPUCullingStats.opaqueVisibleCount, m_lastGPUCullingStats.opaqueCount);
  ImGui::Text("Transparent %u / %u",
              m_lastGPUCullingStats.transparentVisibleCount,
              m_lastGPUCullingStats.transparentCount);
  ImGui::Text("Frustum Culled %u", m_lastGPUCullingStats.frustumCulledCount);
  ImGui::Text("Occlusion Culled %u", m_lastGPUCullingStats.occlusionCulledCount);
  ImGui::Text("Hi-Z Tested %u / %u",
              m_lastGPUCullingStats.hizTestedCount,
              m_lastGPUCullingStats.hizCandidateCount);
  ImGui::Text("Meshlet Cone Culled %u", m_lastGPUCullingStats.meshletConeCulledCount);

  ImGui::SeparatorText("Legend");
  const struct LegendEntry
  {
    const char* label;
    ImU32       color;
  } legends[] = {
      {"Visible Opaque", IM_COL32(92, 220, 120, 210)},
      {"Visible Transparent", IM_COL32(92, 210, 255, 210)},
      {"Frustum Culled", IM_COL32(255, 92, 92, 210)},
      {"Occlusion Culled", IM_COL32(255, 176, 92, 210)},
      {"Cone Culled", IM_COL32(184, 122, 255, 210)},
  };

  for(const LegendEntry& entry : legends)
  {
    ImGui::ColorButton(entry.label,
                       ImGui::ColorConvertU32ToFloat4(entry.color),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(12.0f, 12.0f));
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.label);
  }
}

void RenderDevice::createPassGpuProfileResources(const PassExecutor& passExecutor)
{
  destroyPassGpuProfileResources();

  if(m_device.device == nullptr)
  {
    return;
  }

  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle()), &deviceProperties2);

  m_passGpuProfile.timestampPeriodNs = deviceProperties2.properties.limits.timestampPeriod;
  m_passGpuProfile.queryCount = static_cast<uint32_t>(passExecutor.getPassCount() * 2);
  m_passGpuProfile.passNames.clear();
  m_passGpuProfile.latestCpuPassDurationsMs.clear();
  m_passGpuProfile.latestPassDurationsMs.clear();
  m_passGpuProfile.latestValid = false;
  m_passGpuProfile.frames.clear();
  m_passGpuProfile.currentCpuPassStartNs.clear();

  if(m_passGpuProfile.queryCount == 0 || m_perFrame.frameUserData.empty())
  {
    return;
  }

  m_passGpuProfile.passNames.reserve(passExecutor.getPassCount());
  for(size_t passIndex = 0; passIndex < passExecutor.getPassCount(); ++passIndex)
  {
    const PassNode* pass = passExecutor.getPass(passIndex);
    m_passGpuProfile.passNames.push_back(pass != nullptr ? pass->getName() : "Unknown");
  }
  m_passGpuProfile.latestCpuPassDurationsMs.assign(passExecutor.getPassCount(), 0.0);
  m_passGpuProfile.latestPassDurationsMs.assign(passExecutor.getPassCount(), 0.0);
  m_passGpuProfile.currentCpuPassStartNs.assign(passExecutor.getPassCount(), 0ull);

  m_passGpuProfile.frames.resize(m_perFrame.frameUserData.size());
  for(PassGpuProfileFrame& frame : m_passGpuProfile.frames)
  {
    frame.queryPool = m_device.device->createQueryPool(m_passGpuProfile.queryCount);
    frame.cpuPassDurationsMs.assign(passExecutor.getPassCount(), 0.0);
    frame.passDurationsMs.assign(passExecutor.getPassCount(), 0.0);
    frame.valid = false;
    frame.hasRecordedQueries = false;
  }
}

void RenderDevice::destroyPassGpuProfileResources()
{
  if(m_device.device != nullptr)
  {
    for(PassGpuProfileFrame& frame : m_passGpuProfile.frames)
    {
      if(!frame.queryPool.isNull())
      {
        m_device.device->destroyQueryPool(frame.queryPool);
        frame.queryPool = {};
      }
      frame.cpuPassDurationsMs.clear();
      frame.passDurationsMs.clear();
      frame.valid = false;
      frame.hasRecordedQueries = false;
    }
  }

  m_passGpuProfile.frames.clear();
  m_passGpuProfile.passNames.clear();
  m_passGpuProfile.latestCpuPassDurationsMs.clear();
  m_passGpuProfile.latestPassDurationsMs.clear();
  m_passGpuProfile.latestValid = false;
  m_passGpuProfile.queryCount = 0;
  m_passGpuProfile.timestampPeriodNs = 0.0f;
  m_passGpuProfile.currentCpuPassStartNs.clear();
}

void RenderDevice::resolvePassGpuProfileResults(uint32_t frameIndex)
{
  if(frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  PassGpuProfileFrame& frame = m_passGpuProfile.frames[frameIndex];
  if(frame.queryPool.isNull() || !frame.hasRecordedQueries)
  {
    return;
  }

  std::vector<uint64_t> queryData(static_cast<size_t>(m_passGpuProfile.queryCount) * 2u, 0ull);
  if(!m_device.device->getQueryPoolResultsWithAvailability(frame.queryPool, 0, m_passGpuProfile.queryCount, queryData.data()))
  {
    frame.valid = false;
    return;
  }

  bool anyValidPass = false;
  for(size_t passIndex = 0; passIndex < frame.passDurationsMs.size(); ++passIndex)
  {
    const size_t beginIndex = passIndex * 4u;
    const size_t endIndex = beginIndex + 2u;
    const bool beginAvailable = queryData[beginIndex + 1u] != 0ull;
    const bool endAvailable = queryData[endIndex + 1u] != 0ull;
    if(!beginAvailable || !endAvailable || queryData[endIndex] < queryData[beginIndex])
    {
      frame.passDurationsMs[passIndex] = 0.0;
      continue;
    }

    const uint64_t delta = queryData[endIndex] - queryData[beginIndex];
    frame.passDurationsMs[passIndex] = static_cast<double>(delta) * static_cast<double>(m_passGpuProfile.timestampPeriodNs) * 1e-6;
    anyValidPass = true;
  }

  frame.valid = anyValidPass;
  if(m_passGpuProfile.latestCpuPassDurationsMs.size() == frame.cpuPassDurationsMs.size())
  {
    m_passGpuProfile.latestCpuPassDurationsMs = frame.cpuPassDurationsMs;
  }
  if(anyValidPass)
  {
    m_passGpuProfile.latestPassDurationsMs = frame.passDurationsMs;
    m_passGpuProfile.latestValid = true;
  }
}

void RenderDevice::resetPassGpuProfileQueries(rhi::CommandBuffer& cmdBuffer, uint32_t frameIndex)
{
  if(frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  const rhi::QueryPoolHandle queryPool = m_passGpuProfile.frames[frameIndex].queryPool;
  if(queryPool.isNull())
  {
    return;
  }

  cmdBuffer.resetQueryPool(queryPool, 0, m_passGpuProfile.queryCount);
  m_passGpuProfile.frames[frameIndex].valid = false;
  m_passGpuProfile.frames[frameIndex].hasRecordedQueries = false;
}

void RenderDevice::writePassGpuProfileTimestamp(const PassContext& context, uint32_t passIndex, bool isBegin) const
{
  if(context.commandBuffer == nullptr || context.frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  const rhi::QueryPoolHandle queryPool = m_passGpuProfile.frames[context.frameIndex].queryPool;
  if(queryPool.isNull())
  {
    return;
  }

  const uint32_t queryIndex = passIndex * 2u + (isBegin ? 0u : 1u);
  if(queryIndex >= m_passGpuProfile.queryCount)
  {
    return;
  }

  context.commandBuffer->writeTimestamp(queryPool, queryIndex, /*afterAllCommands=*/!isBegin);
}

void RenderDevice::drawPassGpuProfileOverlay(const RenderParams& params) const
{
  if(m_passGpuProfile.passNames.empty())
  {
    ImGui::TextUnformatted("Waiting for runtime profiler data...");
    return;
  }

  static ImGuiUtils::ProfilerGraph cpuGraph(240);
  static ImGuiUtils::ProfilerGraph gpuGraph(240);
  static std::vector<legit::ProfilerTask> cpuTasks;
  static std::vector<legit::ProfilerTask> gpuTasks;
  static constexpr std::array<uint32_t, 8> kTaskColors = {
      legit::Colors::peterRiver,
      legit::Colors::emerald,
      legit::Colors::sunFlower,
      legit::Colors::carrot,
      legit::Colors::amethyst,
      legit::Colors::alizarin,
      legit::Colors::clouds,
      legit::Colors::turqoise,
  };

  auto buildTasks = [&](const std::vector<double>& durationsMs, std::vector<legit::ProfilerTask>& outTasks) {
    outTasks.clear();
    outTasks.reserve(std::min(m_passGpuProfile.passNames.size(), durationsMs.size()));
    double cursorSeconds = 0.0;
    for(size_t i = 0; i < m_passGpuProfile.passNames.size() && i < durationsMs.size(); ++i)
    {
      const double durationSeconds = std::max(0.0, durationsMs[i]) * 1e-3;
      if(durationSeconds <= 0.0)
      {
        continue;
      }

      legit::ProfilerTask task{};
      task.startTime = cursorSeconds;
      task.endTime = cursorSeconds + durationSeconds;
      task.name = m_passGpuProfile.passNames[i];
      task.color = kTaskColors[i % kTaskColors.size()];
      outTasks.push_back(task);
      cursorSeconds = task.endTime;
    }
  };

  buildTasks(m_passGpuProfile.latestCpuPassDurationsMs, cpuTasks);
  buildTasks(m_passGpuProfile.latestPassDurationsMs, gpuTasks);
  cpuGraph.LoadFrameData(cpuTasks.data(), cpuTasks.size());
  if(m_passGpuProfile.latestValid)
  {
    gpuGraph.LoadFrameData(gpuTasks.data(), gpuTasks.size());
  }
  else
  {
    gpuGraph.LoadFrameData(nullptr, 0);
  }

  float maxFrameTime = std::max(1.0f / 30.0f, params.deltaTime * 1.5f);
  float availableWidth = ImGui::GetContentRegionAvail().x;
  int legendWidth = 220;
  int graphWidth = std::max(120, static_cast<int>(availableWidth) - legendWidth);

  ImGui::TextUnformatted("CPU Pass Timeline");
  cpuGraph.useColoredLegendText = true;
  cpuGraph.RenderTimings(graphWidth, legendWidth, 120, 0, maxFrameTime);
  ImGui::Spacing();
  ImGui::TextUnformatted("GPU Pass Timeline");
  gpuGraph.useColoredLegendText = true;
  gpuGraph.RenderTimings(graphWidth, legendWidth, 120, 0, maxFrameTime);
}

void RenderDevice::drawGPUInfoOverlay(const RenderParams& params) const
{
  ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
  if(params.viewportImageRect.z > 1.0f && params.viewportImageRect.w > 1.0f)
  {
    ImGui::SetNextWindowPos(ImVec2(params.viewportImageRect.x + params.viewportImageRect.z - 360.0f,
                                   params.viewportImageRect.y + 12.0f),
                            ImGuiCond_Always);
  }
  ImGui::SetNextWindowBgAlpha(0.78f);
  if(!ImGui::Begin("GPU Info", nullptr, flags))
  {
    ImGui::End();
    return;
  }

  if(params.debugOptions.showGPUCullingOverlay
     && ImGui::CollapsingHeader("GPU Culling", ImGuiTreeNodeFlags_DefaultOpen))
  {
    drawGPUCullingOverlay(params);
  }

  if(params.debugOptions.showPassGpuProfile
     && ImGui::CollapsingHeader("GPU Pass Profile", ImGuiTreeNodeFlags_DefaultOpen))
  {
    drawPassGpuProfileOverlay(params);
  }

  ImGui::End();
}

void RenderDevice::PassProfilingHooks::beforePass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
{
  if(renderer != nullptr)
  {
    const char* passName = pass.getName();
    if(std::strcmp(passName, "PresentPass") == 0 || std::strcmp(passName, "GPUDrivenPresent") == 0)
    {
      renderer->acquireSwapchainImageForPresent();
    }
    if(passIndex < renderer->m_passGpuProfile.currentCpuPassStartNs.size())
    {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      renderer->m_passGpuProfile.currentCpuPassStartNs[passIndex] =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }
    renderer->writePassGpuProfileTimestamp(context, passIndex, true);
  }
}

void RenderDevice::PassProfilingHooks::afterPass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
{
  (void)pass;
  if(renderer != nullptr)
  {
    if(context.frameIndex < renderer->m_passGpuProfile.frames.size()
       && passIndex < renderer->m_passGpuProfile.currentCpuPassStartNs.size()
       && passIndex < renderer->m_passGpuProfile.frames[context.frameIndex].cpuPassDurationsMs.size())
    {
      const uint64_t startNs = renderer->m_passGpuProfile.currentCpuPassStartNs[passIndex];
      if(startNs != 0)
      {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const uint64_t endNs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        renderer->m_passGpuProfile.frames[context.frameIndex].cpuPassDurationsMs[passIndex] =
            static_cast<double>(endNs - startNs) * 1e-6;
      }
    }
    renderer->writePassGpuProfileTimestamp(context, passIndex, false);
  }
}

void RenderDevice::createGPUCullingResources()
{
  const uint32_t frameCount = std::max<uint32_t>(1u, static_cast<uint32_t>(m_perFrame.frameUserData.size()));

  // Wave 9 (step 3): the 9-binding culling descriptor layout is now an RHI ArgumentLayout
  // (binding 5 is a SAMPLED_IMAGE array of LDepthPyramidMaxMips), and each per-frame set is
  // an RHI ArgumentTable handle. updateGPUCullingArgumentTable writes
  // them via updateArgumentTable. The compute pipeline layout (step 4 migrates it) is still
  // native but sourced from the RHI-owned descriptor-set layout.
  const std::vector<rhi::ArgumentBinding> layoutEntries{
      makeArgumentBinding(0, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(1, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(2, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(3, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(4, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(5, rhi::BindlessResourceType::sampledImage, shaderio::LDepthPyramidMaxMips, rhi::ResourceVisibility::compute),
      makeArgumentBinding(6, rhi::BindlessResourceType::uniformBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(7, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(8, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
  };
  const rhi::ArgumentLayoutHandle cullingLayout = createArgumentLayoutFromBindings(layoutEntries, "gpu-culling");

  m_device.gpuCullingArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    m_device.gpuCullingArgumentTables[frameIndex] = createArgumentTable(ArgumentTableDesc{
        .slot                = ArgumentSlot::shaderSpecific,
        .layout              = cullingLayout,
        .table               = m_device.device->createArgumentTable(cullingLayout),
        .primaryLogicalIndex = 0,
        .debugName           = "gpu-culling",
    });
  }

  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    updateGPUCullingArgumentTable(frameIndex);
  }
}

void RenderDevice::createShadowCullingResources()
{
  const uint32_t frameCount = std::max<uint32_t>(1u, static_cast<uint32_t>(m_perFrame.frameUserData.size()));

  // Wave 9 (shadowCulling RHI migration): the 2-binding descriptor layout is an RHI
  // ArgumentLayout and each per-frame set an ArgumentTable; the pipeline layout is an RHI
  // VulkanPipelineLayout carrying the ShadowCullPushConstants range. Isomorphic to gpuCulling.
  const std::vector<rhi::ArgumentBinding> layoutEntries{
      makeArgumentBinding(0, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
      makeArgumentBinding(1, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::compute),
  };
  const rhi::ArgumentLayoutHandle shadowLayout = createArgumentLayoutFromBindings(layoutEntries, "shadow-culling");

  m_device.shadowCullingArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    m_device.shadowCullingArgumentTables[frameIndex] = createArgumentTable(ArgumentTableDesc{
        .slot                = ArgumentSlot::shaderSpecific,
        .layout              = shadowLayout,
        .table               = m_device.device->createArgumentTable(shadowLayout),
        .primaryLogicalIndex = 0,
        .debugName           = "shadow-culling",
    });
  }

  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    updateShadowCullingArgumentTable(frameIndex);
  }
}

void RenderDevice::updateGPUCullingArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || frameIndex >= m_device.gpuCullingArgumentTables.size()
     || m_device.gpuCullingArgumentTables[frameIndex].isNull())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  // binding 0/7/8 resolve to either the external persistent buffers or the per-frame native
  // object buffer (step 2 mirrored both as RHI handles), exactly like the native path did.
  const rhi::BufferHandle objectHandle =
      frameUserData.useExternalGPUCullingObjectBuffer
          ? frameUserData.externalGPUCullingObjectBufferRHI
          : frameUserData.gpuCullingObjectBufferRHI;
  const rhi::BufferHandle meshletHandle =
      frameUserData.useExternalGPUCullingMeshletData ? frameUserData.externalGPUCullingMeshletBufferRHI : objectHandle;
  const rhi::BufferHandle sceneObjectHandle =
      frameUserData.useExternalGPUCullingMeshletData ? frameUserData.externalGPUCullingSceneObjectBufferRHI : objectHandle;
  if(objectHandle.isNull()
     || meshletHandle.isNull()
     || sceneObjectHandle.isNull()
     || frameUserData.gpuCullingIndirectBufferRHI.isNull()
     || frameUserData.gpuCullingDrawCountBufferRHI.isNull()
     || frameUserData.gpuCullingStatsBufferRHI.isNull()
     || frameUserData.gpuCullingUniformBufferRHI.isNull()
     || frameUserData.gpuCullingResultBufferRHI.isNull())
  {
    return;
  }

  SceneResources& sceneResources = m_swapchainDependent.sceneResources;
  const uint32_t mipCount = sceneResources.getDepthPyramidMipCount();
  if(mipCount == 0)
  {
    return;
  }

  std::vector<rhi::ArgumentWrite> writes;
  writes.reserve(8 + shaderio::LDepthPyramidMaxMips);
  writes.push_back(rhi::ArgumentWrite{.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = objectHandle});
  writes.push_back(rhi::ArgumentWrite{.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.gpuCullingIndirectBufferRHI});
  writes.push_back(rhi::ArgumentWrite{.binding = 2, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.gpuCullingStatsBufferRHI, .size = sizeof(shaderio::GPUCullStats)});
  writes.push_back(rhi::ArgumentWrite{.binding = 3, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.gpuCullingResultBufferRHI});
  writes.push_back(rhi::ArgumentWrite{.binding = 4, .type = rhi::ArgumentType::storageBuffer, .buffer = frameUserData.gpuCullingDrawCountBufferRHI, .size = sizeof(shaderio::GPUCullDrawCounts)});
  for(uint32_t i = 0; i < static_cast<uint32_t>(shaderio::LDepthPyramidMaxMips); ++i)
  {
    writes.push_back(rhi::ArgumentWrite{
        .binding      = 5,
        .arrayElement = i,
        .type         = rhi::ArgumentType::sampledTexture,
        .textureView  = sceneResources.getDepthPyramidMipView(std::min(i, mipCount - 1u)),
        .accessIntent = rhi::ArgumentAccessIntent::readWrite,
    });
  }
  writes.push_back(rhi::ArgumentWrite{.binding = 6, .type = rhi::ArgumentType::uniformBuffer, .buffer = frameUserData.gpuCullingUniformBufferRHI, .size = sizeof(shaderio::GPUCullingUniforms)});
  writes.push_back(rhi::ArgumentWrite{.binding = 7, .type = rhi::ArgumentType::storageBuffer, .buffer = meshletHandle});
  writes.push_back(rhi::ArgumentWrite{.binding = 8, .type = rhi::ArgumentType::storageBuffer, .buffer = sceneObjectHandle});

  m_device.device->updateArgumentTable(m_device.gpuCullingArgumentTables[frameIndex],
                                       static_cast<uint32_t>(writes.size()), writes.data());
}

void RenderDevice::createLightResources()
{
  const uint32_t frameCount = std::max<uint32_t>(1U, static_cast<uint32_t>(m_perFrame.frameUserData.size()));
  m_lightResources.init(*m_device.device, reinterpret_cast<uintptr_t>(m_device.allocator), LightResources::CreateInfo{
      .maxPointLights = 256,
      .maxSpotLights  = 128,
      .frameCount     = frameCount,
  });
}


void RenderDevice::rebuildSwapchainDependentResources(std::optional<rhi::Extent2D> requestedViewportSize)
{
  if(requestedViewportSize.has_value() && isValidExtent(requestedViewportSize.value()))
  {
    if(extentChanged(m_swapchainDependent.viewportSize, requestedViewportSize.value()))
    {
      m_swapchainDependent.swapchain->requestRebuild();
    }
    m_swapchainDependent.viewportSize = requestedViewportSize.value();
  }

  bool swapchainRebuilt = false;
  if(m_swapchainDependent.swapchain->needsRebuild())
  {
    m_swapchainDependent.swapchain->rebuild();
    const rhi::Extent2D extent             = m_swapchainDependent.swapchain->getExtent();
    m_swapchainDependent.windowSize        = extent;
    m_swapchainDependent.currentImageIndex = 0;
    m_swapchainDependent.hasAcquiredImage  = false;
    m_swapchainDependent.imageStates.assign(
        m_swapchainDependent.swapchain->getMaxFramesInFlight(),
        rhi::ResourceState::Undefined);
    swapchainRebuilt                       = true;
  }

  const rhi::Extent2D gBufferSize = m_swapchainDependent.sceneResources.getSize();
  if(!extentChanged(gBufferSize, m_swapchainDependent.viewportSize))
  {
    return;
  }

  // SceneResources are referenced by every graphics pass. Before destroying and
  // recreating them, wait for every frame slot to retire so no in-flight command
  // buffer can still reference the old attachments.
  const uint32_t frameCount = m_perFrame.frameContext->getFrameCount();
  for(uint32_t i = 0; i < frameCount; ++i)
  {
    m_perFrame.frameContext->waitForFrame(i);
  }

  const VkDevice  device        = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const VkQueue   graphicsQueue = fromNativeHandle<VkQueue>(m_device.device->getGraphicsQueue().backendHandle);
  VkCommandBuffer cmd           = utils::beginSingleTimeCommands(device, m_device.transientCmdPool);
  rhi::vulkan::VulkanCommandBuffer rhiCmd;
  rhiCmd.setTarget(cmd, &m_device.resourceTable);
  m_swapchainDependent.sceneResources.update(rhiCmd, m_swapchainDependent.viewportSize);
  utils::endSingleTimeCommands(cmd, device, m_device.transientCmdPool, graphicsQueue);

  for(uint32_t frameIndex = 0; frameIndex < m_perFrame.frameUserData.size(); ++frameIndex)
  {
    updateGPUCullingArgumentTable(frameIndex);
  }

}

void RenderDevice::bindStaticPassResources(PassExecutor& passExecutor) const
{
  // Bind static resources that don't change per-frame
  // Called once after swapchain/resources rebuild
  passExecutor.bindBuffer({
      .handle       = kPassVertexBufferHandle,
      .backendBufferToken = reinterpret_cast<uint64_t>(m_device.vertexBuffer.buffer),
  });
  passExecutor.bindTexture({
      .handle       = kPassGBuffer0Handle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getColorImage(0)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassGBuffer1Handle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getColorImage(1)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassGBuffer2Handle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getColorImage(2)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassSceneDepthHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getDepthImage()),
      .aspect       = sceneDepthTextureAspect(m_swapchainDependent.sceneResources.getDepthFormat()),
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassShadowHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getShadowMapImage()),
      .aspect       = rhi::TextureAspect::depth,
      // After swapchain rebuild, cmdInitImageLayout initializes shadow map to GENERAL
      .initialState = rhi::ResourceState::General,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassCSMShadowHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_csmShadowResources.getCascadeImage()),
      .aspect       = rhi::TextureAspect::depth,
      // The CSM pass clears the cascade array at the start of each frame.
      .initialState = rhi::ResourceState::Undefined,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getDepthPyramidImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassOutputHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getOutputTextureImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassSceneColorHdrHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getSceneColorHdrImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomHalfHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomHalfImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomQuarterHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomQuarterImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomEighthHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomEighthImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomSixteenthHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomSixteenthImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomThirtySecondHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomThirtySecondImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomUpsampleSixteenthHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomUpsampleEighthHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomUpsampleEighthImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomUpsampleQuarterHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomUpsampleQuarterImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassBloomOutputHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getBloomOutputImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassVelocityHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getVelocityImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassSceneColorHistoryReadHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getSceneColorHistoryImage(1)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  passExecutor.bindTexture({
      .handle       = kPassSceneColorHistoryWriteHandle,
      .backendImageToken  = resolveNativeImage(*m_device.device, m_swapchainDependent.sceneResources.getSceneColorHistoryImage(0)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
}

rhi::CommandBuffer& RenderDevice::beginCommandRecording()
{
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");
  auto* vulkanFrameContext = dynamic_cast<rhi::vulkan::VulkanFrameContext*>(m_perFrame.frameContext.get());
  ASSERT(vulkanFrameContext != nullptr, "RenderDevice currently requires VulkanFrameContext");
  vulkanFrameContext->setResourceTable(&m_device.resourceTable);
  rhi::CommandBuffer* cmdBuffer = m_perFrame.frameContext->getCommandBuffer();
  ASSERT(cmdBuffer != nullptr, "Current frame command buffer must be valid");
  return *cmdBuffer;
}

void RenderDevice::drawFrame(rhi::CommandBuffer& cmdBuffer, const RenderParams& params, PassExecutor& passExecutor)
{

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  auto&          frameUserData     = m_perFrame.frameUserData[currentFrameIndex];

  {
    demo::profiling::ScopedCpuRange buildRenderItemsRange("BuildRenderItems");

    // Update CSM cascade matrices based on current camera and light direction
    if(params.cameraUniforms != nullptr)
    {
      const Aabb sceneBounds = computeSceneBounds(params.gltfModel, params.gpuDrivenSceneView);
      m_csmShadowResources.updateCascadeMatrices(*params.cameraUniforms,
                                                 params.lightSettings.direction,
                                                 params.lightSettings.shadowDistance,
                                                 sceneBounds.min,
                                                 sceneBounds.max,
                                                 sceneBounds.valid);
    }

    m_frameLightingState = buildFrameLightingState(params);
    if(params.debugOptions.enablePointLights)
    {
      ensureTestPointLights(params);
    }
    else
    {
      m_testPointLights.clear();
      m_testPointLightMotions.clear();
      m_testPointLightSceneBounds = {};
      m_testSpotLights.clear();
    }
    buildDebugDrawList(params);
    updateLightingUniformBuffer(currentFrameIndex, shaderio::LightingUniforms{m_frameLightingState.lightParams});
    updateLightCullingUniformBuffer(currentFrameIndex, buildLightCullingUniforms(params));
    updateGPUCullingBuffers(currentFrameIndex, params);
    updateShadowCullingBuffers(currentFrameIndex, params);

    // Route through pass executor to orchestrate multi-pass rendering
    // Static resources (GBuffer, SceneDepth, Shadow, CSM, Output, VertexBuffer) are already bound
    // in bindStaticPassResources() and persist across frames
    // Only bind dynamic per-frame resources here
    passExecutor.bindBuffer({
        .handle       = kTransientAllocatorBufferHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(frameUserData.transientAllocator.getBufferHandle()),
    });
    passExecutor.bindBuffer({
        .handle       = kPassPointLightBufferHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(m_lightResources.getPointLightBuffer(currentFrameIndex)),
    });
    passExecutor.bindBuffer({
        .handle       = kPassSpotLightBufferHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(m_lightResources.getSpotLightBuffer(currentFrameIndex)),
    });
    passExecutor.bindBuffer({
        .handle       = kPassPointLightCoarseBoundsHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(m_lightResources.getPointCoarseBoundsBuffer(currentFrameIndex)),
    });
    passExecutor.bindBuffer({
        .handle       = kPassSpotLightCoarseBoundsHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(m_lightResources.getSpotCoarseBoundsBuffer(currentFrameIndex)),
    });
    passExecutor.bindBuffer({
        .handle       = kPassLightCoarseCullingUniformHandle,
        .backendBufferToken = m_device.resourceTable.resolveBuffer(m_lightResources.getCoarseCullingUniformBuffer(currentFrameIndex)),
    });
    passExecutor.bindBuffer({
        .handle       = kPassGPUCullObjectBufferHandle,
        .backendBufferToken = frameUserData.useExternalGPUCullingObjectBuffer
                            ? reinterpret_cast<uint64_t>(frameUserData.externalGPUCullingObjectBuffer)
                            : reinterpret_cast<uint64_t>(frameUserData.gpuCullingObjectBuffer.buffer),
    });
    passExecutor.bindBuffer({
        .handle       = kPassGPUCullIndirectBufferHandle,
        .backendBufferToken = reinterpret_cast<uint64_t>(frameUserData.gpuCullingIndirectBuffer.buffer),
    });
    passExecutor.bindBuffer({
        .handle       = kPassGPUCullStatsBufferHandle,
        .backendBufferToken = reinterpret_cast<uint64_t>(frameUserData.gpuCullingStatsBuffer.buffer),
    });
    passExecutor.bindBuffer({
        .handle       = kPassGPUCullUniformBufferHandle,
        .backendBufferToken = reinterpret_cast<uint64_t>(frameUserData.gpuCullingUniformBuffer.buffer),
    });
    passExecutor.bindBuffer({
        .handle       = kPassGPUCullResultBufferHandle,
        .backendBufferToken = reinterpret_cast<uint64_t>(frameUserData.gpuCullingResultBuffer.buffer),
    });
    m_perPass.drawStream.clear();
  }

  // Allocate CameraUniforms once for all passes
  const rhi::Extent2D viewportExtent = m_swapchainDependent.sceneResources.getSize();
  const float viewportWidth = static_cast<float>(viewportExtent.width);
  const float viewportHeight = static_cast<float>(viewportExtent.height);
  TransientAllocator::Allocation cameraAlloc =
      frameUserData.transientAllocator.allocate(sizeof(shaderio::CameraUniforms), 256);
  shaderio::CameraUniforms cameraData{};
  if(params.cameraUniforms != nullptr)
  {
    cameraData = *params.cameraUniforms;
  }
  else
  {
    // Fallback: default camera (consistent with passes)
    cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraData.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), viewportWidth / viewportHeight, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    cameraData.viewProjection = cameraData.projection * cameraData.view;
    cameraData.inverseViewProjection = glm::inverse(cameraData.viewProjection);
    cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
  }
  if(params.cameraUniforms == nullptr)
  {
    cameraData.unjitteredViewProjection = cameraData.viewProjection;
    cameraData.unjitteredInverseViewProjection = cameraData.inverseViewProjection;
    cameraData.prevUnjitteredViewProjection = cameraData.viewProjection;
    cameraData.prevJitteredViewProjection = cameraData.viewProjection;
  }
  std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
  frameUserData.transientAllocator.flushAllocation(cameraAlloc, sizeof(cameraData));

  demo::PassContext context{
      &frameUserData.transientAllocator, currentFrameIndex, 0, &params, &m_perPass.drawStream, params.gltfModel,
      getCurrentMaterialArgumentTable(), cameraAlloc, true, &cmdBuffer};
  context.executor = &passExecutor;
  resetPassGpuProfileQueries(*context.commandBuffer, currentFrameIndex);

  {
    passExecutor.execute(context, &m_passProfilingHooks);
  }

  if(currentFrameIndex < m_passGpuProfile.frames.size())
  {
    m_passGpuProfile.frames[currentFrameIndex].hasRecordedQueries = true;
  }

  {

    // Update swapchain image state after rendering if this frame actually wrote a
    // presentable image. Late-acquire frames that skipped presentation leave the
    // swapchain ring untouched.
    if(m_swapchainDependent.hasAcquiredImage
       && m_swapchainDependent.currentImageIndex < m_swapchainDependent.imageStates.size())
    {
      m_swapchainDependent.imageStates[m_swapchainDependent.currentImageIndex] = rhi::ResourceState::Present;
    }
  }
}

void RenderDevice::endFrame(rhi::CommandBuffer& cmdBuffer)
{

  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  {
    demo::profiling::ScopedCpuRange queueSubmitRange("QueueSubmit");
    submitFrame(*m_perFrame.frameContext, cmdBuffer);
  }

  // Frame advancement and wait moved to prepareFrameResources for CPU/GPU overlap
  // GPU executes frame N while CPU records frame N+1

  {
    demo::profiling::ScopedCpuRange presentRange("Present");
    presentFrame(*m_swapchainDependent.swapchain);
  }
  m_perFrame.frameCounter++;

}

void RenderDevice::DebugDrawList::addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
{
  vertices.push_back(shaderio::DebugLineVertex{a, color});
  vertices.push_back(shaderio::DebugLineVertex{b, color});
}

void RenderDevice::DebugDrawList::addAabb(const Aabb& bounds, const glm::vec4& color)
{
  if(!bounds.valid)
  {
    return;
  }

  const std::array<glm::vec3, 8> corners{{
      {bounds.min.x, bounds.min.y, bounds.min.z},
      {bounds.max.x, bounds.min.y, bounds.min.z},
      {bounds.min.x, bounds.max.y, bounds.min.z},
      {bounds.max.x, bounds.max.y, bounds.min.z},
      {bounds.min.x, bounds.min.y, bounds.max.z},
      {bounds.max.x, bounds.min.y, bounds.max.z},
      {bounds.min.x, bounds.max.y, bounds.max.z},
      {bounds.max.x, bounds.max.y, bounds.max.z},
  }};
  addFrustum(corners, color);
}

void RenderDevice::DebugDrawList::addFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec4& color)
{
  static constexpr std::array<std::pair<uint32_t, uint32_t>, 12> kEdges{{
      {0, 1}, {1, 3}, {3, 2}, {2, 0},
      {4, 5}, {5, 7}, {7, 6}, {6, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  }};

  for(const auto& [a, b] : kEdges)
  {
    addLine(corners[a], corners[b], color);
  }
}

void RenderDevice::DebugDrawList::addSphere(const glm::vec3& center, float radius, const glm::vec4& color, uint32_t segments)
{
  if(radius <= 0.0f || segments < 3)
  {
    return;
  }

  const float delta = 6.28318530718f / static_cast<float>(segments);
  for(uint32_t i = 0; i < segments; ++i)
  {
    const float angle0 = delta * static_cast<float>(i);
    const float angle1 = delta * static_cast<float>(i + 1);
    addLine(center + glm::vec3(std::cos(angle0) * radius, 0.0f, std::sin(angle0) * radius),
            center + glm::vec3(std::cos(angle1) * radius, 0.0f, std::sin(angle1) * radius), color);
    addLine(center + glm::vec3(0.0f, std::cos(angle0) * radius, std::sin(angle0) * radius),
            center + glm::vec3(0.0f, std::cos(angle1) * radius, std::sin(angle1) * radius), color);
    addLine(center + glm::vec3(std::cos(angle0) * radius, std::sin(angle0) * radius, 0.0f),
            center + glm::vec3(std::cos(angle1) * radius, std::sin(angle1) * radius, 0.0f), color);
  }
}

void RenderDevice::DebugDrawList::addArrow(const glm::vec3& origin, const glm::vec3& direction, float length, const glm::vec4& color)
{
  if(length <= 0.0f)
  {
    return;
  }

  const glm::vec3 dir = glm::normalize(direction);
  const glm::vec3 end = origin + dir * length;
  addLine(origin, end, color);

  const glm::vec3 reference = std::abs(dir.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 tangent = glm::normalize(glm::cross(dir, reference));
  const glm::vec3 bitangent = glm::normalize(glm::cross(dir, tangent));
  const float headLength = length * 0.15f;
  addLine(end, end - dir * headLength + tangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength - tangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength + bitangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength - bitangent * headLength * 0.5f, color);
}

RenderDevice::Aabb RenderDevice::computeSceneBounds(const GltfUploadResult* gltfModel, const GPUDrivenSceneView* gpuDrivenSceneView) const
{
  Aabb bounds{};

  if(gpuDrivenSceneView != nullptr && gpuDrivenSceneView->sceneBoundsValid)
  {
    bounds.min = gpuDrivenSceneView->sceneBoundsMin;
    bounds.max = gpuDrivenSceneView->sceneBoundsMax;
    bounds.valid = true;
    return bounds;
  }

  const MeshHandle* meshHandles = nullptr;
  uint32_t meshCount = 0;
  if(gpuDrivenSceneView != nullptr && gpuDrivenSceneView->meshHandles != nullptr && gpuDrivenSceneView->meshHandleCount > 0u)
  {
    meshHandles = gpuDrivenSceneView->meshHandles;
    meshCount = gpuDrivenSceneView->meshHandleCount;
  }
  else if(gltfModel != nullptr && !gltfModel->meshes.empty())
  {
    meshHandles = gltfModel->meshes.data();
    meshCount = static_cast<uint32_t>(gltfModel->meshes.size());
  }

  if(meshHandles == nullptr || meshCount == 0u)
  {
    return bounds;
  }

  bounds.min = glm::vec3(std::numeric_limits<float>::max());
  bounds.max = glm::vec3(std::numeric_limits<float>::lowest());
  bounds.valid = false;

  for(uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
  {
    const MeshHandle meshHandle = meshHandles[meshIndex];
    const MeshRecord* mesh = m_meshPool.tryGet(meshHandle);
    if(mesh == nullptr)
    {
      continue;
    }

    bounds.min = glm::min(bounds.min, mesh->worldBoundsMin);
    bounds.max = glm::max(bounds.max, mesh->worldBoundsMax);
    bounds.valid = true;
  }

  return bounds;
}

void RenderDevice::ensureTestPointLights(const RenderParams& params)
{
  glm::vec3 minBounds(-12.0f, 0.5f, -12.0f);
  glm::vec3 maxBounds(12.0f, 6.0f, 12.0f);
  if(m_frameLightingState.sceneBounds.valid)
  {
    minBounds = m_frameLightingState.sceneBounds.min;
    maxBounds = m_frameLightingState.sceneBounds.max;
    const glm::vec3 size = glm::max(maxBounds - minBounds, glm::vec3(1.0f));
    minBounds += size * 0.08f;
    maxBounds -= size * 0.08f;
  }

  const glm::vec3 sceneMinBounds = minBounds;
  const glm::vec3 sceneMaxBounds = maxBounds;
  const glm::vec3 sceneBoundsSize = glm::max(sceneMaxBounds - sceneMinBounds, glm::vec3(1.0f));
  const float sceneBoundsEpsilon = std::max(0.1f, glm::length(sceneBoundsSize) * 0.01f);
  const bool sceneBoundsChanged =
      !m_testPointLightSceneBounds.valid ||
      glm::length(m_testPointLightSceneBounds.min - sceneMinBounds) > sceneBoundsEpsilon ||
      glm::length(m_testPointLightSceneBounds.max - sceneMaxBounds) > sceneBoundsEpsilon;

  if(sceneBoundsChanged)
  {
    m_testPointLights.clear();
    m_testPointLightMotions.clear();
    m_testPointLightSceneBounds.min = sceneMinBounds;
    m_testPointLightSceneBounds.max = sceneMaxBounds;
    m_testPointLightSceneBounds.valid = true;
  }

  if(m_frameLightingState.sceneBounds.valid && params.cameraUniforms != nullptr)
  {
    const glm::mat4 inverseView = glm::inverse(params.cameraUniforms->view);
    const glm::vec3 cameraPosition = params.cameraUniforms->cameraPosition;
    const glm::vec3 cameraForward = -glm::normalize(glm::vec3(inverseView[2]));
    const float sceneDiagonal = glm::length(sceneBoundsSize);
    const float focusDistance = glm::clamp(sceneDiagonal * 0.08f, 8.0f, 35.0f);
    const float fieldRadius = glm::clamp(sceneDiagonal * 0.16f, 14.0f, 70.0f);
    const glm::vec3 focus = glm::clamp(cameraPosition + cameraForward * focusDistance, sceneMinBounds, sceneMaxBounds);
    const glm::vec3 halfExtent(fieldRadius, std::max(6.0f, fieldRadius * 0.45f), fieldRadius);
    const glm::vec3 localMin = glm::max(sceneMinBounds, focus - halfExtent);
    const glm::vec3 localMax = glm::min(sceneMaxBounds, focus + halfExtent);
    if(glm::all(glm::greaterThan(localMax - localMin, glm::vec3(0.25f))))
    {
      minBounds = localMin;
      maxBounds = localMax;
    }
  }

  std::mt19937 rng(0x5EED1234u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::uniform_real_distribution<float> phaseDistribution(0.0f, 6.28318530718f);
  std::uniform_real_distribution<float> speedDistribution(0.25f, 1.2f);
  std::uniform_real_distribution<float> amplitudeDistribution(0.06f, 0.18f);

  const glm::vec3 boundsSize = glm::max(maxBounds - minBounds, glm::vec3(1.0f));
  if(m_testPointLights.empty())
  {
    m_testPointLights.reserve(kTestPointLightCount);
    m_testPointLightMotions.reserve(kTestPointLightCount);
    for(uint32_t i = 0; i < kTestPointLightCount; ++i)
    {
      TestPointLightMotion motion{};
      motion.baseT = glm::vec3(unit(rng), unit(rng), unit(rng));
      motion.phase = glm::vec3(phaseDistribution(rng), phaseDistribution(rng), phaseDistribution(rng));
      motion.speed = glm::vec3(speedDistribution(rng), speedDistribution(rng), speedDistribution(rng));
      motion.amplitude = boundsSize * glm::vec3(amplitudeDistribution(rng), amplitudeDistribution(rng), amplitudeDistribution(rng));
      motion.radiusT = unit(rng);
      motion.intensityT = unit(rng);
      m_testPointLightMotions.push_back(motion);

      shaderio::LightData light{};
      light.positionOrDirection = glm::mix(minBounds, maxBounds, motion.baseT);
      light.color = glm::vec3(0.35f + unit(rng) * 0.65f,
                              0.35f + unit(rng) * 0.65f,
                              0.35f + unit(rng) * 0.65f);
      light.spotDirection = glm::vec3(0.0f, -1.0f, 0.0f);
      light.spotInnerAngle = 0.0f;
      light.lightType = shaderio::LLightTypePoint;
      light.spotOuterAngle = 0.0f;
      m_testPointLights.push_back(light);
    }

    m_testSpotLights.clear();
    LOGI("Generated %zu animated random point lights for LightCoarseCulling", m_testPointLights.size());
  }

  const float time = params.timeSeconds;
  const float maxRadius = std::max(0.1f, params.debugOptions.pointLightMaxRadius);
  const float minRadius = std::min(maxRadius, std::max(0.25f, maxRadius * 0.35f));
  const float intensityScale = std::max(0.0f, params.debugOptions.pointLightIntensityScale);
  for(size_t i = 0; i < m_testPointLights.size() && i < m_testPointLightMotions.size(); ++i)
  {
    const TestPointLightMotion& motion = m_testPointLightMotions[i];
    const glm::vec3 basePosition = glm::mix(minBounds, maxBounds, motion.baseT);
    const glm::vec3 offset(
        std::sin(time * motion.speed.x + motion.phase.x) * motion.amplitude.x,
        std::sin(time * motion.speed.y + motion.phase.y) * motion.amplitude.y,
        std::cos(time * motion.speed.z + motion.phase.z) * motion.amplitude.z);
    m_testPointLights[i].positionOrDirection = glm::clamp(basePosition + offset, minBounds, maxBounds);
    m_testPointLights[i].range = glm::mix(minRadius, maxRadius, motion.radiusT);
    m_testPointLights[i].intensity = glm::mix(8.0f, 26.0f, motion.intensityT) * intensityScale;
  }
}

std::array<glm::vec3, 8> RenderDevice::computePerspectiveFrustumCorners(const shaderio::CameraUniforms& cameraUniforms,
                                                                    float nearDistance,
                                                                    float farDistance) const
{
  const glm::mat4 inverseView = glm::inverse(cameraUniforms.view);
  const glm::vec3 position = cameraUniforms.cameraPosition;
  const glm::vec3 right = glm::normalize(glm::vec3(inverseView[0]));
  const glm::vec3 up = glm::normalize(glm::vec3(inverseView[1]));
  const glm::vec3 forward = -glm::normalize(glm::vec3(inverseView[2]));

  const float tanHalfFovX = 1.0f / std::abs(cameraUniforms.projection[0][0]);
  const float tanHalfFovY = 1.0f / std::abs(cameraUniforms.projection[1][1]);

  const float nearHalfWidth = nearDistance * tanHalfFovX;
  const float nearHalfHeight = nearDistance * tanHalfFovY;
  const float farHalfWidth = farDistance * tanHalfFovX;
  const float farHalfHeight = farDistance * tanHalfFovY;

  const glm::vec3 nearCenter = position + forward * nearDistance;
  const glm::vec3 farCenter = position + forward * farDistance;

  return {{
      nearCenter - right * nearHalfWidth - up * nearHalfHeight,
      nearCenter + right * nearHalfWidth - up * nearHalfHeight,
      nearCenter - right * nearHalfWidth + up * nearHalfHeight,
      nearCenter + right * nearHalfWidth + up * nearHalfHeight,
      farCenter - right * farHalfWidth - up * farHalfHeight,
      farCenter + right * farHalfWidth - up * farHalfHeight,
      farCenter - right * farHalfWidth + up * farHalfHeight,
      farCenter + right * farHalfWidth + up * farHalfHeight,
  }};
}

std::array<glm::vec3, 8> RenderDevice::computeOrthoFrustumCorners(const glm::mat4& inverseViewProjection) const
{
  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const std::array<glm::vec3, 8> clipCorners{{
      {-1.0f, -1.0f, projectionConvention.ndcNearZ},
      { 1.0f, -1.0f, projectionConvention.ndcNearZ},
      {-1.0f,  1.0f, projectionConvention.ndcNearZ},
      { 1.0f,  1.0f, projectionConvention.ndcNearZ},
      {-1.0f, -1.0f, projectionConvention.ndcFarZ},
      { 1.0f, -1.0f, projectionConvention.ndcFarZ},
      {-1.0f,  1.0f, projectionConvention.ndcFarZ},
      { 1.0f,  1.0f, projectionConvention.ndcFarZ},
  }};

  std::array<glm::vec3, 8> worldCorners{};
  for(size_t i = 0; i < clipCorners.size(); ++i)
  {
    const glm::vec4 world = inverseViewProjection * glm::vec4(clipCorners[i], 1.0f);
    worldCorners[i] = glm::vec3(world) / world.w;
  }
  return worldCorners;
}

shaderio::LightCullingUniforms RenderDevice::buildLightCullingUniforms(const RenderParams& params) const
{
  shaderio::LightCullingUniforms uniforms{};
  const rhi::Extent2D extent = m_swapchainDependent.sceneResources.getSize();

  shaderio::CameraUniforms camera{};
  if(params.cameraUniforms != nullptr)
  {
    camera = *params.cameraUniforms;
  }
  else
  {
    camera.view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.unjitteredViewProjection = camera.viewProjection;
    camera.unjitteredInverseViewProjection = camera.inverseViewProjection;
    camera.prevUnjitteredViewProjection = camera.viewProjection;
    camera.prevJitteredViewProjection = camera.viewProjection;
    camera.cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f);
  }

  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const float nearPlane = std::abs(clipspace::extractPerspectiveNearPlane(camera.projection, projectionConvention));
  const float farPlane = std::abs(clipspace::extractPerspectiveFarPlane(camera.projection, projectionConvention));

  uniforms.screenSizeAndClipPlanes = glm::vec4(
      static_cast<float>(extent.width),
      static_cast<float>(extent.height),
      nearPlane,
      farPlane);
  uniforms.viewMatrix = camera.view;
  uniforms.projectionMatrix = camera.projection;
  uniforms.invProjectionMatrix = glm::inverse(camera.projection);
  return uniforms;
}

shaderio::GPUCullingUniforms RenderDevice::buildGPUCullingUniforms(const RenderParams& params, uint32_t objectCount) const
{
  shaderio::GPUCullingUniforms uniforms{};
  const rhi::Extent2D screenExtent = m_swapchainDependent.sceneResources.getSize();
  const rhi::Extent2D pyramidExtent = m_swapchainDependent.sceneResources.getDepthPyramidExtent();
  const uint32_t mipCount =
      std::min<uint32_t>(m_swapchainDependent.sceneResources.getDepthPyramidMipCount(), shaderio::LDepthPyramidMaxMips);

  shaderio::CameraUniforms camera{};
  if(params.cameraUniforms != nullptr)
  {
    camera = *params.cameraUniforms;
  }
  else
  {
    camera.view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.unjitteredViewProjection = camera.viewProjection;
    camera.unjitteredInverseViewProjection = camera.inverseViewProjection;
    camera.prevUnjitteredViewProjection = camera.viewProjection;
    camera.prevJitteredViewProjection = camera.viewProjection;
    camera.cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f);
  }

  const glm::mat4 inverseView = glm::inverse(camera.view);
  const auto frustumPlanes = extractFrustumPlanes(camera.viewProjection);

  uniforms.viewMatrix = camera.view;
  uniforms.projectionMatrix = camera.projection;
  uniforms.viewProjectionMatrix = camera.viewProjection;
  for(uint32_t planeIndex = 0; planeIndex < shaderio::LGPUCullingFrustumPlaneCount; ++planeIndex)
  {
    uniforms.frustumPlanes[planeIndex] = frustumPlanes[planeIndex];
  }
  uniforms.cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f);
  uniforms.cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f);
  uniforms.screenSizeAndPyramidSize =
      glm::vec4(static_cast<float>(screenExtent.width),
                static_cast<float>(screenExtent.height),
                static_cast<float>(pyramidExtent.width),
                static_cast<float>(pyramidExtent.height));
  uniforms.cullingInfo = glm::vec4(static_cast<float>(objectCount),
                                   static_cast<float>(mipCount),
                                   params.debugOptions.enableGPUOcclusionCulling ? 1.0f : 0.0f,
                                   2e-3f);
  const bool meshletCulling =
      params.gpuDrivenSceneView != nullptr
      && params.gpuDrivenSceneView->gpuCullMeshletBuffer != 0
      && params.gpuDrivenSceneView->gpuCullSceneObjectBuffer != 0;
  uniforms.cullingControls = glm::vec4(params.debugOptions.enableGPUFrustumCulling ? 1.0f : 0.0f,
                                       params.debugOptions.enableGPUOcclusionCulling ? 1.0f : 0.0f,
                                       meshletCulling ? 1.0f : 0.0f,
                                       (meshletCulling && params.debugOptions.enableGPUMeshletConeCulling) ? 1.0f : 0.0f);
  uniforms.cameraPositionAndMeshletInfo = glm::vec4(camera.cameraPosition, 0.0f);
  return uniforms;
}

shaderio::ShadowCullPushConstants RenderDevice::buildShadowCullPushConstants(uint32_t cascadeIndex, uint32_t objectCount) const
{
  shaderio::ShadowCullPushConstants pushConstants{};
  const shaderio::ShadowUniforms* shadowUniforms = m_csmShadowResources.getShadowUniformsData();
  if(shadowUniforms == nullptr || cascadeIndex >= shaderio::LCascadeCount)
  {
    return pushConstants;
  }

  const CSMShadowResources::CascadeData& cascadeData = m_csmShadowResources.getCascadeData(cascadeIndex);
  const auto& frustumPlanes = cascadeData.cullingPlanes;
  for(uint32_t planeIndex = 0; planeIndex < shaderio::LGPUCullingFrustumPlaneCount; ++planeIndex)
  {
    pushConstants.frustumPlanes[planeIndex] = frustumPlanes[planeIndex];
  }
  pushConstants.objectCount = objectCount;
  return pushConstants;
}

RenderDevice::FrameLightingState RenderDevice::buildFrameLightingState(const RenderParams& params) const
{
  FrameLightingState state{};
  const shaderio::CameraUniforms fallbackCamera{
      .view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
      .projection = [] {
        return clipspace::makePerspectiveProjection(
            glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
            clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
      }(),
      .viewProjection = glm::mat4(1.0f),
      .inverseViewProjection = glm::mat4(1.0f),
      .prevView = glm::mat4(1.0f),
      .prevProjection = glm::mat4(1.0f),
      .prevViewProjection = glm::mat4(1.0f),
      .unjitteredViewProjection = glm::mat4(1.0f),
      .unjitteredInverseViewProjection = glm::mat4(1.0f),
      .prevUnjitteredViewProjection = glm::mat4(1.0f),
      .prevJitteredViewProjection = glm::mat4(1.0f),
      .cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f),
      .shadowConstantBias = 0.0f,
      .shadowDirectionAndSlopeBias = glm::vec4(0.0f),
  };

  shaderio::CameraUniforms camera = params.cameraUniforms != nullptr ? *params.cameraUniforms : fallbackCamera;
  if(params.cameraUniforms == nullptr)
  {
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.unjitteredViewProjection = camera.viewProjection;
    camera.unjitteredInverseViewProjection = camera.inverseViewProjection;
    camera.prevUnjitteredViewProjection = camera.viewProjection;
    camera.prevJitteredViewProjection = camera.viewProjection;
    camera.shadowConstantBias = 0.0f;
    camera.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
  }

  state.sceneBounds = computeSceneBounds(params.gltfModel, params.gpuDrivenSceneView);

  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const float cameraNear = std::abs(clipspace::extractPerspectiveNearPlane(camera.projection, projectionConvention));
  const float cameraFar = std::abs(clipspace::extractPerspectiveFarPlane(camera.projection, projectionConvention));
  state.shadowDistance = glm::clamp(params.lightSettings.shadowDistance, cameraNear + 0.5f, std::max(cameraFar, cameraNear + 1.0f));
  state.viewFrustumCorners = computePerspectiveFrustumCorners(camera, cameraNear, state.shadowDistance);

  std::array<glm::vec3, 8> shadowFitCorners = state.viewFrustumCorners;
  if(state.sceneBounds.valid)
  {
    shadowFitCorners = {{
        {state.sceneBounds.min.x, state.sceneBounds.min.y, state.sceneBounds.min.z},
        {state.sceneBounds.max.x, state.sceneBounds.min.y, state.sceneBounds.min.z},
        {state.sceneBounds.min.x, state.sceneBounds.max.y, state.sceneBounds.min.z},
        {state.sceneBounds.max.x, state.sceneBounds.max.y, state.sceneBounds.min.z},
        {state.sceneBounds.min.x, state.sceneBounds.min.y, state.sceneBounds.max.z},
        {state.sceneBounds.max.x, state.sceneBounds.min.y, state.sceneBounds.max.z},
        {state.sceneBounds.min.x, state.sceneBounds.max.y, state.sceneBounds.max.z},
        {state.sceneBounds.max.x, state.sceneBounds.max.y, state.sceneBounds.max.z},
    }};
  }

  glm::vec3 center(0.0f);
  for(const glm::vec3& corner : shadowFitCorners)
  {
    center += corner;
  }
  center /= static_cast<float>(shadowFitCorners.size());

  const glm::vec3 lightTravelDir = glm::normalize(params.lightSettings.direction);
  const glm::vec3 dirToLight = -lightTravelDir;
  const glm::vec3 upReference =
      std::abs(lightTravelDir.y) > 0.95f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

  float radius = 0.0f;
  for(const glm::vec3& corner : shadowFitCorners)
  {
    radius = std::max(radius, glm::length(corner - center));
  }
  radius = std::max(radius, 5.0f);

  state.lightAnchor = center;
  const glm::vec3 lightPosition = center - lightTravelDir * (radius + 10.0f);
  const glm::mat4 lightView = glm::lookAt(lightPosition, center, upReference);

  glm::vec3 minExtents(std::numeric_limits<float>::max());
  glm::vec3 maxExtents(std::numeric_limits<float>::lowest());
  for(const glm::vec3& corner : shadowFitCorners)
  {
    const glm::vec3 lightSpace = glm::vec3(lightView * glm::vec4(corner, 1.0f));
    minExtents = glm::min(minExtents, lightSpace);
    maxExtents = glm::max(maxExtents, lightSpace);
  }

  const float xyExtent = std::max(maxExtents.x - minExtents.x, maxExtents.y - minExtents.y) * 0.5f + 2.0f;
  glm::vec2 lightSpaceCenter((minExtents.x + maxExtents.x) * 0.5f, (minExtents.y + maxExtents.y) * 0.5f);
  const float texelWorldSize = (xyExtent * 2.0f) / static_cast<float>(SceneResources::kShadowMapSize);
  lightSpaceCenter = glm::floor(lightSpaceCenter / texelWorldSize) * texelWorldSize;

  minExtents.x = lightSpaceCenter.x - xyExtent;
  maxExtents.x = lightSpaceCenter.x + xyExtent;
  minExtents.y = lightSpaceCenter.y - xyExtent;
  maxExtents.y = lightSpaceCenter.y + xyExtent;
  minExtents.z -= radius * 2.0f + 20.0f;
  maxExtents.z += 20.0f;

  const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
      minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, -maxExtents.z, -minExtents.z, projectionConvention);
  state.shadowCamera.view = lightView;
  state.shadowCamera.projection = lightProjection;
  state.shadowCamera.viewProjection = lightProjection * lightView;
  state.shadowCamera.inverseViewProjection = glm::inverse(state.shadowCamera.viewProjection);
  state.shadowCamera.unjitteredViewProjection = state.shadowCamera.viewProjection;
  state.shadowCamera.unjitteredInverseViewProjection = state.shadowCamera.inverseViewProjection;
  state.shadowCamera.prevUnjitteredViewProjection = state.shadowCamera.viewProjection;
  state.shadowCamera.prevJitteredViewProjection = state.shadowCamera.viewProjection;
  state.shadowCamera.cameraPosition = lightPosition;
  state.shadowCamera.shadowConstantBias = params.lightSettings.depthBias;
  state.shadowCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, params.lightSettings.normalBias);
  state.shadowFrustumCorners = computeOrthoFrustumCorners(glm::inverse(state.shadowCamera.viewProjection));

  // Populate CSM cascade matrices and split distances for LightParams
  const shaderio::ShadowUniforms* shadowData = m_csmShadowResources.getShadowUniformsData();
  for(int i = 0; i < shaderio::LCascadeCount; ++i)
  {
    state.lightParams.worldToShadow[i] = shadowData->cascadeViewProjection[i];
  }
  state.lightParams.cascadeSplitDistances = shadowData->cascadeSplitDistances;
  state.lightParams.lightDirectionAndShadowStrength =
      glm::vec4(dirToLight, params.lightSettings.shadowStrength);
  state.lightParams.lightColorAndNormalBias = glm::vec4(params.lightSettings.color, params.lightSettings.normalBias);
  state.lightParams.ambientColorAndTexelSize =
      glm::vec4(params.lightSettings.ambient, 1.0f / static_cast<float>(m_csmShadowResources.getCascadeResolution()));
  state.lightParams.shadowMetrics = glm::vec4(
      1.0f / static_cast<float>(m_csmShadowResources.getCascadeResolution()),
      params.lightSettings.depthBias,
      params.lightSettings.normalBias,
      static_cast<float>(shaderio::LCascadeCount));
  state.lightParams.iblParams = glm::vec4(
      params.debugOptions.enableIBL ? 1.0f : 0.0f,
      params.debugOptions.iblIntensity,
      static_cast<float>(m_device.iblEnvironmentMipCount > 0 ? m_device.iblEnvironmentMipCount - 1u : 0u),
      m_device.iblEnvironmentLoaded ? 1.0f : 0.0f);
  state.lightParams.iblDebugInfo = glm::vec4(static_cast<float>(params.debugOptions.iblDebugMode), 0.0f, 0.0f, 0.0f);
  state.lightParams.phase7Info = glm::vec4(0.0f);
  return state;
}

void RenderDevice::buildDebugDrawList(const RenderParams& params)
{
  m_debugDrawList.clear();
  if(!params.debugOptions.enabled)
  {
    return;
  }

  if(params.debugOptions.showSceneBounds)
  {
    m_debugDrawList.addAabb(m_frameLightingState.sceneBounds, glm::vec4(0.20f, 0.85f, 0.35f, 0.90f));
  }
  if(params.debugOptions.showShadowFrustum)
  {
    m_debugDrawList.addFrustum(m_frameLightingState.shadowFrustumCorners, glm::vec4(0.95f, 0.75f, 0.20f, 0.90f));
  }
  if(params.debugOptions.showViewFrustum)
  {
    m_debugDrawList.addFrustum(m_frameLightingState.viewFrustumCorners, glm::vec4(0.25f, 0.65f, 1.00f, 0.85f));
  }
  if(params.debugOptions.showLightDirection)
  {
    m_debugDrawList.addArrow(m_frameLightingState.lightAnchor, glm::normalize(params.lightSettings.direction), 6.0f,
                             glm::vec4(1.00f, 0.55f, 0.10f, 0.95f));
  }
  if(params.debugOptions.enablePointLights && params.debugOptions.showPointLights)
  {
    float markerRadius = 0.18f;
    if(m_frameLightingState.sceneBounds.valid)
    {
      const glm::vec3 boundsSize = glm::max(m_frameLightingState.sceneBounds.max - m_frameLightingState.sceneBounds.min,
                                            glm::vec3(1.0f));
      markerRadius = glm::clamp(glm::length(boundsSize) * 0.0025f, 0.18f, 1.5f);
    }
    for(const shaderio::LightData& light : m_testPointLights)
    {
      const glm::vec4 color(light.color, 0.85f);
      m_debugDrawList.addSphere(light.positionOrDirection, markerRadius, color, 12);
      m_debugDrawList.addSphere(light.positionOrDirection, light.range, glm::vec4(light.color, 0.22f), 24);
    }
  }
  if(params.debugOptions.showCullDistance && params.cameraUniforms != nullptr)
  {
    m_debugDrawList.addSphere(params.cameraUniforms->cameraPosition, params.debugOptions.cullDistance,
                              glm::vec4(0.95f, 0.20f, 0.30f, 0.70f), 48);
  }
}

void RenderDevice::prebuildRequiredPipelineVariants()
{
  createPrebuiltGraphicsPipelineVariants();
  createPrebuiltComputePipelineVariant();
  createGPUCullingPipeline();
  createShadowCullingPipeline();
}

void RenderDevice::createPrebuiltGraphicsPipelineVariants()
{
#ifdef USE_SLANG
  const char* vertEntryName = "vertexMain";
  const char* fragEntryName = "fragmentMain";

  VkShaderModule vertShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                              {shader_rast_slang, std::size(shader_rast_slang)});
  DBG_VK_NAME(vertShaderModule);
  VkShaderModule fragShaderModule = vertShaderModule;
#else
  const char* vertEntryName = "main";
  const char* fragEntryName = "main";

  VkShaderModule vertShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                              {shader_vert_glsl, std::size(shader_vert_glsl)});
  DBG_VK_NAME(vertShaderModule);
  VkShaderModule fragShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                              {shader_frag_glsl, std::size(shader_frag_glsl)});
  DBG_VK_NAME(fragShaderModule);
#endif

  const auto bindingDescription    = Vertex::getBindingDescription();
  const auto attributeDescriptions = Vertex::getAttributeDescriptions();

  ASSERT(!m_perFrame.frameUserData.empty(), "Per-frame resources must exist before graphics pipeline creation");
  const rhi::vulkan::ArgumentTableRecord* materialTableRecord =
      m_device.resourceTable.tryGetArgumentTable(m_materials.materialArgumentTable);
  const rhi::vulkan::ArgumentTableRecord* sceneTableRecord =
      m_device.resourceTable.tryGetArgumentTable(m_perFrame.frameUserData.front().sceneArgumentTable);
  ASSERT(materialTableRecord != nullptr && sceneTableRecord != nullptr, "Prebuilt graphics pipelines require argument tables");

  const rhi::ShaderReflectionData rasterReflection = buildRasterShaderReflection();
  const std::vector<rhi::PipelinePushConstantRange> pushConstantRanges =
      rhi::derivePipelinePushConstantRanges(rasterReflection);
  const std::array<rhi::ArgumentLayoutHandle, 2> graphicsArgumentLayouts{{
      materialTableRecord->layout,
      sceneTableRecord->layout,
  }};

  const std::array<rhi::VertexBindingDesc, 1>   vertexBindings{{
      rhi::VertexBindingDesc{
          .binding   = bindingDescription[0].binding,
          .stride    = bindingDescription[0].stride,
          .inputRate = rhi::VertexInputRate::perVertex,
      },
  }};
  const std::array<rhi::VertexAttributeDesc, 3> vertexAttributes{{
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[0].location,
          .binding  = attributeDescriptions[0].binding,
          .format   = rhi::VertexFormat::r32g32b32Sfloat,
          .offset   = attributeDescriptions[0].offset,
      },
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[1].location,
          .binding  = attributeDescriptions[1].binding,
          .format   = rhi::VertexFormat::r32g32b32Sfloat,
          .offset   = attributeDescriptions[1].offset,
      },
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[2].location,
          .binding  = attributeDescriptions[2].binding,
          .format   = rhi::VertexFormat::r32g32Sfloat,
          .offset   = attributeDescriptions[2].offset,
      },
  }};
  const rhi::VertexInputLayoutDesc              vertexInput{
      .bindings       = vertexBindings.data(),
      .bindingCount   = static_cast<uint32_t>(vertexBindings.size()),
      .attributes     = vertexAttributes.data(),
      .attributeCount = static_cast<uint32_t>(vertexAttributes.size()),
  };

  const std::array<rhi::DynamicState, 2>         dynamicStates{{
      rhi::DynamicState::viewport,
      rhi::DynamicState::scissor,
  }};
  const std::array<rhi::BlendAttachmentState, 1> blendStates{{
      rhi::BlendAttachmentState{
          .blendEnable         = true,
          .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .colorBlendOp        = rhi::BlendOp::add,
          .srcAlphaBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .alphaBlendOp        = rhi::BlendOp::add,
          .colorWriteMask      = rhi::ColorComponentFlags::all,
      },
  }};

  std::array<rhi::PipelineShaderStageDesc, 2> shaderStages{{
      rhi::PipelineShaderStageDesc{
          .stage        = rhi::ShaderStage::vertex,
          .shaderModule = reinterpret_cast<uint64_t>(vertShaderModule),
          .entryPoint   = vertEntryName,
      },
      rhi::PipelineShaderStageDesc{
          .stage                 = rhi::ShaderStage::fragment,
          .shaderModule          = reinterpret_cast<uint64_t>(fragShaderModule),
          .entryPoint            = fragEntryName,
          .specializationVariant = 1,
      },
  }};

  // Specialization constants for useTexture (constant_id = 0)
  // Textured variant: useTexture = true (must be VkBool32 = uint32_t = 4 bytes)
  uint32_t                                useTextureTrue = VK_TRUE;
  rhi::SpecializationConstant             specConstantTrue(0, 0, sizeof(uint32_t));
  rhi::SpecializationData                 specDataTrue{&useTextureTrue, sizeof(uint32_t)};
  shaderStages[1].specializationData         = specDataTrue;
  shaderStages[1].specializationConstants    = &specConstantTrue;
  shaderStages[1].specializationConstantCount = 1;

  const std::array<rhi::TextureFormat, 1> colorFormats{{
      toPortableTextureFormat(m_swapchainDependent.sceneResources.getColorFormat()),
  }};

  rhi::GraphicsPipelineDesc graphicsDesc{
      .shaderStages      = shaderStages.data(),
      .shaderStageCount  = static_cast<uint32_t>(shaderStages.size()),
      .vertexInput       = vertexInput,
      .rasterState       = rhi::RasterState{},
      .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greaterOrEqual},
      .blendStates       = blendStates.data(),
      .blendStateCount   = static_cast<uint32_t>(blendStates.size()),
      .dynamicStates     = dynamicStates.data(),
      .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
      .renderingInfo =
          {
              .colorFormats     = colorFormats.data(),
              .colorFormatCount = static_cast<uint32_t>(colorFormats.size()),
              .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
          },
      .argumentLayouts     = graphicsArgumentLayouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(graphicsArgumentLayouts.size()),
      .pushConstantRanges  = pushConstantRanges.data(),
      .pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size()),
  };
  graphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
  graphicsDesc.rasterState.cullMode    = rhi::CullMode::none;
  graphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
  graphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

  graphicsDesc.specializationVariant = 1;
  const PipelineHandle graphicsPipelineWithTexture = m_device.device->createGraphicsPipeline(graphicsDesc);
  m_device.prebuiltPipelines.graphicsTextured = graphicsPipelineWithTexture;

  graphicsDesc.specializationVariant          = 0;
  shaderStages[1].specializationVariant        = 0;
  // Non-textured variant: useTexture = false (must be VkBool32 = uint32_t = 4 bytes)
  uint32_t useTextureFalse = VK_FALSE;
  rhi::SpecializationConstant specConstantFalse(0, 0, sizeof(uint32_t));
  rhi::SpecializationData     specDataFalse{&useTextureFalse, sizeof(uint32_t)};
  shaderStages[1].specializationData         = specDataFalse;
  shaderStages[1].specializationConstants    = &specConstantFalse;
  shaderStages[1].specializationConstantCount = 1;
  const PipelineHandle graphicsPipelineWithoutTexture = m_device.device->createGraphicsPipeline(graphicsDesc);
  m_device.prebuiltPipelines.graphicsNonTextured = graphicsPipelineWithoutTexture;

  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), vertShaderModule, nullptr);
#ifndef USE_SLANG
  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), fragShaderModule, nullptr);
#endif

  // Create GBuffer pipeline layout with uniform buffer bindings
  {
    // Set 1: Camera uniform buffer layout (dynamic)
    std::vector<rhi::ArgumentBinding> cameraLayoutEntries{
        makeArgumentBinding(shaderio::LBindCamera, rhi::BindlessResourceType::uniformBufferDynamic, 1, rhi::ResourceVisibility::allGraphics),
        makeArgumentBinding(shaderio::LBindLighting, rhi::BindlessResourceType::uniformBuffer, 1, rhi::ResourceVisibility::fragment),
        makeArgumentBinding(shaderio::LBindLightCulling, rhi::BindlessResourceType::uniformBuffer, 1, rhi::ResourceVisibility::compute),
        makeArgumentBinding(shaderio::LBindPostProcess, rhi::BindlessResourceType::uniformBufferDynamic, 1, rhi::ResourceVisibility::fragment),
    };

    // One shared camera ArgumentLayout, a per-frame ArgumentTable. Dynamic-ness of the
    // camera/post-process UBOs comes from the layout (dynamicBindings), so the writes use
    // the plain uniformBuffer type and updateArgumentTable emits the *_DYNAMIC descriptor.
    const rhi::ArgumentLayoutHandle cameraLayout = createArgumentLayoutFromBindings(cameraLayoutEntries, "gbuffer-camera");
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      PerFrameResources::FrameUserData& fud         = m_perFrame.frameUserData[i];
      const rhi::ArgumentTableHandle    cameraTable = m_device.device->createArgumentTable(cameraLayout);
      fud.cameraArgumentTable                           = createArgumentTable(ArgumentTableDesc{
                                    .slot                = ArgumentSlot::shaderSpecific,
                                    .layout              = cameraLayout,
                                    .table               = cameraTable,
                                    .primaryLogicalIndex = shaderio::LBindCamera,
                                    .debugName           = "gbuffer-camera",
      });

      const std::array<rhi::ArgumentWrite, 4> cameraWrites{{
          rhi::ArgumentWrite{.binding = shaderio::LBindCamera, .type = rhi::ArgumentType::uniformBuffer, .buffer = fud.transientBufferRHI, .offset = 0, .size = sizeof(shaderio::CameraUniforms)},
          rhi::ArgumentWrite{.binding = shaderio::LBindLighting, .type = rhi::ArgumentType::uniformBuffer, .buffer = fud.lightingBufferRHI, .offset = 0, .size = sizeof(shaderio::LightingUniforms)},
          rhi::ArgumentWrite{.binding = shaderio::LBindLightCulling, .type = rhi::ArgumentType::uniformBuffer, .buffer = fud.lightCullingBufferRHI, .offset = 0, .size = sizeof(shaderio::LightCullingUniforms)},
          rhi::ArgumentWrite{.binding = shaderio::LBindPostProcess, .type = rhi::ArgumentType::uniformBuffer, .buffer = fud.transientBufferRHI, .offset = 0, .size = sizeof(shaderio::PostProcessUniforms)},
      }};
      m_device.device->updateArgumentTable(cameraTable, static_cast<uint32_t>(cameraWrites.size()), cameraWrites.data());
    }

    // Set 2: Draw uniform buffer (dynamic). One shared ArgumentLayout, per-frame table.
    std::vector<rhi::ArgumentBinding> drawLayoutEntries{
        makeArgumentBinding(shaderio::LBindDrawModel, rhi::BindlessResourceType::uniformBufferDynamic, 1, rhi::ResourceVisibility::vertex | rhi::ResourceVisibility::fragment),
    };
    const rhi::ArgumentLayoutHandle drawLayout    = createArgumentLayoutFromBindings(drawLayoutEntries, "gbuffer-draw");
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      PerFrameResources::FrameUserData& fud       = m_perFrame.frameUserData[i];
      const rhi::ArgumentTableHandle    drawTable = m_device.device->createArgumentTable(drawLayout);
      fud.drawArgumentTable                           = createArgumentTable(ArgumentTableDesc{
                                    .slot                = ArgumentSlot::shaderSpecific,
                                    .layout              = drawLayout,
                                    .table               = drawTable,
                                    .primaryLogicalIndex = shaderio::LBindDrawModel,
                                    .debugName           = "gbuffer-draw",
      });
      const rhi::ArgumentWrite drawWrite{.binding = shaderio::LBindDrawModel, .type = rhi::ArgumentType::uniformBuffer, .buffer = fud.transientBufferRHI, .offset = 0, .size = sizeof(shaderio::DrawUniforms)};
      m_device.device->updateArgumentTable(drawTable, 1, &drawWrite);
    }

    // MDI draw-data: one shared storage-buffer ArgumentLayout for the three per-frame MDI
    // variants and all CSM cascades. Descriptor sets are written lazily by the
    // ensure*MdiDrawDataBuffer / updateShadowCullingDrawDataArgumentTable paths.
    std::vector<rhi::ArgumentBinding> shadowMdiDrawLayoutEntries{
        makeArgumentBinding(shaderio::LBindDrawModelMdi, rhi::BindlessResourceType::storageBuffer, 1, rhi::ResourceVisibility::vertex | rhi::ResourceVisibility::fragment),
    };
    const rhi::ArgumentLayoutHandle mdiDrawLayout    = createArgumentLayoutFromBindings(shadowMdiDrawLayoutEntries, "mdi-draw");
    const auto makeMdiTable = [&](const char* name) {
      return createArgumentTable(ArgumentTableDesc{
          .slot                = ArgumentSlot::shaderSpecific,
          .layout              = mdiDrawLayout,
          .table               = m_device.device->createArgumentTable(mdiDrawLayout),
          .primaryLogicalIndex = shaderio::LBindDrawModelMdi,
          .debugName           = name,
      });
    };
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      PerFrameResources::FrameUserData& fud = m_perFrame.frameUserData[i];
      fud.mdiDrawArgumentTable                  = makeMdiTable("mdi-draw");
      fud.gbufferMdiDrawArgumentTable           = makeMdiTable("gbuffer-mdi-draw");
      fud.depthMdiDrawArgumentTable             = makeMdiTable("depth-mdi-draw");
      for(uint32_t cascadeIndex = 0; cascadeIndex < shaderio::LCascadeCount; ++cascadeIndex)
      {
        fud.csmShadowMdiDrawArgumentTables[cascadeIndex] = makeMdiTable("csm-shadow-mdi-draw");
      }

      updateMdiDrawDataArgumentTable(i);
      updateGBufferMdiDrawDataArgumentTable(i);
      updateDepthMdiDrawDataArgumentTable(i);
      updateShadowCullingDrawDataArgumentTable(i);
    }

    const rhi::vulkan::ArgumentTableRecord* materialLayoutRecord =
        m_device.resourceTable.tryGetArgumentTable(m_materials.materialArgumentTable);
    ASSERT(materialLayoutRecord != nullptr, "Material argument table must exist before graphics pipelines");
    m_device.gbufferArgumentLayouts = {materialLayoutRecord->layout, cameraLayout, drawLayout};
    m_device.mdiArgumentLayouts = {materialLayoutRecord->layout, cameraLayout, mdiDrawLayout};
    m_device.csmShadowMdiArgumentLayouts = {materialLayoutRecord->layout, cameraLayout, mdiDrawLayout};
    m_device.debugArgumentLayouts = {materialLayoutRecord->layout, cameraLayout};
    m_device.fullscreenArgumentLayouts = {materialLayoutRecord->layout, cameraLayout};
  }

  // Create depth prepass pipelines (Opaque + AlphaTest variants)
  {
    VkShaderModule depthPrepassShaderModule =
        utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                  {shader_depth_prepass_slang, std::size(shader_depth_prepass_slang)});
    DBG_VK_NAME(depthPrepassShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> depthBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 4> depthAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};
    const rhi::VertexInputLayoutDesc depthVertexInput{
        .bindings       = depthBindings.data(),
        .bindingCount   = static_cast<uint32_t>(depthBindings.size()),
        .attributes     = depthAttributes.data(),
        .attributeCount = static_cast<uint32_t>(depthAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> depthDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const std::array<rhi::PipelineShaderStageDesc, 2> depthStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "fragmentMain"},
    }};
    const std::array<rhi::PipelineShaderStageDesc, 2> depthMdiStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "vertexMdiMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "fragmentMdiMain"},
    }};

    rhi::GraphicsPipelineDesc depthDesc{
        .shaderStages      = depthStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(depthStages.size()),
        .vertexInput       = depthVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greater},
        .blendStates       = nullptr,
        .blendStateCount   = 0,
        .dynamicStates     = depthDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(depthDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = nullptr,
                .colorFormatCount = 0,
                .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
            },
        .argumentLayouts = m_device.gbufferArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.gbufferArgumentLayouts.size()),
    };
    depthDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    depthDesc.rasterState.cullMode    = rhi::CullMode::back;
    depthDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    depthDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(uint32_t));
    std::array<rhi::PipelineShaderStageDesc, 2> depthShaderStages = depthStages;
    depthDesc.shaderStages = depthShaderStages.data();

    uint32_t alphaTestFalse = VK_FALSE;
    rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(uint32_t)};
    depthShaderStages[1].specializationData = specDataFalse;
    depthShaderStages[1].specializationConstants = &specConstantAlphaTest;
    depthShaderStages[1].specializationConstantCount = 1;

    depthDesc.specializationVariant = 10;
    m_depthPrepassOpaquePipeline = m_device.device->createGraphicsPipeline(depthDesc);

    uint32_t alphaTestTrue = VK_TRUE;
    rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(uint32_t)};
    depthShaderStages[1].specializationData = specDataTrue;
    depthDesc.specializationVariant = 11;
    m_depthPrepassAlphaTestPipeline = m_device.device->createGraphicsPipeline(depthDesc);

    if(!m_device.mdiArgumentLayouts[0].isNull())
    {
      rhi::GraphicsPipelineDesc depthMdiDesc = depthDesc;
      std::array<rhi::PipelineShaderStageDesc, 2> depthMdiShaderStages = depthMdiStages;
      depthMdiDesc.shaderStages = depthMdiShaderStages.data();
      depthMdiDesc.argumentLayouts = m_device.mdiArgumentLayouts.data();
      depthMdiDesc.argumentLayoutCount = static_cast<uint32_t>(m_device.mdiArgumentLayouts.size());

      depthMdiShaderStages[1].specializationData = specDataFalse;
      depthMdiShaderStages[1].specializationConstants = &specConstantAlphaTest;
      depthMdiShaderStages[1].specializationConstantCount = 1;
      depthMdiDesc.specializationVariant = 13;
    m_depthPrepassOpaqueMDIPipeline = m_device.device->createGraphicsPipeline(depthMdiDesc);

      depthMdiShaderStages[1].specializationData = specDataTrue;
      depthMdiDesc.specializationVariant = 14;
    m_depthPrepassAlphaTestMDIPipeline = m_device.device->createGraphicsPipeline(depthMdiDesc);
    }

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), depthPrepassShaderModule, nullptr);
  }

  // Create GBuffer pipelines (Opaque + AlphaTest variants)
  {
    VkShaderModule gbufferShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                                {shader_gbuffer_slang, std::size(shader_gbuffer_slang)});
    DBG_VK_NAME(gbufferShaderModule);

    // GBuffer vertex input: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    const std::array<rhi::VertexBindingDesc, 1> gbufferBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> gbufferAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},    // Position
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},   // Normal
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},   // TexCoord
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32}, // Tangent
    }};

    const rhi::VertexInputLayoutDesc gbufferVertexInput{
        .bindings       = gbufferBindings.data(),
        .bindingCount   = static_cast<uint32_t>(gbufferBindings.size()),
        .attributes     = gbufferAttributes.data(),
        .attributeCount = static_cast<uint32_t>(gbufferAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> gbufferDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // No blending for GBuffer
    const std::array<rhi::BlendAttachmentState, kPackedGBufferTargetCount> gbufferBlendStates{{
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
    }};

    // Packed GBuffer formats: 3 color attachments + depth
    const std::array<rhi::TextureFormat, kPackedGBufferTargetCount> gbufferColorFormats{{
        rhi::TextureFormat::rgba8Unorm,  // GBuffer0: BaseColor.rgb + Roughness
        rhi::TextureFormat::rgba8Unorm,  // GBuffer1: OctNormal.xy + Metallic + AO
        rhi::TextureFormat::rgba16Sfloat,  // GBuffer2: Emissive.rgb + flags
    }};

    // Specialization constant for alpha test (must be VkBool32 = uint32_t = 4 bytes)
    rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(uint32_t));

    std::array<rhi::PipelineShaderStageDesc, 2> gbufferShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};
    std::array<rhi::PipelineShaderStageDesc, 2> gbufferMdiShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "vertexMdiMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "fragmentMdiMain",
        },
    }};

    rhi::GraphicsPipelineDesc gbufferGraphicsDesc{
        .shaderStages      = gbufferShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(gbufferShaderStages.size()),
        .vertexInput       = gbufferVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::greaterOrEqual},
        .blendStates       = gbufferBlendStates.data(),
        .blendStateCount   = static_cast<uint32_t>(gbufferBlendStates.size()),
        .dynamicStates     = gbufferDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(gbufferDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = gbufferColorFormats.data(),
                .colorFormatCount = static_cast<uint32_t>(gbufferColorFormats.size()),
                .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
            },
        .argumentLayouts = m_device.gbufferArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.gbufferArgumentLayouts.size()),
    };
    gbufferGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    gbufferGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    gbufferGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    gbufferGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    // Variant 0: Opaque (alphaTestEnabled = false)
    uint32_t alphaTestFalse = VK_FALSE;
    rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(uint32_t)};
    gbufferShaderStages[1].specializationData = specDataFalse;
    gbufferShaderStages[1].specializationConstants = &specConstantAlphaTest;
    gbufferShaderStages[1].specializationConstantCount = 1;

    gbufferGraphicsDesc.specializationVariant = 3;  // GBuffer Opaque variant
    m_gbufferOpaquePipeline = m_device.device->createGraphicsPipeline(gbufferGraphicsDesc);

    // Variant 1: AlphaTest (alphaTestEnabled = true)
    uint32_t alphaTestTrue = VK_TRUE;
    rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(uint32_t)};
    gbufferShaderStages[1].specializationData = specDataTrue;
    gbufferGraphicsDesc.specializationVariant = 4;  // GBuffer AlphaTest variant
    m_gbufferAlphaTestPipeline = m_device.device->createGraphicsPipeline(gbufferGraphicsDesc);

    if(!m_device.mdiArgumentLayouts[0].isNull())
    {
      rhi::GraphicsPipelineDesc gbufferMdiGraphicsDesc = gbufferGraphicsDesc;
      gbufferMdiGraphicsDesc.shaderStages = gbufferMdiShaderStages.data();
      gbufferMdiGraphicsDesc.argumentLayouts = m_device.mdiArgumentLayouts.data();
      gbufferMdiGraphicsDesc.argumentLayoutCount = static_cast<uint32_t>(m_device.mdiArgumentLayouts.size());

      gbufferMdiShaderStages[1].specializationData = specDataFalse;
      gbufferMdiShaderStages[1].specializationConstants = &specConstantAlphaTest;
      gbufferMdiShaderStages[1].specializationConstantCount = 1;
      gbufferMdiGraphicsDesc.specializationVariant = 15;
    m_gbufferOpaqueMDIPipeline = m_device.device->createGraphicsPipeline(gbufferMdiGraphicsDesc);

      gbufferMdiShaderStages[1].specializationData = specDataTrue;
      gbufferMdiGraphicsDesc.specializationVariant = 16;
    m_gbufferAlphaTestMDIPipeline = m_device.device->createGraphicsPipeline(gbufferMdiGraphicsDesc);
    }

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), gbufferShaderModule, nullptr);
  }

  // Create Shadow pipeline
  {
    VkShaderModule shadowShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                                  {shader_shadow_slang, std::size(shader_shadow_slang)});
    DBG_VK_NAME(shadowShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> shadowBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 4> shadowAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat, .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};
    const rhi::VertexInputLayoutDesc shadowVertexInput{
        .bindings       = shadowBindings.data(),
        .bindingCount   = static_cast<uint32_t>(shadowBindings.size()),
        .attributes     = shadowAttributes.data(),
        .attributeCount = static_cast<uint32_t>(shadowAttributes.size()),
    };
    const std::array<rhi::DynamicState, 2> shadowDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const std::array<rhi::PipelineShaderStageDesc, 2> shadowStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "fragmentMain"},
    }};
    rhi::GraphicsPipelineDesc shadowDesc{
        .shaderStages      = shadowStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(shadowStages.size()),
        .vertexInput       = shadowVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greaterOrEqual},
        .blendStates       = nullptr,
        .blendStateCount   = 0,
        .dynamicStates     = shadowDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(shadowDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = nullptr,
                .colorFormatCount = 0,
                .depthFormat      = toPortableTextureFormat(m_csmShadowResources.getShadowFormat()),
            },
        .argumentLayouts = m_device.gbufferArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.gbufferArgumentLayouts.size()),
    };
    shadowDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    shadowDesc.rasterState.cullMode    = rhi::CullMode::none;
    shadowDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    shadowDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    shadowDesc.specializationVariant = 6;
    m_shadowPipeline = m_device.device->createGraphicsPipeline(shadowDesc);

    std::array<rhi::PipelineShaderStageDesc, 2> shadowMdiStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "vertexMdiMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "fragmentMdiMain"},
    }};
    rhi::GraphicsPipelineDesc shadowMdiDesc = shadowDesc;
    shadowMdiDesc.shaderStages = shadowMdiStages.data();
    shadowMdiDesc.argumentLayouts = m_device.csmShadowMdiArgumentLayouts.data();
    shadowMdiDesc.argumentLayoutCount = static_cast<uint32_t>(m_device.csmShadowMdiArgumentLayouts.size());

    shadowMdiDesc.specializationVariant = 7;
    m_csmShadowPipeline = m_device.device->createGraphicsPipeline(shadowMdiDesc);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), shadowShaderModule, nullptr);
  }

  // Create Forward pipeline for transparent objects
  {
    VkShaderModule forwardShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                                {shader_forward_slang, std::size(shader_forward_slang)});
    DBG_VK_NAME(forwardShaderModule);

    // Same vertex input as GBuffer
    const std::array<rhi::VertexBindingDesc, 1> forwardBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> forwardAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};

    const rhi::VertexInputLayoutDesc forwardVertexInput{
        .bindings       = forwardBindings.data(),
        .bindingCount   = static_cast<uint32_t>(forwardBindings.size()),
        .attributes     = forwardAttributes.data(),
        .attributeCount = static_cast<uint32_t>(forwardAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> forwardDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // Alpha blending for transparent objects
    const rhi::BlendAttachmentState forwardBlend{
        .blendEnable = true,
        .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
        .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .colorBlendOp = rhi::BlendOp::add,
        .srcAlphaBlendFactor = rhi::BlendFactor::one,
        .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .alphaBlendOp = rhi::BlendOp::add,
        .colorWriteMask = rhi::ColorComponentFlags::all,
    };

    std::array<rhi::PipelineShaderStageDesc, 2> forwardShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};
    std::array<rhi::PipelineShaderStageDesc, 2> forwardMdiShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "vertexMdiMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "fragmentMdiMain",
        },
    }};

    // Render to swapchain format
    const rhi::TextureFormat swapchainFormat = m_swapchainDependent.swapchainImageFormat;
    const rhi::TextureFormat hdrSceneColorFormat = toPortableTextureFormat(SceneResources::kSceneColorHdrFormat);
    const rhi::TextureFormat depthFormat = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat());

    rhi::GraphicsPipelineDesc forwardGraphicsDesc{
        .shaderStages      = forwardShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(forwardShaderStages.size()),
        .vertexInput       = forwardVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::greaterOrEqual},  // Test enabled, write disabled
        .blendStates       = &forwardBlend,
        .blendStateCount   = 1,
        .dynamicStates     = forwardDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(forwardDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &swapchainFormat,
                .colorFormatCount = 1,
                .depthFormat      = depthFormat,
            },
        .argumentLayouts = m_device.gbufferArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.gbufferArgumentLayouts.size()),
    };
    forwardGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    forwardGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    forwardGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    forwardGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    forwardGraphicsDesc.specializationVariant = 5;  // Forward variant
    m_forwardPipeline = m_device.device->createGraphicsPipeline(forwardGraphicsDesc);

    if(!m_device.mdiArgumentLayouts[0].isNull())
    {
      rhi::GraphicsPipelineDesc forwardMdiGraphicsDesc = forwardGraphicsDesc;
      forwardMdiGraphicsDesc.shaderStages = forwardMdiShaderStages.data();
      forwardMdiGraphicsDesc.renderingInfo.colorFormats = &hdrSceneColorFormat;
      forwardMdiGraphicsDesc.argumentLayouts = m_device.mdiArgumentLayouts.data();
      forwardMdiGraphicsDesc.argumentLayoutCount = static_cast<uint32_t>(m_device.mdiArgumentLayouts.size());

      forwardMdiGraphicsDesc.specializationVariant = 17;
    m_forwardMDIPipeline = m_device.device->createGraphicsPipeline(forwardMdiGraphicsDesc);
    }

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), forwardShaderModule, nullptr);
  }

  // Create Debug line pipeline
  {
    VkShaderModule debugShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                                 {shader_debug_slang, std::size(shader_debug_slang)});
    DBG_VK_NAME(debugShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> debugBindings{{
        {.binding = 0, .stride = sizeof(shaderio::DebugLineVertex), .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 2> debugAttributes{{
        {.location = shaderio::LVDebugPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat,
         .offset = static_cast<uint32_t>(offsetof(shaderio::DebugLineVertex, position))},
        {.location = shaderio::LVDebugColor, .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat,
         .offset = static_cast<uint32_t>(offsetof(shaderio::DebugLineVertex, color))},
    }};
    const rhi::VertexInputLayoutDesc debugVertexInput{
        .bindings       = debugBindings.data(),
        .bindingCount   = static_cast<uint32_t>(debugBindings.size()),
        .attributes     = debugAttributes.data(),
        .attributeCount = static_cast<uint32_t>(debugAttributes.size()),
    };
    const std::array<rhi::DynamicState, 2> debugDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const rhi::BlendAttachmentState debugBlend{
        .blendEnable         = true,
        .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
        .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .colorBlendOp        = rhi::BlendOp::add,
        .srcAlphaBlendFactor = rhi::BlendFactor::one,
        .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .alphaBlendOp        = rhi::BlendOp::add,
        .colorWriteMask      = rhi::ColorComponentFlags::all,
    };
    const std::array<rhi::PipelineShaderStageDesc, 2> debugStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "fragmentMain"},
    }};
    const std::array<rhi::PipelinePushConstantRange, 1> debugPushConstants{{
        rhi::PipelinePushConstantRange{
            .stages = rhi::ShaderStage::vertex,
            .offset = 0,
            .size   = sizeof(shaderio::PushConstantGPUCullDebug),
        },
    }};
    const rhi::TextureFormat outputFormat = toPortableTextureFormat(VK_FORMAT_B8G8R8A8_UNORM);
    rhi::GraphicsPipelineDesc debugDesc{
        .shaderStages      = debugStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(debugStages.size()),
        .vertexInput       = debugVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{false, false, rhi::CompareOp::always},
        .blendStates       = &debugBlend,
        .blendStateCount   = 1,
        .dynamicStates     = debugDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(debugDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &outputFormat,
                .colorFormatCount = 1,
                .depthFormat      = rhi::TextureFormat::undefined,
        },
        .argumentLayouts = m_device.gbufferArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.gbufferArgumentLayouts.size()),
        .pushConstantRanges  = debugPushConstants.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(debugPushConstants.size()),
    };
    debugDesc.rasterState.topology    = rhi::PrimitiveTopology::lineList;
    debugDesc.rasterState.cullMode    = rhi::CullMode::none;
    debugDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    debugDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    debugDesc.specializationVariant = 7;
    m_debugPipeline = m_device.device->createGraphicsPipeline(debugDesc);

    const std::array<rhi::PipelineShaderStageDesc, 2> debugCullStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "vertexCullMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "fragmentMain"},
    }};
    rhi::GraphicsPipelineDesc debugCullDesc{
        .shaderStages      = debugCullStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(debugCullStages.size()),
        .vertexInput       = {},
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{false, false, rhi::CompareOp::always},
        .blendStates       = &debugBlend,
        .blendStateCount   = 1,
        .dynamicStates     = debugDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(debugDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &outputFormat,
                .colorFormatCount = 1,
                .depthFormat      = rhi::TextureFormat::undefined,
        },
        .argumentLayouts = m_device.debugArgumentLayouts.data(),
        .argumentLayoutCount = static_cast<uint32_t>(m_device.debugArgumentLayouts.size()),
        .pushConstantRanges  = debugPushConstants.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(debugPushConstants.size()),
    };
    debugCullDesc.rasterState.topology    = rhi::PrimitiveTopology::lineList;
    debugCullDesc.rasterState.cullMode    = rhi::CullMode::none;
    debugCullDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    debugCullDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    debugCullDesc.specializationVariant = 8;
    m_gpuCullingDebugPipeline = m_device.device->createGraphicsPipeline(debugCullDesc);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), debugShaderModule, nullptr);
  }
}

void RenderDevice::initImGui(void* window)
{
  LOGI("RenderDevice::initImGui: begin");
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

#ifdef __ANDROID__
  ImGui_ImplAndroid_Init(static_cast<ANativeWindow*>(window));
#else
  ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window), true);
#endif
  VkFormat                  imageFormats[] = {toVkFormat(m_swapchainDependent.swapchainImageFormat)};
  const rhi::QueueInfo      graphicsQueue  = m_device.device->getGraphicsQueue();
  ImGui_ImplVulkan_InitInfo initInfo       = {
      .Instance       = fromNativeHandle<VkInstance>(m_device.device->getBackendInstanceHandle()),
      .PhysicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle()),
      .Device         = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
      .QueueFamily    = graphicsQueue.familyIndex,
      .Queue          = fromNativeHandle<VkQueue>(graphicsQueue.backendHandle),
      .DescriptorPool = m_device.uiDescriptorPool,
      .MinImageCount  = 2,
      .ImageCount     = m_swapchainDependent.swapchain->getMaxFramesInFlight(),
      .PipelineInfoMain =
          {
              .PipelineRenderingCreateInfo =
                  {
                      .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                      .colorAttachmentCount    = 1,
                      .pColorAttachmentFormats = imageFormats,
                  },
          },
      .UseDynamicRendering = true,
  };

  initInfo.PipelineInfoForViewports = initInfo.PipelineInfoMain;

  ImGui_ImplVulkan_Init(&initInfo);
  LOGI("RenderDevice::initImGui: Vulkan backend initialized");

  ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable;
  LOGI("RenderDevice::initImGui: completed");
}

void RenderDevice::createDescriptorPool()
{
  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle()), &deviceProperties2);
  const auto& deviceProperties = deviceProperties2.properties;

  {
    m_materials.maxTextures = std::min(m_materials.maxTextures, deviceProperties.limits.maxDescriptorSetSampledImages);
    const uint32_t maxDescriptorSets = 32U;
    const uint32_t dynamicUniformCount =
        std::max(1U, std::min(maxDescriptorSets, deviceProperties.limits.maxDescriptorSetUniformBuffersDynamic));
    const uint32_t frameCount = std::max<uint32_t>(1U, static_cast<uint32_t>(m_perFrame.frameUserData.size()));
    const std::array<VkDescriptorPoolSize, 6> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_materials.maxTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4U + frameCount * shaderio::LDepthPyramidMaxMips},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, shaderio::LDepthPyramidMaxMips},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, dynamicUniformCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8U + frameCount * 5U},  // Scene, light, culling, and LightPass fallback UBOs
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 24U},
    }};
    const VkDescriptorPoolCreateInfo          poolInfo = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = maxDescriptorSets,
        .poolSizeCount = uint32_t(poolSizes.size()),
        .pPoolSizes    = poolSizes.data(),
    };
    VK_CHECK(vkCreateDescriptorPool(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), &poolInfo, nullptr,
                                    &m_device.descriptorPool));
    DBG_VK_NAME(m_device.descriptorPool);
    LOGI("Created application descriptor pool: %u textures, %u dynamic UBOs, %u sets", m_materials.maxTextures,
         dynamicUniformCount, maxDescriptorSets);
  }

  {
    constexpr uint32_t uiTextureDescriptorCount = 20U;
    constexpr uint32_t uiSamplerDescriptorCount = 2U;
    constexpr uint32_t uiMaxDescriptorSets      = uiTextureDescriptorCount + uiSamplerDescriptorCount;
    const std::array<VkDescriptorPoolSize, 3> poolSizes{{
        // Dear ImGui Vulkan backend switched to separate sampled-image + sampler
        // descriptor sets in 2026. Keep combined-image-sampler capacity too so the
        // pool remains compatible with older backend revisions in existing build trees.
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, uiTextureDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, uiSamplerDescriptorCount},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uiTextureDescriptorCount},
    }};
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = uiMaxDescriptorSets,
        .poolSizeCount = uint32_t(poolSizes.size()),
        .pPoolSizes    = poolSizes.data(),
    };

    VK_CHECK(vkCreateDescriptorPool(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), &poolInfo, nullptr,
                                    &m_device.uiDescriptorPool));
    DBG_VK_NAME(m_device.uiDescriptorPool);
    LOGI("Created UI descriptor pool: %u sampled images, %u samplers, %u sets", uiTextureDescriptorCount,
         uiSamplerDescriptorCount, uiMaxDescriptorSets);
  }
}

static void writeHostVisibleBuffer(const VmaAllocator allocator, utils::Buffer& buffer, const void* data, const VkDeviceSize size)
{
  if(buffer.allocation == nullptr || data == nullptr || size == 0)
  {
    return;
  }

  void* mappedData = buffer.mapped;
  bool  mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(allocator, buffer.allocation, &mappedData));
    mappedHere = true;
  }

  std::memcpy(mappedData, data, static_cast<size_t>(size));
  VK_CHECK(vmaFlushAllocation(allocator, buffer.allocation, 0, size));

  if(mappedHere)
  {
    vmaUnmapMemory(allocator, buffer.allocation);
  }
}

static void writeHostVisibleBufferRange(const VmaAllocator allocator,
                                        utils::Buffer&      buffer,
                                        const VkDeviceSize  offset,
                                        const void*         data,
                                        const VkDeviceSize  size)
{
  if(buffer.allocation == nullptr || data == nullptr || size == 0)
  {
    return;
  }

  void* mappedData = buffer.mapped;
  bool  mappedHere = false;
  if(mappedData == nullptr)
  {
    VK_CHECK(vmaMapMemory(allocator, buffer.allocation, &mappedData));
    mappedHere = true;
  }

  std::byte* dst = static_cast<std::byte*>(mappedData) + offset;
  std::memcpy(dst, data, static_cast<size_t>(size));
  VK_CHECK(vmaFlushAllocation(allocator, buffer.allocation, offset, size));

  if(mappedHere)
  {
    vmaUnmapMemory(allocator, buffer.allocation);
  }
}

void RenderDevice::createMaterialArgumentTable()
{
  // Create per-frame material bind groups early - needed for pipeline layout creation
  std::vector<rhi::ArgumentBinding> layoutEntries{makeArgumentBinding(kMaterialBindlessTexturesIndex, rhi::BindlessResourceType::sampledTexture, m_materials.maxTextures, rhi::ResourceVisibility::allGraphics)};

  const uint32_t frameCount = std::max<uint32_t>(1u, static_cast<uint32_t>(m_perFrame.frameUserData.size()));
  m_materials.materialArgumentTables.clear();
  m_materials.materialArgumentTables.reserve(frameCount);
  m_materials.materialArgumentTableGenerations.assign(frameCount, 0);
  m_materials.materialDescriptorViews.assign(m_materials.maxTextures, {});
  m_materials.materialDescriptorValid.assign(m_materials.maxTextures, 0);
  m_materials.materialDescriptorGeneration = 0;

  const rhi::ArgumentLayoutHandle materialLayout = createArgumentLayoutFromBindings(layoutEntries, "material-texture-bind-group");
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    m_materials.materialArgumentTables.push_back(createArgumentTable(ArgumentTableDesc{
        .slot                = ArgumentSlot::material,
        .layout              = materialLayout,
        .table               = m_device.device->createArgumentTable(materialLayout),
        .primaryLogicalIndex = kMaterialBindlessTexturesIndex,
        .debugName           = "material-texture-bind-group",
    }));
  }

  m_materials.materialArgumentTable =
      m_materials.materialArgumentTables.empty() ? rhi::ArgumentTableHandle{} : m_materials.materialArgumentTables.front();
}

void RenderDevice::createGraphicsArgumentTables()
{
  if(m_materials.materialArgumentTables.size() < m_perFrame.frameUserData.size())
  {
    std::vector<rhi::ArgumentBinding> layoutEntries{makeArgumentBinding(kMaterialBindlessTexturesIndex, rhi::BindlessResourceType::sampledTexture, m_materials.maxTextures, rhi::ResourceVisibility::allGraphics)};

    const size_t existingCount = m_materials.materialArgumentTables.size();
    m_materials.materialArgumentTables.reserve(m_perFrame.frameUserData.size());
    m_materials.materialArgumentTableGenerations.resize(m_perFrame.frameUserData.size(), 0);
    const rhi::ArgumentLayoutHandle materialLayout = createArgumentLayoutFromBindings(layoutEntries, "material-texture-bind-group");
    for(size_t frameIndex = existingCount; frameIndex < m_perFrame.frameUserData.size(); ++frameIndex)
    {
      m_materials.materialArgumentTables.push_back(createArgumentTable(ArgumentTableDesc{
          .slot                = ArgumentSlot::material,
          .layout              = materialLayout,
          .table               = m_device.device->createArgumentTable(materialLayout),
          .primaryLogicalIndex = kMaterialBindlessTexturesIndex,
          .debugName           = "material-texture-bind-group",
      }));
    }
  }

  // Create per-frame scene bind groups - must be called after createFrameSubmission()
  {
    std::vector<rhi::ArgumentBinding> layoutEntries{makeArgumentBinding(kSceneBindlessInfoIndex, rhi::BindlessResourceType::uniformBuffer, 1, rhi::ResourceVisibility::allGraphics)};

    const rhi::ArgumentLayoutHandle sceneLayout = createArgumentLayoutFromBindings(layoutEntries, "scene-dynamic-bind-group");
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      m_perFrame.frameUserData[i].sceneArgumentTable = createArgumentTable(ArgumentTableDesc{
          .slot                = ArgumentSlot::drawDynamic,
          .layout              = sceneLayout,
          .table               = m_device.device->createArgumentTable(sceneLayout),
          .primaryLogicalIndex = kSceneBindlessInfoIndex,
          .debugName           = "scene-dynamic-bind-group",
      });
    }
  }
}

void RenderDevice::updateGraphicsArgumentTables()
{
  // Wave 8: shared material sampler for combinedImageSampler ArgumentWrites, created once
  // through the RHI (linear mag/min/mip, repeat, full LOD range).
  if(m_materials.materialSamplerHandle.isNull())
  {
    m_materials.materialSamplerHandle = m_device.device->createSampler(rhi::SamplerDesc{
        .magFilter    = rhi::Filter::linear,
        .minFilter    = rhi::Filter::linear,
        .mipmapMode   = rhi::MipmapMode::linear,
        .addressModeU = rhi::AddressMode::repeat,
        .addressModeV = rhi::AddressMode::repeat,
        .addressModeW = rhi::AddressMode::repeat,
        .maxLod       = VK_LOD_CLAMP_NONE,
        .debugName    = "MaterialSampler",
    });
  }

  // Demo material slots: cache the per-slot view handles and write them as
  // combinedImageSampler ArgumentWrites (shared materialSamplerHandle).
  std::array<rhi::ArgumentWrite, kDemoMaterialSlotCount> materialWrites{};
  for(uint32_t slot = 0; slot < kDemoMaterialSlotCount; ++slot)
  {
    const MaterialHandle                     materialHandle = m_materials.sampleMaterials[slot];
    const MaterialResources::MaterialRecord* material       = tryGetMaterial(materialHandle);
    ASSERT(material != nullptr, "Sample material handle must resolve to an active pool entry");
    ASSERT(material->descriptorIndex < kDemoMaterialSlotCount,
           "Sample material descriptor index must stay in [0, kDemoMaterialSlotCount)");

    const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(material->sampledTexture);
    ASSERT(textureHot != nullptr, "Material texture handle must resolve to an active pool entry");
    ASSERT(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::materialSampled,
           "Material descriptor writes require material-sampled textures");

    m_materials.materialDescriptorViews[material->descriptorIndex] = textureHot->sampledViewHandle;
    m_materials.materialDescriptorValid[material->descriptorIndex] = 1;
    materialWrites[material->descriptorIndex]                      = rhi::ArgumentWrite{
                             .binding      = kMaterialBindlessTexturesIndex,
                             .arrayElement = material->descriptorIndex,
                             .type         = rhi::ArgumentType::combinedImageSampler,
                             .textureView  = textureHot->sampledViewHandle,
                             .sampler      = m_materials.materialSamplerHandle,
    };
  }
  ++m_materials.materialDescriptorGeneration;

  for(uint32_t frameIndex = 0; frameIndex < m_materials.materialArgumentTables.size(); ++frameIndex)
  {
    updateArgumentTable(m_materials.materialArgumentTables[frameIndex], materialWrites.data(),
                    static_cast<uint32_t>(materialWrites.size()));
    m_materials.materialArgumentTableGenerations[frameIndex] = m_materials.materialDescriptorGeneration;
  }

  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    const rhi::ArgumentWrite sceneWrite{
        .binding = kSceneBindlessInfoIndex,
        .type    = rhi::ArgumentType::uniformBuffer,
        .buffer  = frameUserData.transientBufferRHI,
        .offset  = 0,
        .size    = sizeof(shaderio::SceneInfo),
    };
    updateArgumentTable(frameUserData.sceneArgumentTable, &sceneWrite, 1);
  }
}

void RenderDevice::syncMaterialArgumentTable(uint32_t frameIndex)
{
  if(frameIndex >= m_materials.materialArgumentTables.size() || frameIndex >= m_materials.materialArgumentTableGenerations.size())
  {
    return;
  }

  if(m_materials.materialArgumentTableGenerations[frameIndex] == m_materials.materialDescriptorGeneration)
  {
    return;
  }

  // Resolve a safe placeholder view for invalid/stale slots so the GPU never samples a
  // destroyed view after scene switches or partial uploads.
  rhi::TextureViewHandle placeholderViewHandle{};
  if(const MaterialResources::MaterialRecord* placeholderMaterial = tryGetMaterial(m_materials.sampleMaterials[0]))
  {
    if(const MaterialResources::TextureHotData* placeholderTexture = tryGetTextureHot(placeholderMaterial->sampledTexture))
    {
      placeholderViewHandle = placeholderTexture->sampledViewHandle;
    }
  }

  std::vector<rhi::ArgumentWrite> writes;
  writes.reserve(m_materials.materialDescriptorViews.size());
  for(uint32_t index = 0; index < m_materials.materialDescriptorViews.size(); ++index)
  {
    rhi::TextureViewHandle view;
    if(m_materials.materialDescriptorValid[index] == 0)
    {
      if(placeholderViewHandle.isNull())
      {
        continue;
      }
      view = placeholderViewHandle;  // overwrite stale slots with a safe placeholder view
    }
    else
    {
      view = m_materials.materialDescriptorViews[index];
      if(view.isNull())
      {
        continue;
      }
    }
    writes.push_back(rhi::ArgumentWrite{
        .binding      = kMaterialBindlessTexturesIndex,
        .arrayElement = index,
        .type         = rhi::ArgumentType::combinedImageSampler,
        .textureView  = view,
        .sampler      = m_materials.materialSamplerHandle,
    });
  }

  if(!writes.empty())
  {
    updateArgumentTable(m_materials.materialArgumentTables[frameIndex], writes.data(), static_cast<uint32_t>(writes.size()));
  }

  m_materials.materialArgumentTableGenerations[frameIndex] = m_materials.materialDescriptorGeneration;
}

rhi::ArgumentTableHandle RenderDevice::getCurrentMaterialArgumentTable() const
{
  if(m_materials.materialArgumentTables.empty())
  {
    return m_materials.materialArgumentTable;
  }

  uint32_t frameIndex = 0;
  if(m_perFrame.frameContext != nullptr)
  {
    frameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  }
  if(frameIndex >= m_materials.materialArgumentTables.size())
  {
    return m_materials.materialArgumentTables.front();
  }
  return m_materials.materialArgumentTables[frameIndex];
}

// Maps a legacy BindlessResourceType to the new ArgumentType + dynamic-offset flag.
// Note: BindlessResourceType::sampledTexture is a COMBINED_IMAGE_SAMPLER (shaders sample
// via sampler2D), so it maps to ArgumentType::combinedImageSampler, not sampledTexture.
static std::pair<rhi::ArgumentType, bool> toArgumentType(rhi::BindlessResourceType type)
{
  using BR = rhi::BindlessResourceType;
  using AT = rhi::ArgumentType;
  switch(type)
  {
    case BR::sampler:              return {AT::sampler, false};
    case BR::sampledTexture:       return {AT::combinedImageSampler, false};
    case BR::sampledImage:         return {AT::sampledTexture, false};
    case BR::storageTexture:       return {AT::storageTexture, false};
    case BR::uniformBuffer:        return {AT::uniformBuffer, false};
    case BR::storageBuffer:        return {AT::storageBuffer, false};
    case BR::uniformBufferDynamic: return {AT::uniformBuffer, true};
    case BR::storageBufferDynamic: return {AT::storageBuffer, true};
    default:                       return {AT::storageBuffer, false};
  }
}

rhi::ArgumentBinding makeArgumentBinding(uint32_t logicalIndex,
                                         rhi::BindlessResourceType resourceType,
                                         uint32_t descriptorCount,
                                         rhi::ResourceVisibility visibility)
{
  const std::pair<rhi::ArgumentType, bool> mapped = toArgumentType(resourceType);
  return rhi::ArgumentBinding{
      .binding       = logicalIndex,
      .type          = mapped.first,
      .visibility    = static_cast<rhi::ShaderStage>(static_cast<uint32_t>(visibility)),
      .arrayCount    = descriptorCount,
      .bindless      = false,
      .dynamicOffset = mapped.second,
  };
}

rhi::ArgumentLayoutHandle RenderDevice::createArgumentLayoutFromBindings(std::span<const rhi::ArgumentBinding> bindings,
                                                                         const char* debugName)
{
  const rhi::ArgumentLayoutDesc layoutDesc{
      .bindings     = bindings.data(),
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .debugName    = debugName,
  };
  const rhi::ArgumentLayoutHandle layout = m_device.device->createArgumentLayout(layoutDesc);
  m_materials.ownedArgumentLayouts.push_back(layout);
  return layout;
}

rhi::ArgumentLayoutHandle RenderDevice::createArgumentLayout(const ArgumentLayoutDesc& desc)
{
  std::vector<rhi::ArgumentBinding> bindings;
  bindings.reserve(desc.entryCount);
  for(uint32_t i = 0; i < desc.entryCount; ++i)
  {
    bindings.push_back(makeArgumentBinding(desc.entries[i].binding,
                                           desc.entries[i].type,
                                           desc.entries[i].count,
                                           static_cast<rhi::ResourceVisibility>(desc.entries[i].visibility)));
  }
  return createArgumentLayoutFromBindings(bindings, "argument-layout");
}

void RenderDevice::destroyArgumentTable(rhi::ArgumentTableHandle handle)
{
  if(handle.isNull())
  {
    return;
  }
  // Destroy only the ArgumentTable (descriptor set). The layout it was built from is
  // owned separately (ownedArgumentLayouts) and released in destroyArgumentTablesAndLayouts(). For
  // adopted external tables (owned=false) this just unregisters the mirror.
  m_device.device->destroyArgumentTable(handle);
}

rhi::ArgumentTableHandle RenderDevice::createArgumentTable(ArgumentTableDesc desc)
{
  ASSERT(isStableArgumentSlot(desc.slot), "ArgumentTableDesc slot must be one of stable set slots 0..3");
  ASSERT(desc.table.isValid(), "ArgumentTableDesc table must be a valid ArgumentTable handle");
  m_materials.ownedArgumentTables.push_back(desc.table);
  return desc.table;
}

rhi::ArgumentTableHandle RenderDevice::createPersistentArgumentTable(rhi::ArgumentLayoutHandle layout, const char* debugName)
{
  ASSERT(!layout.isNull(), "createPersistentArgumentTable requires a valid argument layout handle");
  return createArgumentTable(ArgumentTableDesc{
      .slot                = ArgumentSlot::shaderSpecific,
      .layout              = layout,
      .table               = m_device.device->createArgumentTable(layout),
      .primaryLogicalIndex = 0,
      .debugName           = debugName,
  });
}

rhi::ArgumentTableHandle RenderDevice::createTemporaryArgumentTable(rhi::ArgumentLayoutHandle layoutHandle,
                                                                    const rhi::ArgumentWrite* writes,
                                                                    uint32_t writeCount,
                                                                    ArgumentSlot slot,
                                                                    const char* debugName)
{
  ASSERT(!layoutHandle.isNull(), "createTemporaryArgumentTable requires a valid argument layout handle");
  (void)slot;
  (void)debugName;

  // Allocate a fresh ArgumentTable from the shared layout and write it now. The handle is
  // recycled (destroyArgumentTable) when this frame index is recorded again.
  const rhi::ArgumentTableHandle table = m_device.device->createArgumentTable(layoutHandle);
  if(writeCount > 0)
  {
    m_device.device->updateArgumentTable(table, writeCount, writes);
  }

  const uint32_t frameIndex = getCurrentFrameIndexHint();
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    m_perFrame.frameUserData[frameIndex].transientArgumentTables.push_back(table);
  }
  return table;
}

void RenderDevice::updateArgumentTable(rhi::ArgumentTableHandle handle,
                                       const rhi::ArgumentWrite* writes,
                                       uint32_t writeCount) const
{
  if(writeCount == 0 || writes == nullptr || handle.isNull())
  {
    return;
  }
  m_device.device->updateArgumentTable(handle, writeCount, writes);
}

void RenderDevice::destroyArgumentTablesAndLayouts()
{
  for(const rhi::ArgumentTableHandle table : m_materials.ownedArgumentTables)
  {
    m_device.device->destroyArgumentTable(table);
  }
  m_materials.ownedArgumentTables.clear();
  for(const rhi::ArgumentLayoutHandle layout : m_materials.ownedArgumentLayouts)
  {
    m_device.device->destroyArgumentLayout(layout);
  }
  m_materials.ownedArgumentLayouts.clear();

  m_materials.materialArgumentTable = rhi::ArgumentTableHandle{};
  m_materials.materialArgumentTables.clear();
  m_materials.materialDescriptorViews.clear();
  m_materials.materialDescriptorValid.clear();
  m_materials.materialArgumentTableGenerations.clear();
  m_materials.materialDescriptorGeneration = 0;
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    frameUserData.sceneArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.cameraArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.drawArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.mdiDrawArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.gbufferMdiDrawArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.depthMdiDrawArgumentTable = rhi::ArgumentTableHandle{};
    frameUserData.csmShadowMdiDrawArgumentTables.fill(rhi::ArgumentTableHandle{});
  }
}

utils::ImageResource RenderDevice::loadAndCreateImage(rhi::CommandBuffer& cmd, const std::string& filename)
{
  int            w = 0, h = 0, comp = 0, req_comp{4};
  const stbi_uc* data = stbi_load(filename.c_str(), &w, &h, &comp, req_comp);
#ifdef __ANDROID__
  std::array<stbi_uc, 16> fallbackPixels{};
  if(data == nullptr)
  {
    const bool useBlue = filename.find("image2") != std::string::npos;
    const std::array<stbi_uc, 4> color = useBlue ? std::array<stbi_uc, 4>{64, 128, 255, 255}
                                                 : std::array<stbi_uc, 4>{255, 128, 64, 255};
    for(size_t i = 0; i < fallbackPixels.size(); i += color.size())
    {
      std::copy(color.begin(), color.end(), fallbackPixels.begin() + static_cast<std::ptrdiff_t>(i));
    }
    data = fallbackPixels.data();
    w    = 2;
    h    = 2;
    comp = req_comp;
  }
#endif
  ASSERT(data != nullptr, "Could not load texture image!");
  const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  const uint32_t mipLevels = 1;

  const VkImageCreateInfo imageInfo = {
      .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType   = VK_IMAGE_TYPE_2D,
      .format      = format,
      .extent      = {uint32_t(w), uint32_t(h), 1},
      .mipLevels   = mipLevels,
      .arrayLayers = 1,
      .samples     = VK_SAMPLE_COUNT_1_BIT,
      .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
  };

  utils::Image nativeImage = createImage(m_device.allocator, imageInfo);
  const rhi::TextureHandle imageHandle =
      m_device.device->registerExternalTexture(reinterpret_cast<uint64_t>(nativeImage.image));
  const rhi::TextureSubresourceRange imageRange{
      .aspect = rhi::TextureAspect::color,
      .baseMipLevel = 0,
      .levelCount = mipLevels,
      .baseArrayLayer = 0,
      .layerCount = 1,
  };
  const rhi::TextureBarrier uploadBeginBarrier{
      .texture = imageHandle,
      .before = rhi::ResourceState::Undefined,
      .after = rhi::ResourceState::TransferDst,
      .range = imageRange,
  };
  cmd.resourceBarrier(&uploadBeginBarrier, 1, nullptr, 0);

  BatchUploadContext upload;
  const std::span<const std::byte> dataSpan(reinterpret_cast<const std::byte*>(data), static_cast<size_t>(w * h * 4));
  upload.init(*m_device.device, static_cast<uint64_t>(dataSpan.size_bytes()));
  const BatchUploadContext::Slice slice = upload.allocate(static_cast<uint64_t>(dataSpan.size_bytes()), 4);
  std::memcpy(slice.cpuPtr, dataSpan.data(), dataSpan.size_bytes());
  upload.recordTextureUpload(
      slice,
      imageHandle,
      rhi::BufferTextureCopyDesc{
          .texture = imageHandle,
          .aspect = rhi::TextureAspect::color,
          .mipLevel = 0,
          .baseArrayLayer = 0,
          .layerCount = 1,
          .width = static_cast<uint32_t>(w),
          .height = static_cast<uint32_t>(h),
          .depth = 1,
      });
  upload.executeUploads(cmd);

  const rhi::TextureBarrier uploadEndBarrier{
      .texture = imageHandle,
      .before = rhi::ResourceState::TransferDst,
      .after = rhi::ResourceState::General,
      .range = imageRange,
  };
  cmd.resourceBarrier(&uploadEndBarrier, 1, nullptr, 0);
  m_device.device->destroyImage(imageHandle);

  rhi::BufferHandle staging = upload.releaseStagingBuffer();
  if(!staging.isNull())
  {
    m_device.rhiStagingBuffers.push_back(staging);
  }

  utils::ImageResource image{};
  image.image = nativeImage.image;
  image.allocation = nativeImage.allocation;
  image.layout = VK_IMAGE_LAYOUT_GENERAL;
  DBG_VK_NAME(image.image);
  image.extent = {uint32_t(w), uint32_t(h)};

  const VkImageViewCreateInfo viewInfo = {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image            = image.image,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = mipLevels, .layerCount = 1},
  };
  VK_CHECK(vkCreateImageView(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), &viewInfo, nullptr, &image.view));
  DBG_VK_NAME(image.view);
#ifdef __ANDROID__
  if(data != fallbackPixels.data())
#endif
  {
    stbi_image_free(const_cast<stbi_uc*>(data));
  }

  return image;
}

void RenderDevice::createPrebuiltComputePipelineVariant()
{
#ifdef USE_SLANG
  VkShaderModule compute = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                     {shader_comp_slang, std::size(shader_comp_slang)});
#else
  VkShaderModule compute = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()),
                                                     {shader_comp_glsl, std::size(shader_comp_glsl)});
#endif
  DBG_VK_NAME(compute);

  const std::array<rhi::PipelinePushConstantRange, 1> computePushConstants{{
      rhi::PipelinePushConstantRange{
          .stages = rhi::ShaderStage::compute,
          .offset = 0,
          .size   = sizeof(shaderio::PushConstantCompute),
      },
  }};
  const rhi::ComputePipelineDesc computeDesc{
      .shaderStage =
          {
              .stage        = rhi::ShaderStage::compute,
              .shaderModule = reinterpret_cast<uint64_t>(compute),
              .entryPoint   = "main",
          },
      .pushConstantRanges  = computePushConstants.data(),
      .pushConstantRangeCount = static_cast<uint32_t>(computePushConstants.size()),
  };
  m_device.prebuiltPipelines.compute = m_device.device->createComputePipeline(computeDesc);

  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle()), compute, nullptr);
}



void RenderDevice::createGPUCullingPipeline()
{
  if(m_device.gpuCullingArgumentTables.empty() || m_device.gpuCullingArgumentTables.front().isNull())
  {
    return;
  }

#ifdef USE_SLANG
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice, {shader_gpu_culling_slang, std::size(shader_gpu_culling_slang)});
  DBG_VK_NAME(shaderModule);
  const rhi::vulkan::ArgumentTableRecord* tableRecord =
      m_device.resourceTable.tryGetArgumentTable(m_device.gpuCullingArgumentTables.front());
  ASSERT(tableRecord != nullptr, "GPU culling pipeline requires an argument layout");
  const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{tableRecord->layout}};

  const rhi::ComputePipelineDesc computeDesc{
      .shaderStage =
          {
              .stage        = rhi::ShaderStage::compute,
              .shaderModule = reinterpret_cast<uint64_t>(shaderModule),
              .entryPoint   = "gpuCullingMain",
      },
      .argumentLayouts     = argumentLayouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
      .specializationVariant = 13,
  };
  m_gpuCullingPipeline = m_device.device->createComputePipeline(computeDesc);

  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif
}

void RenderDevice::createShadowCullingPipeline()
{
  if(m_device.shadowCullingArgumentTables.empty() || m_device.shadowCullingArgumentTables.front().isNull())
  {
    return;
  }

#ifdef USE_SLANG
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice, {shader_shadow_culling_slang, std::size(shader_shadow_culling_slang)});
  DBG_VK_NAME(shaderModule);
  const rhi::vulkan::ArgumentTableRecord* tableRecord =
      m_device.resourceTable.tryGetArgumentTable(m_device.shadowCullingArgumentTables.front());
  ASSERT(tableRecord != nullptr, "Shadow culling pipeline requires an argument layout");
  const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{tableRecord->layout}};
  const std::array<rhi::PipelinePushConstantRange, 1> pushConstants{{
      rhi::PipelinePushConstantRange{.stages = rhi::ShaderStage::compute,
                                     .offset = 0,
                                     .size   = sizeof(shaderio::ShadowCullPushConstants)},
  }};

  const rhi::ComputePipelineDesc computeDesc{
      .shaderStage =
          {
              .stage        = rhi::ShaderStage::compute,
              .shaderModule = reinterpret_cast<uint64_t>(shaderModule),
              .entryPoint   = "shadowCullingMain",
          },
      .argumentLayouts     = argumentLayouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
      .pushConstantRanges  = pushConstants.data(),
      .pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size()),
      .specializationVariant = 14,
  };
  m_shadowCullingPipeline = m_device.device->createComputePipeline(computeDesc);

  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif
}

// Texture-view lifetime now lives on rhi::Device (VulkanDevice). These remain as thin
// forwards so existing RenderDevice-based callers (CSM, swapchain) keep working.
rhi::TextureViewHandle RenderDevice::createTextureView(const rhi::TextureViewCreateDesc& desc)
{
  return m_device.device->createTextureView(desc);
}

rhi::TextureViewHandle RenderDevice::registerExternalTextureView(uint64_t externalView)
{
  return m_device.device->registerExternalTextureView(externalView);
}

void RenderDevice::destroyTextureView(rhi::TextureViewHandle handle)
{
  m_device.device->destroyTextureView(handle);
}

uint64_t RenderDevice::resolveTextureViewBackendHandle(rhi::TextureViewHandle handle) const
{
  return m_device.device->resolveTextureViewBackendHandle(handle);
}

void RenderDevice::destroyPipelines()
{
  std::vector<PipelineHandle> handles;
  m_device.resourceTable.forEachPipeline(
      [&](PipelineHandle handle, const auto&) { handles.push_back(handle); });

  for(const PipelineHandle handle : handles)
  {
    m_device.device->destroyPipeline(handle);
  }

  m_device.prebuiltPipelines.compute             = kNullPipelineHandle;
  m_device.prebuiltPipelines.graphicsTextured    = kNullPipelineHandle;
  m_device.prebuiltPipelines.graphicsNonTextured = kNullPipelineHandle;
  m_lightPipeline = kNullPipelineHandle;
  m_depthPrepassOpaquePipeline = kNullPipelineHandle;
  m_depthPrepassAlphaTestPipeline = kNullPipelineHandle;
  m_depthPrepassOpaqueMDIPipeline = kNullPipelineHandle;
  m_depthPrepassAlphaTestMDIPipeline = kNullPipelineHandle;
  m_gpuCullingPipeline = kNullPipelineHandle;
  m_shadowCullingPipeline = kNullPipelineHandle;
  m_gbufferOpaquePipeline = kNullPipelineHandle;
  m_gbufferAlphaTestPipeline = kNullPipelineHandle;
  m_gbufferOpaqueMDIPipeline = kNullPipelineHandle;
  m_gbufferAlphaTestMDIPipeline = kNullPipelineHandle;
  m_shadowPipeline = kNullPipelineHandle;
  m_forwardPipeline = kNullPipelineHandle;
  m_forwardMDIPipeline = kNullPipelineHandle;
  m_gpuDrivenLightHdrPipeline = kNullPipelineHandle;
  m_gpuDrivenSkyboxPipeline = kNullPipelineHandle;
  m_bloomPrefilterPipeline = kNullPipelineHandle;
  m_bloomDownsamplePipeline = kNullPipelineHandle;
  m_finalColorPipeline = kNullPipelineHandle;
  m_velocityPipeline = kNullPipelineHandle;
  m_taaResolvePipeline = kNullPipelineHandle;
  m_debugPipeline = kNullPipelineHandle;
  m_gpuCullingDebugPipeline = kNullPipelineHandle;
}

const RenderDevice::MaterialResources::TextureHotData* RenderDevice::tryGetTextureHot(TextureHandle handle) const
{
  const MaterialResources::TextureRecord* textureRecord = m_materials.texturePool.tryGet(handle);
  return textureRecord != nullptr ? &textureRecord->hot : nullptr;
}

const RenderDevice::MaterialResources::MaterialRecord* RenderDevice::tryGetMaterial(MaterialHandle handle) const
{
  return m_materials.materialPool.tryGet(handle);
}

const RenderDevice::MaterialResources::TextureColdData* RenderDevice::tryGetTextureCold(TextureHandle handle) const
{
  const MaterialResources::TextureRecord* textureRecord = m_materials.texturePool.tryGet(handle);
  return textureRecord != nullptr ? &textureRecord->cold : nullptr;
}

void RenderDevice::waitForIdle()
{
  flushPendingUploadCommands(true);
  m_device.device->waitIdle();
}

uintptr_t RenderDevice::getBackendDeviceToken() const
{
  if(m_device.device == nullptr)
  {
    return 0;
  }
  return static_cast<uintptr_t>(m_device.device->getBackendDeviceHandle());
}

void RenderDevice::executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn)
{
  // Upload cmd pool + fence lifecycle sunk into VulkanDevice (UPL-02).
  m_device.device->executeImmediateUpload(std::move(uploadFn));
}

void RenderDevice::flushPendingUploadCommands(bool waitForCompletion)
{
  if(m_device.device == nullptr)
  {
    return;
  }
  // Step 1: backend fence/cmd retirement — VulkanDevice polls or blocks on upload fences
  // and recycles their cmd buffers (UPL-02/03 lifecycle preserved).
  m_device.device->flushUploadRetirements(waitForCompletion);

  // Step 2: render-layer staging buffer retirement.
  // rhiStagingBuffers (rhi::BufferHandle retirement queue) stays here — VulkanDevice
  // cannot hold render-layer handle vectors (D-05 UPL-03 fence-gated free).
  freeRhiStagingBuffers(m_device.device.get(), m_device.rhiStagingBuffers);
  m_meshPool.freeStagingBuffers();
}

GltfUploadResult RenderDevice::uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd)
{
  GltfUploadResult result;
  initializeGltfUploadResult(model, result);

  std::vector<uint32_t> textureIndices(model.images.size());
  std::vector<uint32_t> materialIndices(model.materials.size());
  std::vector<uint32_t> meshIndices(model.meshes.size());
  std::iota(textureIndices.begin(), textureIndices.end(), 0u);
  std::iota(materialIndices.begin(), materialIndices.end(), 0u);
  std::iota(meshIndices.begin(), meshIndices.end(), 0u);

  uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, result, cmd);
  return result;
}

SceneUploadResult RenderDevice::commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan, rhi::CommandBuffer& cmd)
{
  const SceneUploadPlanValidationResult validation =
      SceneUploadPlanner::validate(makeSceneAssetView(asset), plan);
  ASSERT(validation.valid, "SceneUploadPlan validation failed before GPU commit");

  SceneUploadResult result{};
  result.meshes.resize(asset.meshes.size(), kNullMeshHandle);
  result.materials.resize(asset.materials.size(), kNullMaterialHandle);
  result.textures.resize(asset.textures.size(), kNullTextureHandle);

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const VkPhysicalDevice physicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle());

  VkDeviceSize estimatedTextureUploadBytes = 0;
  VkDeviceSize estimatedVertexUploadBytes = 0;
  VkDeviceSize estimatedIndexUploadBytes = 0;
  for(const TextureUploadPlan& texturePlan : plan.textures)
  {
    estimatedTextureUploadBytes += texturePlan.payloadByteSize;
  }
  for(const MeshUploadPlan& meshPlan : plan.meshes)
  {
    estimatedVertexUploadBytes += meshPlan.vertexByteSize;
    estimatedIndexUploadBytes += meshPlan.indexByteSize;
  }

  struct PendingTextureUploadState
  {
    rhi::TextureHandle texture{};
    uint32_t           mipLevels{1};
  };

  BatchUploadContext textureBatchUpload;
  textureBatchUpload.init(*m_device.device, static_cast<uint64_t>(estimatedTextureUploadBytes));
  std::vector<PendingTextureUploadState> textureUploadStates;
  BatchUploadContext meshBatchUpload;
  meshBatchUpload.init(*m_device.device, static_cast<uint64_t>(estimatedVertexUploadBytes + estimatedIndexUploadBytes));
  m_meshPool.reserve(estimatedVertexUploadBytes, estimatedIndexUploadBytes, cmd);

  for(const TextureUploadPlan& texturePlan : plan.textures)
  {
    if(texturePlan.textureIndex >= asset.textures.size() || !result.textures[texturePlan.textureIndex].isNull())
    {
      continue;
    }

    const SceneTexture& texture = asset.textures[texturePlan.textureIndex];
    if(texture.payloadSize == 0 || texture.payloadOffset + texture.payloadSize > asset.texturePayload.size())
    {
      continue;
    }

    const uint8_t* payloadData = asset.texturePayload.data() + texture.payloadOffset;
    const size_t payloadSize = static_cast<size_t>(texture.payloadSize);
    VkFormat format = toNativeFormat(texturePlan.format);
    uint32_t width = texturePlan.width;
    uint32_t height = texturePlan.height;
    uint32_t mipLevels = std::max(texturePlan.mipLevels, 1u);
    Ktx2Loader::Ktx2Texture ktxTexture;
    const bool isKtxPayload = texturePlan.payloadKind == TexturePayloadKind::Ktx2Container;
    if(isKtxPayload)
    {
      Ktx2Loader loader;
      if(!loader.loadFromMemory(payloadData, payloadSize, ktxTexture))
      {
        LOGW("Skipping SceneAsset KTX2 texture %u: %s", texturePlan.textureIndex, loader.getLastError().c_str());
        continue;
      }
      if(!supportsSampledImageFormat(physicalDevice, toNativeFormat(ktxTexture.format)))
      {
        LOGW("Skipping unsupported SceneAsset KTX2 texture format %s for texture %u",
             string_VkFormat(toNativeFormat(ktxTexture.format)),
             texturePlan.textureIndex);
        continue;
      }
      format = toNativeFormat(ktxTexture.format);
      width = ktxTexture.width;
      height = ktxTexture.height;
      mipLevels = std::max(ktxTexture.mipLevels, 1u);
    }

    const VkImageCreateInfo imageInfo = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = format,
        .extent      = {width, height, 1},
        .mipLevels   = mipLevels,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    utils::Image image = createImage(m_device.allocator, imageInfo);
    const rhi::TextureHandle imageHandle =
        m_device.device->registerExternalTexture(reinterpret_cast<uint64_t>(image.image));
    const rhi::TextureSubresourceRange imageRange{
        .aspect = rhi::TextureAspect::color,
        .baseMipLevel = 0,
        .levelCount = mipLevels,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const rhi::TextureBarrier uploadBeginBarrier{
        .texture = imageHandle,
        .before = rhi::ResourceState::Undefined,
        .after = rhi::ResourceState::TransferDst,
        .range = imageRange,
    };
    cmd.resourceBarrier(&uploadBeginBarrier, 1, nullptr, 0);
    textureUploadStates.push_back(PendingTextureUploadState{.texture = imageHandle, .mipLevels = mipLevels});

    if(isKtxPayload)
    {
      const BatchUploadContext::Slice slice = textureBatchUpload.allocate(ktxTexture.data.size(), 16);
      std::memcpy(slice.cpuPtr, ktxTexture.data.data(), ktxTexture.data.size());

      for(uint32_t mip = 0; mip < mipLevels; ++mip)
      {
        const rhi::BufferTextureCopyDesc region{
            .bufferOffset = ktxTexture.mipOffsets[mip],
            .texture = imageHandle,
            .aspect = rhi::TextureAspect::color,
            .mipLevel = mip,
            .baseArrayLayer = 0,
            .layerCount = 1,
            .width = std::max(width >> mip, 1u),
            .height = std::max(height >> mip, 1u),
            .depth = 1,
        };
        textureBatchUpload.recordTextureUpload(slice, imageHandle, region);
      }
    }
    else
    {
      const BatchUploadContext::Slice slice = textureBatchUpload.allocate(payloadSize, 4);
      std::memcpy(slice.cpuPtr, payloadData, payloadSize);
      textureBatchUpload.recordTextureUpload(
          slice,
          imageHandle,
          rhi::BufferTextureCopyDesc{
              .texture = imageHandle,
              .aspect = rhi::TextureAspect::color,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
              .width = width,
              .height = height,
              .depth = 1,
          });
    }

    const VkImageViewCreateInfo viewInfo = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = mipLevels, .layerCount = 1},
    };

    VkImageView imageView = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageView));

    utils::ImageResource imageResource{};
    imageResource.image = image.image;
    imageResource.allocation = image.allocation;
    imageResource.view = imageView;
    imageResource.layout = VK_IMAGE_LAYOUT_GENERAL;
    imageResource.extent = {width, height};

    result.textures[texturePlan.textureIndex] = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot = {
            .runtimeKind = MaterialResources::TextureRuntimeKind::materialSampled,
            .sampledImageView = imageView,
            .sampledImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .sampledViewHandle = registerExternalTextureView(reinterpret_cast<uint64_t>(imageView)),
        },
        .cold = {
            .ownedImage = imageResource,
            .sourceExtent = {width, height},
            .mipLevels = mipLevels,
        },
    });
  }

  for(const MaterialCreatePlan& materialPlan : plan.materials)
  {
    if(materialPlan.materialIndex >= asset.materials.size() || !result.materials[materialPlan.materialIndex].isNull())
    {
      continue;
    }

    MaterialResources::MaterialRecord record;
    if(materialPlan.baseColorTexture >= 0 && static_cast<size_t>(materialPlan.baseColorTexture) < result.textures.size())
    {
      record.baseColorTexture = result.textures[materialPlan.baseColorTexture];
      record.sampledTexture = record.baseColorTexture;
    }
    if(materialPlan.metallicRoughnessTexture >= 0 && static_cast<size_t>(materialPlan.metallicRoughnessTexture) < result.textures.size())
    {
      record.metallicRoughnessTexture = result.textures[materialPlan.metallicRoughnessTexture];
    }
    if(materialPlan.normalTexture >= 0 && static_cast<size_t>(materialPlan.normalTexture) < result.textures.size())
    {
      record.normalTexture = result.textures[materialPlan.normalTexture];
    }
    if(materialPlan.occlusionTexture >= 0 && static_cast<size_t>(materialPlan.occlusionTexture) < result.textures.size())
    {
      record.occlusionTexture = result.textures[materialPlan.occlusionTexture];
    }
    if(materialPlan.emissiveTexture >= 0 && static_cast<size_t>(materialPlan.emissiveTexture) < result.textures.size())
    {
      record.emissiveTexture = result.textures[materialPlan.emissiveTexture];
    }

    record.baseColorFactor = materialPlan.baseColorFactor;
    record.metallicFactor = materialPlan.metallicFactor;
    record.roughnessFactor = materialPlan.roughnessFactor;
    record.normalScale = materialPlan.normalScale;
    record.occlusionStrength = materialPlan.occlusionStrength;
    record.emissiveFactor = materialPlan.emissiveFactor;
    record.alphaMode = materialPlan.alphaMode;
    record.alphaCutoff = materialPlan.alphaCutoff;
    record.descriptorIndex = static_cast<rhi::ResourceIndex>(materialPlan.materialIndex);
    record.debugName = asset.materials[materialPlan.materialIndex].name.empty()
                           ? "scene-material"
                           : asset.materials[materialPlan.materialIndex].name.c_str();

    result.materials[materialPlan.materialIndex] = m_materials.materialPool.emplace(std::move(record));
  }

  for(const MeshUploadPlan& meshPlan : plan.meshes)
  {
    if(meshPlan.meshIndex >= asset.meshes.size() || !result.meshes[meshPlan.meshIndex].isNull())
    {
      continue;
    }

    const SceneMesh& mesh = asset.meshes[meshPlan.meshIndex];
    if(meshPlan.vertexSrcOffset + meshPlan.vertexByteSize > asset.vertexPayload.size()
       || meshPlan.indexSrcOffset + meshPlan.indexByteSize > asset.indexPayload.size())
    {
      continue;
    }

    const uint8_t* vertexBegin = asset.vertexPayload.data() + meshPlan.vertexSrcOffset;
    const uint32_t* indexBegin = reinterpret_cast<const uint32_t*>(asset.indexPayload.data() + meshPlan.indexSrcOffset);
    SceneMeshData sceneMeshData{
        .interleavedVertexData = std::span<const uint8_t>(vertexBegin, static_cast<size_t>(meshPlan.vertexByteSize)),
        .vertexCount = meshPlan.vertexCount,
        .indices = std::span<const uint32_t>(indexBegin, meshPlan.indexCount),
        .transform = mesh.exportTransform,
        .materialIndex = static_cast<int32_t>(mesh.materialIndex),
    };

    MeshHandle meshHandle = m_meshPool.uploadMesh(sceneMeshData, cmd, &meshBatchUpload);
    if(meshHandle.isNull())
    {
      continue;
    }

    result.meshes[meshPlan.meshIndex] = meshHandle;

    const SceneMaterial* material = mesh.materialIndex < asset.materials.size() ? &asset.materials[mesh.materialIndex] : nullptr;
    if(material != nullptr)
    {
      m_meshPool.setMeshAlphaMode(meshHandle, material->alphaMode, material->alphaCutoff);
      m_meshPool.setMeshMaterialData(meshHandle,
                                     material->baseColorFactor,
                                     material->baseColorTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + material->baseColorTexture) : -1,
                                     material->normalTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + material->normalTexture) : -1,
                                     material->metallicRoughnessTexture >= 0
                                         ? static_cast<int32_t>(getGltfTextureBaseIndex() + material->metallicRoughnessTexture)
                                         : -1,
                                     material->occlusionTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + material->occlusionTexture) : -1,
                                     material->emissiveTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + material->emissiveTexture) : -1,
                                     material->metallicFactor,
                                     material->roughnessFactor,
                                     material->normalScale,
                                     material->occlusionStrength,
                                     glm::vec4(material->emissiveFactor, 0.0f),
                                     material->materialWorkflow);

      if(material->alphaMode == shaderio::LAlphaOpaque)
      {
        result.opaqueMeshIndices.push_back(meshPlan.meshIndex);
        result.shadowCasterIndices.push_back(meshPlan.meshIndex);
      }
      else if(material->alphaMode == shaderio::LAlphaMask)
      {
        result.alphaTestMeshIndices.push_back(meshPlan.meshIndex);
        result.shadowCasterIndices.push_back(meshPlan.meshIndex);
      }
      else
      {
        result.transparentMeshIndices.push_back(meshPlan.meshIndex);
      }
    }
    else
    {
      result.opaqueMeshIndices.push_back(meshPlan.meshIndex);
      result.shadowCasterIndices.push_back(meshPlan.meshIndex);
    }
  }

  result.instanceToDrawRecord.assign(plan.instances.instances.size(), UINT32_MAX);
  result.drawCommandToDrawRecord.assign(plan.drawCommands.size(), UINT32_MAX);
  result.drawRecords.reserve(plan.drawCommands.empty() ? plan.instances.instances.size() : plan.drawCommands.size());

  auto appendDrawRecord = [&](uint32_t instanceIndex,
                              uint32_t meshIndex,
                              uint32_t materialIndex,
                              const glm::mat4& worldTransform,
                              SceneDrawBucket bucket,
                              const glm::vec4& boundsSphere) -> uint32_t {
    if(meshIndex >= result.meshes.size())
    {
      return UINT32_MAX;
    }

    const MeshHandle meshHandle = result.meshes[meshIndex];
    if(meshHandle.isNull())
    {
      return UINT32_MAX;
    }

    MaterialHandle materialHandle = kNullMaterialHandle;
    if(materialIndex < result.materials.size())
    {
      materialHandle = result.materials[materialIndex];
    }

    const uint32_t alphaMode = bucket == SceneDrawBucket::Transparent
                                   ? shaderio::LAlphaBlend
                                   : (bucket == SceneDrawBucket::AlphaMask ? shaderio::LAlphaMask : shaderio::LAlphaOpaque);
    float alphaCutoff = 0.5f;
    if(materialIndex < asset.materials.size())
    {
      alphaCutoff = asset.materials[materialIndex].alphaCutoff;
    }

    const uint32_t drawRecordIndex = static_cast<uint32_t>(result.drawRecords.size());
    result.drawRecords.push_back(SceneUploadResult::SceneDrawRecord{
      .instanceIndex = instanceIndex,
      .meshIndex = meshIndex,
      .materialIndex = materialIndex,
      .meshHandle = meshHandle,
      .materialHandle = materialHandle,
      .worldTransform = worldTransform,
      .boundsSphere = boundsSphere,
      .alphaMode = alphaMode,
      .alphaCutoff = alphaCutoff,
    });
    result.drawMeshHandles.push_back(meshHandle);
    if(bucket == SceneDrawBucket::Transparent)
    {
      result.transparentDrawIndices.push_back(drawRecordIndex);
      result.transparentDrawDistances.push_back(0.0f);
    }
    else if(bucket == SceneDrawBucket::AlphaMask)
    {
      result.shadowCasterDrawIndices.push_back(drawRecordIndex);
      result.alphaTestDrawIndices.push_back(drawRecordIndex);
    }
    else
    {
      result.shadowCasterDrawIndices.push_back(drawRecordIndex);
      result.opaqueDrawIndices.push_back(drawRecordIndex);
    }
    return drawRecordIndex;
  };

  if(!plan.drawCommands.empty())
  {
    for(uint32_t commandIndex = 0; commandIndex < static_cast<uint32_t>(plan.drawCommands.size()); ++commandIndex)
    {
      const DrawCommandBuildPlan& command = plan.drawCommands[commandIndex];
      if(command.instanceIndex >= plan.instances.instances.size())
      {
        continue;
      }

      const SceneDrawInstance& instance = plan.instances.instances[command.instanceIndex];
      glm::vec4 boundsSphere(0.0f);
      for(const InstanceCullRecord& cullRecord : plan.cullRecords)
      {
        if(cullRecord.instanceIndex == command.instanceIndex)
        {
          boundsSphere = cullRecord.boundingSphere;
          break;
        }
      }

      const uint32_t drawRecordIndex = appendDrawRecord(command.instanceIndex,
                                                        command.meshIndex,
                                                        command.materialIndex,
                                                        instance.worldTransform,
                                                        command.bucket,
                                                        boundsSphere);
      if(drawRecordIndex == UINT32_MAX)
      {
        continue;
      }
      result.drawCommandToDrawRecord[commandIndex] = drawRecordIndex;
      if(command.instanceIndex < result.instanceToDrawRecord.size()
         && result.instanceToDrawRecord[command.instanceIndex] == UINT32_MAX)
      {
        result.instanceToDrawRecord[command.instanceIndex] = drawRecordIndex;
      }
    }
  }
  else
  {
    for(uint32_t instanceIndex = 0; instanceIndex < static_cast<uint32_t>(plan.instances.instances.size()); ++instanceIndex)
    {
      const SceneDrawInstance& instance = plan.instances.instances[instanceIndex];
      SceneDrawBucket bucket = SceneDrawBucket::Opaque;
      if(instance.materialIndex < asset.materials.size())
      {
        const int32_t alphaMode = asset.materials[instance.materialIndex].alphaMode;
        bucket = alphaMode == shaderio::LAlphaBlend
                     ? SceneDrawBucket::Transparent
                     : (alphaMode == shaderio::LAlphaMask ? SceneDrawBucket::AlphaMask : SceneDrawBucket::Opaque);
      }

      glm::vec4 boundsSphere(0.0f);
      for(const InstanceCullRecord& cullRecord : plan.cullRecords)
      {
        if(cullRecord.instanceIndex == instanceIndex)
        {
          boundsSphere = cullRecord.boundingSphere;
          break;
        }
      }

      const uint32_t drawRecordIndex = appendDrawRecord(instanceIndex,
                                                        instance.meshIndex,
                                                        instance.materialIndex,
                                                        instance.worldTransform,
                                                        bucket,
                                                        boundsSphere);
      if(drawRecordIndex != UINT32_MAX)
      {
        result.instanceToDrawRecord[instanceIndex] = drawRecordIndex;
      }
    }
  }

  textureBatchUpload.executeUploads(cmd);
  meshBatchUpload.executeUploads(cmd);

  for(const PendingTextureUploadState& state : textureUploadStates)
  {
    const rhi::TextureBarrier uploadEndBarrier{
        .texture = state.texture,
        .before = rhi::ResourceState::TransferDst,
        .after = rhi::ResourceState::General,
        .range =
            {
                .aspect = rhi::TextureAspect::color,
                .baseMipLevel = 0,
                .levelCount = state.mipLevels,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    cmd.resourceBarrier(&uploadEndBarrier, 1, nullptr, 0);
    m_device.device->destroyImage(state.texture);
  }

  rhi::BufferHandle textureStagingBuffer = textureBatchUpload.releaseStagingBuffer();
  if(!textureStagingBuffer.isNull())
  {
    m_device.rhiStagingBuffers.push_back(textureStagingBuffer);
  }
  rhi::BufferHandle meshStagingBuffer = meshBatchUpload.releaseStagingBuffer();
  if(!meshStagingBuffer.isNull())
  {
    m_meshPool.deferStagingBuffer(meshStagingBuffer);
  }

  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();
  for(uint32_t textureIndex = 0; textureIndex < result.textures.size(); ++textureIndex)
  {
    if(!result.textures[textureIndex].isNull())
    {
      updateBindlessTexture(gltfTextureBaseIndex + textureIndex, result.textures[textureIndex]);
    }
  }

  result.transparentDistances.resize(result.transparentMeshIndices.size(), 0.0f);
  result.transparentSortDirty = true;
  rebuildShadowPackedBuffers(asset, result, cmd);
  return result;
}

void RenderDevice::initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const
{
  outResult = {};
  outResult.meshes.resize(model.meshes.size(), kNullMeshHandle);
  outResult.materials.resize(model.materials.size(), kNullMaterialHandle);
  outResult.textures.resize(model.images.size(), kNullTextureHandle);
}

void RenderDevice::uploadGltfModelBatch(const GltfModel&          model,
                                    std::span<const uint32_t> textureIndices,
                                    std::span<const uint32_t> materialIndices,
                                    std::span<const uint32_t> meshIndices,
                                    GltfUploadResult&         ioResult,
                                    rhi::CommandBuffer&       cmd)
{
  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const VkPhysicalDevice physicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getBackendPhysicalDeviceHandle());

  if(ioResult.meshes.size() != model.meshes.size()
     || ioResult.materials.size() != model.materials.size()
     || ioResult.textures.size() != model.images.size())
  {
    initializeGltfUploadResult(model, ioResult);
  }

  BatchUploadContext textureBatchUpload;
  const VkDeviceSize estimatedTextureUploadBytes = computeSelectedTextureUploadSize(model, textureIndices);
  const VkDeviceSize estimatedMeshUploadBytes = computeSelectedMeshUploadSize(model, meshIndices);
  const VkDeviceSize estimatedBatchUploadBytes = estimatedTextureUploadBytes + estimatedMeshUploadBytes;
  const TextureUploadDiagnostics textureDiagnostics = gatherTextureUploadDiagnostics(model, textureIndices);
  LOGI("glTF upload batch: textures=%zu materials=%zu meshes=%zu textureBytes=%llu meshBytes=%llu totalBytes=%llu mipGenTextures=%u maxTexture=%ux%u",
       textureIndices.size(),
       materialIndices.size(),
       meshIndices.size(),
       static_cast<unsigned long long>(estimatedTextureUploadBytes),
       static_cast<unsigned long long>(estimatedMeshUploadBytes),
       static_cast<unsigned long long>(estimatedBatchUploadBytes),
       textureDiagnostics.mipGenerationCount,
       textureDiagnostics.maxWidth,
       textureDiagnostics.maxHeight);
  Ktx2Loader batchKtx2Loader;
  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex >= model.images.size())
    {
      continue;
    }

    const GltfImageData& imageData = model.images[textureIndex];
    Ktx2Loader::Ktx2Texture ktxTexture;
    const bool hasKtx2Sidecar = tryLoadKtx2Texture(model, imageData, batchKtx2Loader, ktxTexture);
    const uint32_t width = hasKtx2Sidecar ? ktxTexture.width : static_cast<uint32_t>(imageData.width);
    const uint32_t height = hasKtx2Sidecar ? ktxTexture.height : static_cast<uint32_t>(imageData.height);
    const uint32_t mipLevels = hasKtx2Sidecar ? std::max(ktxTexture.mipLevels, 1u)
                                              : MipmapGenerator::calculateMipLevelCount(width, height);
    LOGI("  glTF texture[%u]: uri=%s size=%ux%u pixels=%zu ktx2=%d mipLevels=%u",
         textureIndex,
         imageData.uri.empty() ? "<embedded>" : imageData.uri.c_str(),
         width,
         height,
         imageData.pixels.size(),
         hasKtx2Sidecar ? 1 : 0,
         mipLevels);
  }
  struct PendingTextureUploadState
  {
    rhi::TextureHandle texture{};
    uint32_t           mipLevels{1};
  };

  textureBatchUpload.init(*m_device.device, static_cast<uint64_t>(estimatedTextureUploadBytes));
  std::vector<PendingTextureUploadState> textureUploadStates;
  BatchUploadContext meshBatchUpload;
  meshBatchUpload.init(*m_device.device, static_cast<uint64_t>(estimatedMeshUploadBytes));

  VkDeviceSize selectedVertexBytes = 0;
  VkDeviceSize selectedIndexBytes = 0;
  for(const uint32_t meshIndex : meshIndices)
  {
    if(meshIndex >= model.meshes.size() || !ioResult.meshes[meshIndex].isNull())
    {
      continue;
    }

    const GltfMeshData& meshData = model.meshes[meshIndex];
    selectedVertexBytes += static_cast<VkDeviceSize>(meshData.positions.size() / 3u) * 48ull;
    selectedIndexBytes += static_cast<VkDeviceSize>(meshData.indices.size()) * sizeof(uint32_t);
  }
  m_meshPool.reserve(selectedVertexBytes, selectedIndexBytes, cmd);

  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex >= model.images.size() || !ioResult.textures[textureIndex].isNull())
    {
      continue;
    }

    const GltfImageData& imageData = model.images[textureIndex];
    if(!isUploadableGltfImage(imageData))
    {
      continue;
    }

    Ktx2Loader              ktx2Loader;
    Ktx2Loader::Ktx2Texture ktxTexture;
    std::filesystem::path   ktx2Path;
    const bool loadedKtx2Sidecar = tryLoadKtx2Texture(model, imageData, ktx2Loader, ktxTexture, &ktx2Path);
    const bool hasSupportedKtx2Sidecar =
        loadedKtx2Sidecar && supportsSampledImageFormat(physicalDevice, toNativeFormat(ktxTexture.format));
    if(loadedKtx2Sidecar && !hasSupportedKtx2Sidecar)
    {
      LOGW("Skipping unsupported KTX2 format %s for %s, falling back to source image upload",
           string_VkFormat(toNativeFormat(ktxTexture.format)),
           ktx2Path.string().c_str());
    }
    const GltfImageData* rawImageData = hasSupportedKtx2Sidecar ? nullptr : findRawImageFallback(model, imageData);
    if(!hasSupportedKtx2Sidecar && rawImageData == nullptr)
    {
      LOGW("Skipping glTF KTX2 texture without a raw image fallback: uri=%s path=%s reason=%s",
           imageData.uri.empty() ? "<embedded>" : imageData.uri.c_str(),
           ktx2Path.empty() ? "<none>" : ktx2Path.string().c_str(),
           ktx2Loader.getLastError().empty() ? "unsupported or missing KTX2" : ktx2Loader.getLastError().c_str());
      continue;
    }

    const VkFormat format = hasSupportedKtx2Sidecar ? toNativeFormat(ktxTexture.format) : VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t width = hasSupportedKtx2Sidecar ? ktxTexture.width : static_cast<uint32_t>(rawImageData->width);
    const uint32_t height = hasSupportedKtx2Sidecar ? ktxTexture.height : static_cast<uint32_t>(rawImageData->height);
    const uint32_t requestedMipLevels =
        hasSupportedKtx2Sidecar ? std::max(ktxTexture.mipLevels, 1u) : MipmapGenerator::calculateMipLevelCount(width, height);
    const uint32_t mipLevels = hasSupportedKtx2Sidecar ? requestedMipLevels : 1u;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(!hasSupportedKtx2Sidecar && mipLevels > 1)
    {
      usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    const VkImageCreateInfo imageInfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = {width, height, 1},
        .mipLevels     = mipLevels,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .usage         = usage,
    };

    utils::Image image = createImage(m_device.allocator, imageInfo);
    const rhi::TextureHandle imageHandle =
        m_device.device->registerExternalTexture(reinterpret_cast<uint64_t>(image.image));
    const rhi::TextureSubresourceRange imageRange{
        .aspect = rhi::TextureAspect::color,
        .baseMipLevel = 0,
        .levelCount = mipLevels,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const rhi::TextureBarrier uploadBeginBarrier{
        .texture = imageHandle,
        .before = rhi::ResourceState::Undefined,
        .after = rhi::ResourceState::TransferDst,
        .range = imageRange,
    };
    cmd.resourceBarrier(&uploadBeginBarrier, 1, nullptr, 0);
    textureUploadStates.push_back(PendingTextureUploadState{.texture = imageHandle, .mipLevels = mipLevels});

    if(hasSupportedKtx2Sidecar)
    {
      const BatchUploadContext::Slice slice = textureBatchUpload.allocate(ktxTexture.data.size(), 16);
      std::memcpy(slice.cpuPtr, ktxTexture.data.data(), ktxTexture.data.size());

      for(uint32_t mip = 0; mip < mipLevels; ++mip)
      {
        const rhi::BufferTextureCopyDesc region{
            .bufferOffset = ktxTexture.mipOffsets[mip],
            .texture = imageHandle,
            .aspect = rhi::TextureAspect::color,
            .mipLevel = mip,
            .baseArrayLayer = 0,
            .layerCount = 1,
            .width = std::max(width >> mip, 1u),
            .height = std::max(height >> mip, 1u),
            .depth = 1,
        };
        textureBatchUpload.recordTextureUpload(slice, imageHandle, region);
      }
    }
    else
    {
      const BatchUploadContext::Slice slice = textureBatchUpload.allocate(rawImageData->pixels.size(), 4);
      std::memcpy(slice.cpuPtr, rawImageData->pixels.data(), rawImageData->pixels.size());
      textureBatchUpload.recordTextureUpload(
          slice,
          imageHandle,
          rhi::BufferTextureCopyDesc{
              .texture = imageHandle,
              .aspect = rhi::TextureAspect::color,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
              .width = width,
              .height = height,
              .depth = 1,
          });

      // Temporary validation path: keep raw glTF texture uploads at base mip only
      // so we can isolate device-loss issues from the runtime mip-generation chain.
      if(requestedMipLevels > 1)
      {
        LOGI("Skipping runtime mip generation for raw glTF texture: uri=%s size=%ux%u requestedMipLevels=%u",
             rawImageData->uri.empty() ? "<embedded>" : rawImageData->uri.c_str(),
             width,
             height,
             requestedMipLevels);
      }
    }

    const VkImageViewCreateInfo viewInfo = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = mipLevels, .layerCount = 1},
    };

    VkImageView imageView = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageView));

    utils::ImageResource imageResource{};
    imageResource.image      = image.image;
    imageResource.allocation = image.allocation;
    imageResource.view       = imageView;
    imageResource.layout     = VK_IMAGE_LAYOUT_GENERAL;
    imageResource.extent     = {width, height};

    ioResult.textures[textureIndex] = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = imageView,
                .sampledImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .sampledViewHandle  = registerExternalTextureView(reinterpret_cast<uint64_t>(imageView)),
            },
        .cold =
            {
                .ownedImage   = imageResource,
                .sourceExtent = {width, height},
                .mipLevels    = mipLevels,
            },
    });
  }

  for(const uint32_t materialIndex : materialIndices)
  {
    if(materialIndex >= model.materials.size() || !ioResult.materials[materialIndex].isNull())
    {
      continue;
    }

    const auto& matData = model.materials[materialIndex];
    MaterialResources::MaterialRecord record;

    if(matData.baseColorTexture >= 0 && static_cast<size_t>(matData.baseColorTexture) < ioResult.textures.size())
    {
      record.baseColorTexture = ioResult.textures[matData.baseColorTexture];
      record.sampledTexture   = record.baseColorTexture;
    }
    if(matData.metallicRoughnessTexture >= 0 && static_cast<size_t>(matData.metallicRoughnessTexture) < ioResult.textures.size())
    {
      record.metallicRoughnessTexture = ioResult.textures[matData.metallicRoughnessTexture];
    }
    if(matData.normalTexture >= 0 && static_cast<size_t>(matData.normalTexture) < ioResult.textures.size())
    {
      record.normalTexture = ioResult.textures[matData.normalTexture];
    }
    if(matData.occlusionTexture >= 0 && static_cast<size_t>(matData.occlusionTexture) < ioResult.textures.size())
    {
      record.occlusionTexture = ioResult.textures[matData.occlusionTexture];
    }
    if(matData.emissiveTexture >= 0 && static_cast<size_t>(matData.emissiveTexture) < ioResult.textures.size())
    {
      record.emissiveTexture = ioResult.textures[matData.emissiveTexture];
    }

    record.baseColorFactor    = matData.baseColorFactor;
    record.metallicFactor     = matData.metallicFactor;
    record.roughnessFactor    = matData.roughnessFactor;
    record.normalScale        = matData.normalScale;
    record.occlusionStrength  = matData.occlusionStrength;
    record.emissiveFactor     = matData.emissiveFactor;
    record.alphaMode          = matData.alphaMode;
    record.alphaCutoff        = matData.alphaCutoff;
    record.descriptorIndex    = static_cast<rhi::ResourceIndex>(materialIndex);
    record.debugName          = matData.name.empty() ? "gltf-material" : matData.name.c_str();

    ioResult.materials[materialIndex] = m_materials.materialPool.emplace(std::move(record));
  }

  for(const uint32_t meshIndex : meshIndices)
  {
    if(meshIndex >= model.meshes.size() || !ioResult.meshes[meshIndex].isNull())
    {
      continue;
    }

    const GltfMeshData& meshData = model.meshes[meshIndex];
    MeshHandle          meshHandle = m_meshPool.uploadMesh(meshData, cmd, &meshBatchUpload);
    if(meshHandle.isNull())
    {
      continue;
    }

    ioResult.meshes[meshIndex] = meshHandle;

    if(meshData.materialIndex >= 0 && meshData.materialIndex < static_cast<int>(model.materials.size()))
    {
      const auto& matData = model.materials[meshData.materialIndex];
      m_meshPool.setMeshAlphaMode(meshHandle, matData.alphaMode, matData.alphaCutoff);
      m_meshPool.setMeshMaterialData(meshHandle,
                                     matData.baseColorFactor,
                                     matData.baseColorTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + matData.baseColorTexture) : -1,
                                     matData.normalTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + matData.normalTexture) : -1,
                                     matData.metallicRoughnessTexture >= 0
                                         ? static_cast<int32_t>(getGltfTextureBaseIndex() + matData.metallicRoughnessTexture)
                                         : -1,
                                     matData.occlusionTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + matData.occlusionTexture) : -1,
                                     matData.emissiveTexture >= 0 ? static_cast<int32_t>(getGltfTextureBaseIndex() + matData.emissiveTexture) : -1,
                                     matData.metallicFactor,
                                     matData.roughnessFactor,
                                     matData.normalScale,
                                     matData.occlusionStrength,
                                     glm::vec4(matData.emissiveFactor, 0.0f),
                                     matData.materialWorkflow);

      if(matData.alphaMode == shaderio::LAlphaOpaque)
      {
        ioResult.opaqueMeshIndices.push_back(meshIndex);
        ioResult.shadowCasterIndices.push_back(meshIndex);
      }
      else if(matData.alphaMode == shaderio::LAlphaMask)
      {
        ioResult.alphaTestMeshIndices.push_back(meshIndex);
        ioResult.shadowCasterIndices.push_back(meshIndex);
      }
      else
      {
        ioResult.transparentMeshIndices.push_back(meshIndex);
      }
    }
    else
    {
      ioResult.opaqueMeshIndices.push_back(meshIndex);
      ioResult.shadowCasterIndices.push_back(meshIndex);
    }
  }

  textureBatchUpload.executeUploads(cmd);
  meshBatchUpload.executeUploads(cmd);

  for(const PendingTextureUploadState& state : textureUploadStates)
  {
    const rhi::TextureBarrier uploadEndBarrier{
        .texture = state.texture,
        .before = rhi::ResourceState::TransferDst,
        .after = rhi::ResourceState::General,
        .range =
            {
                .aspect = rhi::TextureAspect::color,
                .baseMipLevel = 0,
                .levelCount = state.mipLevels,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    cmd.resourceBarrier(&uploadEndBarrier, 1, nullptr, 0);
    m_device.device->destroyImage(state.texture);
  }

  rhi::BufferHandle textureStagingBuffer = textureBatchUpload.releaseStagingBuffer();
  if(!textureStagingBuffer.isNull())
  {
    m_device.rhiStagingBuffers.push_back(textureStagingBuffer);
  }
  rhi::BufferHandle meshStagingBuffer = meshBatchUpload.releaseStagingBuffer();
  if(!meshStagingBuffer.isNull())
  {
    m_meshPool.deferStagingBuffer(meshStagingBuffer);
  }

  const uint32_t gltfTextureBaseIndex = kDemoMaterialSlotCount;
  for(const uint32_t textureIndex : textureIndices)
  {
    if(textureIndex < ioResult.textures.size() && !ioResult.textures[textureIndex].isNull())
    {
      updateBindlessTexture(gltfTextureBaseIndex + textureIndex, ioResult.textures[textureIndex]);
    }
  }

  ioResult.transparentDistances.resize(ioResult.transparentMeshIndices.size(), 0.0f);
  ioResult.transparentSortDirty = true;
  rebuildShadowPackedBuffers(model, ioResult, cmd);
}

UploadBufferRecord RenderDevice::createShadowPackedUploadBuffer(rhi::CommandBuffer& cmd,
                                                                std::span<const std::byte> data,
                                                                rhi::BufferUsageFlags usage,
                                                                const char* debugName)
{
  if(data.empty())
  {
    return {};
  }

  const rhi::BufferHandle buffer = m_device.device->createBuffer(rhi::BufferDesc{
      .size = static_cast<uint64_t>(data.size_bytes()),
      .usage = usage | rhi::BufferUsageFlags::transferDst | rhi::BufferUsageFlags::shaderDeviceAddress,
      .memoryUsage = rhi::MemoryUsage::gpuOnly,
      .allowGpuAddress = true,
      .debugName = debugName,
  });
  const rhi::vulkan::BufferRecord* record = m_device.resourceTable.tryGetBuffer(buffer);
  ASSERT(record != nullptr, "RHI shadow packed buffer must be registered in the resource table");

  BatchUploadContext upload;
  upload.init(*m_device.device, static_cast<uint64_t>(data.size_bytes()));
  const BatchUploadContext::Slice slice = upload.allocate(static_cast<uint64_t>(data.size_bytes()), 16);
  std::memcpy(slice.cpuPtr, data.data(), data.size_bytes());
  upload.recordBufferUpload(slice, buffer, 0, static_cast<uint64_t>(data.size_bytes()));
  upload.executeUploads(cmd);

  rhi::BufferHandle staging = upload.releaseStagingBuffer();
  if(!staging.isNull())
  {
    m_device.rhiStagingBuffers.push_back(staging);
  }

  return toUploadBufferRecord(*record, buffer);
}

void RenderDevice::rebuildShadowPackedBuffers(const GltfModel& model, GltfUploadResult& result, rhi::CommandBuffer& cmd)
{
  if(result.shadowPackedVertexBuffer.buffer != 0 || result.shadowPackedIndexBuffer.buffer != 0)
  {
    waitForAllFrameSlots();
  }
  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedVertexBuffer);
  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedIndexBuffer);
  result.shadowPackedMeshes.clear();

  if(result.shadowCasterIndices.empty())
  {
    return;
  }

  std::vector<uint8_t>  packedVertexData;
  std::vector<uint32_t> packedIndexData;
  packedVertexData.reserve(result.shadowCasterIndices.size() * 48u * 64u);

  for(const size_t meshIndex : result.shadowCasterIndices)
  {
    if(meshIndex >= model.meshes.size())
    {
      continue;
    }

    const GltfMeshData& meshData = model.meshes[meshIndex];
    if(meshData.positions.empty() || meshData.indices.empty())
    {
      continue;
    }
    const MeshRecord* meshRecord = meshIndex < result.meshes.size()
        ? m_meshPool.tryGet(result.meshes[meshIndex])
        : nullptr;

    const uint32_t vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3u);
    const uint32_t vertexBase = static_cast<uint32_t>(packedVertexData.size() / 48u);
    const uint32_t firstIndex = static_cast<uint32_t>(packedIndexData.size());

    packedVertexData.resize(packedVertexData.size() + static_cast<size_t>(vertexCount) * 48u);
    for(uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
      float* dst = reinterpret_cast<float*>(packedVertexData.data() + (static_cast<size_t>(vertexBase) + vertexIndex) * 48u);

      dst[0] = meshData.positions[vertexIndex * 3u + 0u];
      dst[1] = meshData.positions[vertexIndex * 3u + 1u];
      dst[2] = meshData.positions[vertexIndex * 3u + 2u];

      if(!meshData.normals.empty())
      {
        dst[3] = meshData.normals[vertexIndex * 3u + 0u];
        dst[4] = meshData.normals[vertexIndex * 3u + 1u];
        dst[5] = meshData.normals[vertexIndex * 3u + 2u];
      }
      else
      {
        dst[3] = 0.0f;
        dst[4] = 1.0f;
        dst[5] = 0.0f;
      }

      if(!meshData.texCoords.empty())
      {
        dst[6] = meshData.texCoords[vertexIndex * 2u + 0u];
        dst[7] = meshData.texCoords[vertexIndex * 2u + 1u];
      }
      else
      {
        dst[6] = 0.0f;
        dst[7] = 0.0f;
      }

      if(!meshData.tangents.empty())
      {
        dst[8] = meshData.tangents[vertexIndex * 4u + 0u];
        dst[9] = meshData.tangents[vertexIndex * 4u + 1u];
        dst[10] = meshData.tangents[vertexIndex * 4u + 2u];
        dst[11] = meshData.tangents[vertexIndex * 4u + 3u];
      }
      else
      {
        dst[8] = 1.0f;
        dst[9] = 0.0f;
        dst[10] = 0.0f;
        dst[11] = 1.0f;
      }
    }

    for(uint32_t index : meshData.indices)
    {
      packedIndexData.push_back(vertexBase + index);
    }

    result.shadowPackedMeshes.push_back(ShadowPackedMesh{
        .meshIndex     = meshIndex,
        .indexCount    = static_cast<uint32_t>(meshData.indices.size()),
        .firstIndex    = firstIndex,
        .vertexOffset  = 0,
        .boundsSphere  = meshRecord != nullptr
            ? glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius)
            : glm::vec4(0.0f),
        .drawData      = meshRecord != nullptr ? buildShadowDrawUniforms(*meshRecord) : shaderio::DrawUniforms{},
    });
  }

  if(result.shadowPackedMeshes.empty())
  {
    return;
  }

  const std::span<const std::byte> packedVertexBytes(reinterpret_cast<const std::byte*>(packedVertexData.data()),
                                                     packedVertexData.size());
  const std::span<const std::byte> packedIndexBytes(reinterpret_cast<const std::byte*>(packedIndexData.data()),
                                                    packedIndexData.size() * sizeof(uint32_t));

  result.shadowPackedVertexBuffer =
      createShadowPackedUploadBuffer(cmd, packedVertexBytes, rhi::BufferUsageFlags::vertex, "ShadowPackedVertexBuffer");
  result.shadowPackedIndexBuffer =
      createShadowPackedUploadBuffer(cmd, packedIndexBytes, rhi::BufferUsageFlags::index, "ShadowPackedIndexBuffer");
}

void RenderDevice::rebuildShadowPackedBuffers(const SceneAsset& asset, SceneUploadResult& result, rhi::CommandBuffer& cmd)
{
  if(result.shadowPackedVertexBuffer.buffer != 0 || result.shadowPackedIndexBuffer.buffer != 0)
  {
    waitForAllFrameSlots();
  }
  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedVertexBuffer);
  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedIndexBuffer);
  result.shadowPackedMeshes.clear();

  if(result.shadowCasterDrawIndices.empty() && result.shadowCasterIndices.empty())
  {
    return;
  }

  std::vector<uint8_t> packedVertexData;
  std::vector<uint32_t> packedIndexData;

  const auto appendPackedMesh = [&](size_t meshIndex,
                                    const glm::vec4& boundsSphere,
                                    const shaderio::DrawUniforms& drawData) {
    if(meshIndex >= asset.meshes.size())
    {
      return;
    }

    const SceneMesh& mesh = asset.meshes[meshIndex];
    const size_t vertexByteOffset = mesh.vertexOffset;
    const size_t vertexByteSize = static_cast<size_t>(mesh.vertexCount) * 48u;
    const size_t indexByteOffset = mesh.indexOffset;
    const size_t indexByteSize = static_cast<size_t>(mesh.indexCount) * sizeof(uint32_t);
    if(vertexByteOffset + vertexByteSize > asset.vertexPayload.size()
       || indexByteOffset + indexByteSize > asset.indexPayload.size())
    {
      return;
    }

    const uint32_t vertexBase = static_cast<uint32_t>(packedVertexData.size() / 48u);
    const uint32_t firstIndex = static_cast<uint32_t>(packedIndexData.size());
    packedVertexData.insert(packedVertexData.end(),
                            asset.vertexPayload.begin() + static_cast<ptrdiff_t>(vertexByteOffset),
                            asset.vertexPayload.begin() + static_cast<ptrdiff_t>(vertexByteOffset + vertexByteSize));

    const uint32_t* sourceIndices = reinterpret_cast<const uint32_t*>(asset.indexPayload.data() + indexByteOffset);
    for(uint32_t index = 0; index < mesh.indexCount; ++index)
    {
      packedIndexData.push_back(vertexBase + sourceIndices[index]);
    }

    result.shadowPackedMeshes.push_back(ShadowPackedMesh{
        .meshIndex = meshIndex,
        .indexCount = mesh.indexCount,
        .firstIndex = firstIndex,
        .vertexOffset = 0,
        .boundsSphere = boundsSphere,
        .drawData = drawData,
    });
  };

  if(!result.shadowCasterDrawIndices.empty())
  {
    for(const size_t drawRecordIndex : result.shadowCasterDrawIndices)
    {
      if(drawRecordIndex >= result.drawRecords.size())
      {
        continue;
      }

      const SceneUploadResult::SceneDrawRecord& drawRecord = result.drawRecords[drawRecordIndex];
      if(drawRecord.meshIndex >= result.meshes.size())
      {
        continue;
      }

      const MeshRecord* meshRecord = m_meshPool.tryGet(drawRecord.meshHandle);
      if(meshRecord == nullptr)
      {
        continue;
      }

      const glm::vec4 boundsSphere = drawRecord.boundsSphere.w > 0.0f
          ? drawRecord.boundsSphere
          : computeBoundsSphere(*meshRecord, drawRecord.worldTransform);
      appendPackedMesh(drawRecord.meshIndex, boundsSphere, buildShadowDrawUniforms(*meshRecord, drawRecord));
    }
  }
  else
  {
    for(const size_t meshIndex : result.shadowCasterIndices)
    {
      if(meshIndex >= result.meshes.size())
      {
        continue;
      }

      const MeshRecord* meshRecord = m_meshPool.tryGet(result.meshes[meshIndex]);
      if(meshRecord == nullptr)
      {
        continue;
      }

      appendPackedMesh(meshIndex,
                       glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius),
                       buildShadowDrawUniforms(*meshRecord));
    }
  }

  if(result.shadowPackedMeshes.empty())
  {
    return;
  }

  const std::span<const std::byte> packedVertexBytes(reinterpret_cast<const std::byte*>(packedVertexData.data()),
                                                     packedVertexData.size());
  const std::span<const std::byte> packedIndexBytes(reinterpret_cast<const std::byte*>(packedIndexData.data()),
                                                    packedIndexData.size() * sizeof(uint32_t));

  result.shadowPackedVertexBuffer =
      createShadowPackedUploadBuffer(cmd, packedVertexBytes, rhi::BufferUsageFlags::vertex, "SceneShadowPackedVertexBuffer");
  result.shadowPackedIndexBuffer =
      createShadowPackedUploadBuffer(cmd, packedIndexBytes, rhi::BufferUsageFlags::index, "SceneShadowPackedIndexBuffer");
}

void RenderDevice::destroyGltfResources(const GltfUploadResult& result)
{
  VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getBackendDeviceHandle());
  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();

  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedVertexBuffer);
  destroyUploadBufferRecord(m_device.device.get(), m_device.allocator, result.shadowPackedIndexBuffer);

  // Destroy meshes
  for(MeshHandle handle : result.meshes)
  {
    m_meshPool.destroyMesh(handle);
  }

  // Destroy materials
  for(MaterialHandle handle : result.materials)
  {
    m_materials.materialPool.destroy(handle);
  }

  // Destroy textures
  for(size_t textureIndex = 0; textureIndex < result.textures.size(); ++textureIndex)
  {
    invalidateBindlessTexture(gltfTextureBaseIndex + static_cast<uint32_t>(textureIndex));
    TextureHandle handle = result.textures[textureIndex];
    if(const MaterialResources::TextureHotData* hot = tryGetTextureHot(handle); hot != nullptr && !hot->sampledViewHandle.isNull())
    {
      m_device.resourceTable.removeTextureView(hot->sampledViewHandle);
    }
    const MaterialResources::TextureColdData* cold = tryGetTextureCold(handle);
    if(cold && cold->ownedImage.image != VK_NULL_HANDLE)
    {
      utils::ImageResource image = cold->ownedImage;
      destroyImageResource(device, m_device.allocator, image);
    }
    m_materials.texturePool.destroy(handle);
  }
}

void RenderDevice::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  m_meshPool.updateTransform(handle, transform);
}

uint32_t RenderDevice::getFrameResourceCount() const
{
  return static_cast<uint32_t>(m_perFrame.frameUserData.size());
}

rhi::ArgumentTableHandle RenderDevice::getGraphicsMaterialArgumentTable() const
{
  return getCurrentMaterialArgumentTable();
}

bool RenderDevice::getIBLEnvironmentLoaded() const
{
  return m_device.iblEnvironmentLoaded;
}

bool RenderDevice::getIBLUsingFallback() const
{
  return m_device.iblUsingFallback;
}

rhi::TextureFormat RenderDevice::getIBLEnvironmentFormat() const
{
  return m_device.iblEnvironmentFormat;
}

rhi::Extent2D RenderDevice::getIBLEnvironmentExtent() const
{
  return m_device.iblEnvironmentExtent;
}

uint32_t RenderDevice::getIBLEnvironmentMipCount() const
{
  return m_device.iblEnvironmentMipCount;
}

uint64_t RenderDevice::getIBLEnvironmentEstimatedBytes() const
{
  return m_device.iblEnvironmentEstimatedBytes;
}

const std::string& RenderDevice::getIBLEnvironmentPath() const
{
  return m_device.iblEnvironmentPath;
}

const std::string& RenderDevice::getIBLEnvironmentStatus() const
{
  return m_device.iblEnvironmentStatus;
}

void RenderDevice::updateLightCoarseCullingResources(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms)
{
  m_lightResources.updatePointLights(frameIndex, m_testPointLights);
  m_lightResources.updateSpotLights(frameIndex, m_testSpotLights);
  m_lightResources.updateCoarseCullingUniforms(frameIndex, uniforms);
}

uint64_t RenderDevice::getGPUCullingObjectBufferAddress(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  const PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.useExternalGPUCullingObjectBuffer)
  {
    return frameUserData.externalGPUCullingObjectBufferAddress;
  }
  return static_cast<uint64_t>(frameUserData.gpuCullingObjectBuffer.address);
}

uint64_t RenderDevice::getGPUCullingResultBufferAddress(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return static_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingResultBuffer.address);
}

uint64_t RenderDevice::getGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingIndirectBuffer.buffer);
}

uint64_t RenderDevice::getGPUCullingDrawCountBufferOpaque(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingDrawCountBuffer.buffer);
}

uint32_t RenderDevice::getGPUCullingObjectCount(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return m_perFrame.frameUserData[frameIndex].gpuCullingObjectCount;
}

uint32_t RenderDevice::getCurrentFrameIndexHint() const
{
  if(m_perFrame.frameContext == nullptr)
  {
    return 0;
  }
  return m_perFrame.frameContext->getCurrentFrameIndex();
}

uint64_t RenderDevice::getPreviousGPUCullingIndirectBufferOpaque(uint32_t currentFrameIndex) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1)
  {
    return 0;
  }

  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[previousFrameIndex].gpuCullingIndirectBuffer.buffer);
}

uint64_t RenderDevice::getPreviousGPUCullingDrawCountBufferOpaque(uint32_t currentFrameIndex) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1)
  {
    return 0;
  }

  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[previousFrameIndex].gpuCullingDrawCountBuffer.buffer);
}

uint64_t RenderDevice::getGPUDrivenPersistentIndirectStreamBuffer(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuDrivenPersistentIndirectStreamBuffer.buffer);
}

void RenderDevice::rebindFrameBufferHandle(rhi::BufferHandle& handle, const utils::Buffer& buffer)
{
  rebindFrameBufferHandle(handle, buffer.buffer);
}

void RenderDevice::rebindFrameBufferHandle(rhi::BufferHandle& handle, VkBuffer buffer)
{
  if(buffer == VK_NULL_HANDLE)
  {
    if(!handle.isNull())
    {
      m_device.resourceTable.removeBuffer(handle);
      handle = {};
    }
    return;
  }
  const uint64_t native = reinterpret_cast<uint64_t>(buffer);
  if(handle.isNull())
  {
    rhi::vulkan::BufferRecord rec{};
    rec.nativeBuffer = native;
    rec.owned        = false;  // FrameUserData owns the VMA lifetime; registry mirrors only.
    handle = m_device.resourceTable.registerBuffer(rec);
  }
  else
  {
    m_device.resourceTable.updateBuffer(handle, native);
  }
}

rhi::BufferHandle RenderDevice::getGPUCullingIndirectBufferRHIHandle(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size()) return {};
  return m_perFrame.frameUserData[frameIndex].gpuCullingIndirectBufferRHI;
}

rhi::BufferHandle RenderDevice::getGPUCullingDrawCountBufferRHIHandle(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size()) return {};
  return m_perFrame.frameUserData[frameIndex].gpuCullingDrawCountBufferRHI;
}

rhi::BufferHandle RenderDevice::getPreviousGPUCullingIndirectBufferRHIHandle(uint32_t currentFrameIndex) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1) return {};
  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  return m_perFrame.frameUserData[previousFrameIndex].gpuCullingIndirectBufferRHI;
}

rhi::BufferHandle RenderDevice::getPreviousGPUCullingDrawCountBufferRHIHandle(uint32_t currentFrameIndex) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1) return {};
  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  return m_perFrame.frameUserData[previousFrameIndex].gpuCullingDrawCountBufferRHI;
}

rhi::BufferHandle RenderDevice::getGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size()) return {};
  return m_perFrame.frameUserData[frameIndex].gpuDrivenPersistentIndirectStreamBufferRHI;
}

rhi::BufferHandle RenderDevice::getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t currentFrameIndex) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1) return {};
  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  return m_perFrame.frameUserData[previousFrameIndex].gpuDrivenPersistentIndirectStreamBufferRHI;
}

rhi::BufferHandle RenderDevice::getShadowCullingIndirectBufferRHIHandle(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size()) return {};
  return m_perFrame.frameUserData[frameIndex].shadowCullingIndirectBufferRHI;
}

uint32_t RenderDevice::getPreviousGPUCullingObjectCount(uint32_t currentFrameIndex, const GltfUploadResult* gltfModel) const
{
  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  if(frameCount == 0 || currentFrameIndex >= frameCount || m_perFrame.frameCounter <= 1)
  {
    return 0;
  }

  const uint32_t previousFrameIndex = (currentFrameIndex + frameCount - 1u) % frameCount;
  const PerFrameResources::FrameUserData& previousFrame = m_perFrame.frameUserData[previousFrameIndex];
  if(previousFrame.useExternalGPUCullingObjectBuffer)
  {
    return previousFrame.gpuCullingObjectCount;
  }
  if(gltfModel == nullptr)
  {
    return 0;
  }
  if(previousFrame.gpuCullingSourceModel != gltfModel)
  {
    return 0;
  }

  return previousFrame.gpuCullingObjectCount;
}

rhi::ArgumentTableHandle RenderDevice::getCameraArgumentTable(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].cameraArgumentTable;
  }
  return {};
}

rhi::ArgumentTableHandle RenderDevice::getDrawArgumentTable(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].drawArgumentTable;
  }
  return {};
}

rhi::ArgumentTableHandle RenderDevice::getMDIDrawArgumentTable(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].mdiDrawArgumentTable;
  }
  return {};
}

rhi::ArgumentTableHandle RenderDevice::getGBufferMDIDrawArgumentTable(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].gbufferMdiDrawArgumentTable;
  }
  return {};
}

rhi::ArgumentTableHandle RenderDevice::getDepthMDIDrawArgumentTable(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].depthMdiDrawArgumentTable;
  }
  return {};
}

rhi::ArgumentTableHandle RenderDevice::getCSMShadowMDIDrawArgumentTable(uint32_t frameIndex, uint32_t cascadeIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size() && cascadeIndex < shaderio::LCascadeCount)
  {
    return m_perFrame.frameUserData[frameIndex].csmShadowMdiDrawArgumentTables[cascadeIndex];
  }
  return {};
}

uint64_t RenderDevice::getForwardMDIIndirectBuffer(uint32_t frameIndex) const
{
  return getGPUDrivenPersistentIndirectStreamBuffer(frameIndex);
}

void RenderDevice::uploadMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureMdiDrawDataBuffer(frameUserData, static_cast<uint32_t>(drawData.size()));
  if(frameUserData.mdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBuffer(m_device.allocator, frameUserData.mdiDrawDataBuffer, drawData.data(),
                         sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::uploadMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex, std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureMdiDrawDataBuffer(frameUserData, firstDrawIndex + static_cast<uint32_t>(drawData.size()));
  if(frameUserData.mdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBufferRange(m_device.allocator,
                              frameUserData.mdiDrawDataBuffer,
                              static_cast<VkDeviceSize>(firstDrawIndex) * sizeof(shaderio::DrawUniforms),
                              drawData.data(),
                              sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::uploadGBufferMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureGBufferMdiDrawDataBuffer(frameUserData, static_cast<uint32_t>(drawData.size()));
  if(frameUserData.gbufferMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBuffer(m_device.allocator, frameUserData.gbufferMdiDrawDataBuffer, drawData.data(),
                         sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::uploadGBufferMDIDrawDataRange(uint32_t frameIndex,
                                             uint32_t firstDrawIndex,
                                             std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureGBufferMdiDrawDataBuffer(frameUserData, firstDrawIndex + static_cast<uint32_t>(drawData.size()));
  if(frameUserData.gbufferMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBufferRange(m_device.allocator,
                              frameUserData.gbufferMdiDrawDataBuffer,
                              static_cast<VkDeviceSize>(firstDrawIndex) * sizeof(shaderio::DrawUniforms),
                              drawData.data(),
                              sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::uploadDepthMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureDepthMdiDrawDataBuffer(frameUserData, static_cast<uint32_t>(drawData.size()));
  if(frameUserData.depthMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBuffer(m_device.allocator, frameUserData.depthMdiDrawDataBuffer, drawData.data(),
                         sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::uploadDepthMDIDrawDataRange(uint32_t frameIndex,
                                           uint32_t firstDrawIndex,
                                           std::span<const shaderio::DrawUniforms> drawData)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || drawData.empty())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureDepthMdiDrawDataBuffer(frameUserData, firstDrawIndex + static_cast<uint32_t>(drawData.size()));
  if(frameUserData.depthMdiDrawDataBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  writeHostVisibleBufferRange(m_device.allocator,
                              frameUserData.depthMdiDrawDataBuffer,
                              static_cast<VkDeviceSize>(firstDrawIndex) * sizeof(shaderio::DrawUniforms),
                              drawData.data(),
                              sizeof(shaderio::DrawUniforms) * drawData.size());
}

void RenderDevice::ensureGPUDrivenPersistentIndirectStream(uint32_t frameIndex, uint32_t requiredDrawCount)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  ensureGPUDrivenPersistentIndirectStreamBuffer(frameUserData, requiredDrawCount);
}

rhi::ArgumentTableHandle RenderDevice::getGlobalBindlessGroup() const
{
  return getCurrentMaterialArgumentTable();
}

rhi::BufferHandle RenderDevice::getCurrentTransientBufferHandle() const
{
  uint32_t frameIndex = 0;
  if(m_perFrame.frameContext != nullptr)
  {
    frameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  }
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return {};
  }
  return m_perFrame.frameUserData[frameIndex].transientBufferRHI;
}

rhi::BufferHandle RenderDevice::getTransientBufferHandle(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return {};
  }
  return m_perFrame.frameUserData[frameIndex].transientBufferRHI;
}

glm::vec4 RenderDevice::getMaterialBaseColorFactor(MaterialHandle handle) const
{
  const MaterialResources::MaterialRecord* material = tryGetMaterial(handle);
  if(material)
  {
    return material->baseColorFactor;
  }
  return glm::vec4(1.0f);
}

int32_t RenderDevice::getMaterialBaseColorTextureIndex(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const
{
  if(!gltfModel)
  {
    return -1;
  }

  const MaterialResources::MaterialRecord* material = tryGetMaterial(materialHandle);
  if(!material || material->baseColorTexture.isNull())
  {
    return -1;
  }

  // Find texture index in gltfModel->textures
  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();
  for(size_t i = 0; i < gltfModel->textures.size(); ++i)
  {
    if(gltfModel->textures[i] == material->baseColorTexture)
    {
      return static_cast<int32_t>(gltfTextureBaseIndex + i);
    }
  }

  return -1;
}

RenderDevice::MaterialTextureIndices RenderDevice::getMaterialTextureIndices(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const
{
  MaterialTextureIndices result;

  if(!gltfModel)
  {
    return result;
  }

  const MaterialResources::MaterialRecord* material = tryGetMaterial(materialHandle);
  if(!material)
  {
    return result;
  }

  // Fill PBR factors
  result.metallicFactor = material->metallicFactor;
  result.roughnessFactor = material->roughnessFactor;
  result.normalScale = material->normalScale;

  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();

  // Helper to find texture index
  auto findTextureIndex = [&](TextureHandle handle) -> int32_t {
    if(handle.isNull())
    {
      return -1;
    }
    for(size_t i = 0; i < gltfModel->textures.size(); ++i)
    {
      if(gltfModel->textures[i] == handle)
      {
        return static_cast<int32_t>(gltfTextureBaseIndex + i);
      }
    }
    return -1;
  };

  result.baseColor = findTextureIndex(material->baseColorTexture);
  result.normal = findTextureIndex(material->normalTexture);
  result.metallicRoughness = findTextureIndex(material->metallicRoughnessTexture);
  result.occlusion = findTextureIndex(material->occlusionTexture);

  // Fill alpha properties
  result.alphaMode = material->alphaMode;
  result.alphaCutoff = material->alphaCutoff;

  return result;
}

void RenderDevice::updateBindlessTexture(uint32_t index, TextureHandle textureHandle)
{
  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(textureHandle);
  if(!textureHot || textureHot->runtimeKind != MaterialResources::TextureRuntimeKind::materialSampled)
  {
    return;
  }

  // Wave 8: cache the adopted view handle; the shared materialSamplerHandle pairs with it
  // through combinedImageSampler ArgumentWrites in syncMaterialArgumentTable.
  if(index < m_materials.materialDescriptorViews.size())
  {
    m_materials.materialDescriptorViews[index] = textureHot->sampledViewHandle;
    m_materials.materialDescriptorValid[index] = 1;
    ++m_materials.materialDescriptorGeneration;
  }
}

void RenderDevice::invalidateBindlessTexture(uint32_t index)
{
  if(index >= m_materials.materialDescriptorViews.size() || index >= m_materials.materialDescriptorValid.size())
  {
    return;
  }

  m_materials.materialDescriptorViews[index] = {};
  if(m_materials.materialDescriptorValid[index] != 0)
  {
    m_materials.materialDescriptorValid[index] = 0;
    ++m_materials.materialDescriptorGeneration;
  }
}

}  // namespace demo
