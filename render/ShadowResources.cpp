#include "ShadowResources.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace demo {

namespace {

constexpr float kDefaultMaxShadowDistance = 100.0f;
constexpr float kReceiverBias             = 0.0015f;
constexpr float kDepthBiasConstant        = 1.25f;
constexpr float kDepthBiasSlope           = 1.75f;
constexpr float kShadowIntensity          = 1.0f;
constexpr float kShadowKernelRadius       = 1.0f;  // 1 => 3x3 PCF

[[nodiscard]] float extractNearPlane(const glm::mat4& projection)
{
  const float a = projection[2][2];
  const float b = projection[3][2];
  return std::abs(a) > 1e-5f ? b / a : 0.1f;
}

[[nodiscard]] float extractFarPlane(const glm::mat4& projection)
{
  const float a = projection[2][2];
  const float b = projection[3][2];
  return std::abs(a + 1.0f) > 1e-5f ? b / (a + 1.0f) : kDefaultMaxShadowDistance;
}

[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
  const float lengthSq = glm::dot(value, value);
  return lengthSq > 1e-6f ? value * glm::inversesqrt(lengthSq) : fallback;
}

[[nodiscard]] std::array<glm::vec3, 8> buildFrustumSliceCornersWorld(
    const shaderio::CameraUniforms& camera,
    const clipspace::ProjectionConvention& projectionConvention,
    float sliceFarDistance)
{
  const glm::mat4 invViewProjection = glm::inverse(camera.viewProjection);
  const float cameraNear = std::max(0.01f, extractNearPlane(camera.projection));
  const float cameraFar  = std::max(cameraNear + 0.01f, extractFarPlane(camera.projection));
  const float sliceFar   = glm::clamp(sliceFarDistance, cameraNear + 0.01f, cameraFar);
  const float sliceLerp  = (sliceFar - cameraNear) / std::max(0.01f, cameraFar - cameraNear);

  const std::array<glm::vec2, 4> ndcCorners = {
      glm::vec2(-1.0f, -1.0f),
      glm::vec2(1.0f, -1.0f),
      glm::vec2(1.0f, 1.0f),
      glm::vec2(-1.0f, 1.0f),
  };

  std::array<glm::vec3, 4> nearCorners{};
  std::array<glm::vec3, 4> farCorners{};

  for(size_t i = 0; i < ndcCorners.size(); ++i)
  {
    glm::vec4 nearCorner = invViewProjection * glm::vec4(ndcCorners[i], projectionConvention.ndcNearZ, 1.0f);
    glm::vec4 farCorner  = invViewProjection * glm::vec4(ndcCorners[i], projectionConvention.ndcFarZ, 1.0f);
    nearCorner /= nearCorner.w;
    farCorner /= farCorner.w;
    nearCorners[i] = glm::vec3(nearCorner);
    farCorners[i]  = glm::vec3(farCorner);
  }

  std::array<glm::vec3, 8> sliceCorners{};
  for(size_t i = 0; i < ndcCorners.size(); ++i)
  {
    const glm::vec3 ray = farCorners[i] - nearCorners[i];
    sliceCorners[i]     = nearCorners[i];
    sliceCorners[i + 4] = nearCorners[i] + ray * sliceLerp;
  }

  return sliceCorners;
}

[[nodiscard]] glm::vec3 computeFrustumCenter(const std::array<glm::vec3, 8>& corners)
{
  glm::vec3 center(0.0f);
  for(const glm::vec3& corner : corners)
  {
    center += corner;
  }
  return center / static_cast<float>(corners.size());
}

}  // namespace

void ShadowResources::init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  m_device               = device;
  m_allocator            = allocator;
  m_shadowMapSize        = createInfo.shadowMapSize;
  m_projectionConvention = createInfo.projectionConvention;

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();

  const VkImageCreateInfo shadowInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.shadowFormat,
      .extent = {m_shadowMapSize, m_shadowMapSize, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateImage(
      m_allocator, &shadowInfo, &imageAllocInfo, &m_shadowMapImage.image, &m_shadowMapImage.allocation, nullptr));
  dutil.setObjectName(m_shadowMapImage.image, "Shadow_Map");

  const VkImageViewCreateInfo viewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_shadowMapImage.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = createInfo.shadowFormat,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_shadowMapView));
  dutil.setObjectName(m_shadowMapView, "Shadow_MapView");

  const VkBufferCreateInfo uniformInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(shaderio::ShadowUniforms),
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
  };
  VmaAllocationInfo uniformAllocInfo{};
  const VmaAllocationCreateInfo uniformCreateInfo{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
      .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
  };
  VK_CHECK(vmaCreateBuffer(
      m_allocator, &uniformInfo, &uniformCreateInfo, &m_shadowUniformBuffer.buffer,
      &m_shadowUniformBuffer.allocation, &uniformAllocInfo));
  m_shadowUniformMapped = nullptr;
  VK_CHECK(vmaMapMemory(m_allocator, m_shadowUniformBuffer.allocation, &m_shadowUniformMapped));
  dutil.setObjectName(m_shadowUniformBuffer.buffer, "Shadow_UniformBuffer");

  const VkImageMemoryBarrier2 initBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = m_shadowMapImage.image,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
  };
  const VkDependencyInfo initDepInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &initBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &initDepInfo);
}

void ShadowResources::deinit()
{
  if(m_shadowUniformMapped != nullptr)
  {
    vmaUnmapMemory(m_allocator, m_shadowUniformBuffer.allocation);
    m_shadowUniformMapped = nullptr;
  }

  if(m_shadowMapView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_shadowMapView, nullptr);
  }
  if(m_shadowMapImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_shadowMapImage.image, m_shadowMapImage.allocation);
  }
  if(m_shadowUniformBuffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_shadowUniformBuffer.buffer, m_shadowUniformBuffer.allocation);
  }

  *this = ShadowResources{};
}

void ShadowResources::updateShadowMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir)
{
  const float cameraFar          = std::max(1.0f, extractFarPlane(camera.projection));
  const float maxShadowDistance  = std::min(kDefaultMaxShadowDistance, cameraFar);
  const glm::vec3 lightDirection = safeNormalize(lightDir, glm::vec3(0.0f, -1.0f, 0.0f));

  const std::array<glm::vec3, 8> sliceCorners =
      buildFrustumSliceCornersWorld(camera, m_projectionConvention, maxShadowDistance);

  const glm::vec3 frustumCenter = computeFrustumCenter(sliceCorners);

  float frustumRadius = 0.0f;
  for(const glm::vec3& corner : sliceCorners)
  {
    frustumRadius = std::max(frustumRadius, glm::length(corner - frustumCenter));
  }

  const glm::vec3 worldUp = std::abs(lightDirection.y) > 0.95f
      ? glm::vec3(0.0f, 0.0f, 1.0f)
      : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 lightPosition = frustumCenter - lightDirection * (frustumRadius + maxShadowDistance);
  const glm::mat4 lightView     = glm::lookAt(lightPosition, frustumCenter, worldUp);

  glm::vec3 minLightSpace(std::numeric_limits<float>::max());
  glm::vec3 maxLightSpace(std::numeric_limits<float>::lowest());
  for(const glm::vec3& corner : sliceCorners)
  {
    const glm::vec3 lightSpace = glm::vec3(lightView * glm::vec4(corner, 1.0f));
    minLightSpace = glm::min(minLightSpace, lightSpace);
    maxLightSpace = glm::max(maxLightSpace, lightSpace);
  }

  const float halfExtentX       = 0.5f * (maxLightSpace.x - minLightSpace.x);
  const float halfExtentY       = 0.5f * (maxLightSpace.y - minLightSpace.y);
  const float halfExtent        = std::max(halfExtentX, halfExtentY) + 4.0f;
  glm::vec2 lightCenterXY       = 0.5f * glm::vec2(minLightSpace.x + maxLightSpace.x, minLightSpace.y + maxLightSpace.y);
  const float worldUnitsPerTexel = (halfExtent * 2.0f) / static_cast<float>(m_shadowMapSize);
  if(worldUnitsPerTexel > 0.0f)
  {
    lightCenterXY = glm::floor(lightCenterXY / worldUnitsPerTexel) * worldUnitsPerTexel;
  }

  const float left   = lightCenterXY.x - halfExtent;
  const float right  = lightCenterXY.x + halfExtent;
  const float bottom = lightCenterXY.y - halfExtent;
  const float top    = lightCenterXY.y + halfExtent;

  const float depthPadding = std::max(25.0f, frustumRadius);
  const float nearPlane    = std::max(0.1f, -maxLightSpace.z - depthPadding);
  const float farPlane     = std::max(nearPlane + 1.0f, -minLightSpace.z + depthPadding);

  const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
      left, right, bottom, top, nearPlane, farPlane, m_projectionConvention);
  const glm::mat4 lightViewProjection = lightProjection * lightView;

  m_shadowUniformsData.lightViewProjectionMatrix  = lightViewProjection;
  m_shadowUniformsData.worldToShadowTextureMatrix =
      clipspace::makeNdcToShadowTextureMatrix(m_projectionConvention) * lightViewProjection;
  m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(lightDirection, kShadowIntensity);
  m_shadowUniformsData.shadowMapMetrics = glm::vec4(
      1.0f / static_cast<float>(m_shadowMapSize), maxShadowDistance, kReceiverBias, 0.0f);
  m_shadowUniformsData.shadowBiasAndKernel = glm::vec4(
      kDepthBiasConstant, kDepthBiasSlope, static_cast<float>(m_shadowMapSize), kShadowKernelRadius);

  if(m_shadowUniformMapped == nullptr)
  {
    VK_CHECK(vmaMapMemory(m_allocator, m_shadowUniformBuffer.allocation, &m_shadowUniformMapped));
  }

  std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
  vmaFlushAllocation(m_allocator, m_shadowUniformBuffer.allocation, 0, sizeof(m_shadowUniformsData));
}

}  // namespace demo
