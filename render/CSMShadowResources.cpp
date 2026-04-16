#include "CSMShadowResources.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace demo {

namespace {

// Shadow parameters (consistent with single shadow system)
constexpr float kDefaultMaxShadowDistance    = 100.0f;
constexpr float kReceiverBias                = 0.0015f;
constexpr float kDepthBiasConstant           = 1.25f;
constexpr float kDepthBiasSlope              = 1.75f;
constexpr float kShadowIntensity             = 1.0f;
constexpr float kShadowKernelRadius          = 1.0f;  // 1 => 3x3 PCF
constexpr float kCascadeBiasScaleFactor      = 0.5f;  // Bias decreases for farther cascades
constexpr float kCascadeNearPlanePadding     = 25.0f; // Padding for near plane in ortho projection

[[nodiscard]] float extractNearPlane(const glm::mat4& projection, const clipspace::ProjectionConvention& convention)
{
  return clipspace::extractPerspectiveNearPlane(projection, convention);
}

[[nodiscard]] float extractFarPlane(const glm::mat4& projection, const clipspace::ProjectionConvention& convention)
{
  return clipspace::extractPerspectiveFarPlane(projection, convention);
}

[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
  const float lengthSq = glm::dot(value, value);
  return lengthSq > 1e-6f ? value * glm::inversesqrt(lengthSq) : fallback;
}

// Compute cascade split distances using practical split scheme
// Lambda blends logarithmic (better for perspective) and uniform (better for uniform distribution)
void computeCascadeSplits(float* splits, uint32_t count, float nearDist, float farDist, float lambda)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    const float fraction  = static_cast<float>(i + 1) / static_cast<float>(count);
    const float uniformSplit = nearDist + (farDist - nearDist) * fraction;
    const float logSplit     = nearDist * std::pow(farDist / nearDist, fraction);
    splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
  }
}

// Compute frustum corners for a slice between sliceNear and sliceFar distances
[[nodiscard]] std::array<glm::vec3, 8> computeFrustumSliceCornersWorld(
    const shaderio::CameraUniforms& camera,
    const clipspace::ProjectionConvention& projectionConvention,
    float sliceNear,
    float sliceFar)
{
  const glm::mat4 invViewProjection = glm::inverse(camera.viewProjection);
  const float cameraNear = std::max(0.01f, extractNearPlane(camera.projection, projectionConvention));
  const float cameraFar  = std::max(cameraNear + 0.01f, extractFarPlane(camera.projection, projectionConvention));

  // Clamp slice distances to valid range
  sliceNear = glm::clamp(sliceNear, cameraNear, cameraFar);
  sliceFar  = glm::clamp(sliceFar, sliceNear + 0.01f, cameraFar);

  // Compute lerp factors for near and far planes of the slice
  const float nearLerp = (sliceNear - cameraNear) / std::max(0.01f, cameraFar - cameraNear);
  const float farLerp  = (sliceFar - cameraNear) / std::max(0.01f, cameraFar - cameraNear);

  const std::array<glm::vec2, 4> ndcCorners = {
      glm::vec2(-1.0f, -1.0f),
      glm::vec2(1.0f, -1.0f),
      glm::vec2(1.0f, 1.0f),
      glm::vec2(-1.0f, 1.0f),
  };

  std::array<glm::vec3, 4> cameraNearCorners{};
  std::array<glm::vec3, 4> cameraFarCorners{};

  // Transform NDC corners to world space
  for(size_t i = 0; i < ndcCorners.size(); ++i)
  {
    glm::vec4 nearCorner = invViewProjection * glm::vec4(ndcCorners[i], projectionConvention.ndcNearZ, 1.0f);
    glm::vec4 farCorner  = invViewProjection * glm::vec4(ndcCorners[i], projectionConvention.ndcFarZ, 1.0f);
    nearCorner /= nearCorner.w;
    farCorner /= farCorner.w;
    cameraNearCorners[i] = glm::vec3(nearCorner);
    cameraFarCorners[i]  = glm::vec3(farCorner);
  }

  // Interpolate corners to slice boundaries
  std::array<glm::vec3, 8> sliceCorners{};
  for(size_t i = 0; i < ndcCorners.size(); ++i)
  {
    const glm::vec3 ray = cameraFarCorners[i] - cameraNearCorners[i];
    sliceCorners[i]     = cameraNearCorners[i] + ray * nearLerp;  // Near corners of slice
    sliceCorners[i + 4] = cameraNearCorners[i] + ray * farLerp;   // Far corners of slice
  }

  return sliceCorners;
}

// Compute bounding sphere from frustum corners (rotation-stable)
struct BoundingSphere
{
  glm::vec3 center;
  float     radius;
};

[[nodiscard]] BoundingSphere computeBoundingSphere(const std::array<glm::vec3, 8>& corners)
{
  // Compute center as average of corners
  glm::vec3 center(0.0f);
  for(const glm::vec3& corner : corners)
  {
    center += corner;
  }
  center /= static_cast<float>(corners.size());

  // Compute radius as maximum distance from center
  float radius = 0.0f;
  for(const glm::vec3& corner : corners)
  {
    radius = std::max(radius, glm::length(corner - center));
  }

  return {center, radius};
}

// Snap projection bounds to texel grid for shadow stability
void snapToTexelGrid(float& left, float& right, float& bottom, float& top, uint32_t resolution)
{
  const float diameter  = std::max(right - left, top - bottom);
  const float texelSize = diameter / static_cast<float>(resolution);

  if(texelSize > 0.0f)
  {
    left   = std::floor(left / texelSize) * texelSize;
    right  = std::ceil(right / texelSize) * texelSize;
    bottom = std::floor(bottom / texelSize) * texelSize;
    top    = std::ceil(top / texelSize) * texelSize;
  }
}

}  // namespace

void CSMShadowResources::init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  m_device               = device;
  m_allocator            = allocator;
  m_cascadeCount         = createInfo.cascadeCount;
  m_cascadeResolution    = createInfo.cascadeResolution;
  m_shadowFormat         = createInfo.shadowFormat;
  m_projectionConvention = createInfo.projectionConvention;

  assert(m_cascadeCount <= shaderio::LCascadeCount && "Cascade count exceeds maximum");

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();

  // Create Texture2DArray for all cascades
  const VkImageCreateInfo cascadeArrayInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = m_shadowFormat,
      .extent        = {m_cascadeResolution, m_cascadeResolution, 1},
      .mipLevels     = 1,
      .arrayLayers   = m_cascadeCount,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateImage(
      m_allocator, &cascadeArrayInfo, &imageAllocInfo, &m_cascadeArray.image, &m_cascadeArray.allocation, nullptr));
  dutil.setObjectName(m_cascadeArray.image, "CSM_CascadeArray");

  // Create full array view (for sampling in shaders)
  const VkImageViewCreateInfo arrayViewInfo{
      .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image      = m_cascadeArray.image,
      .viewType   = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format     = m_shadowFormat,
      .subresourceRange =
          {
              .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
              .baseMipLevel   = 0,
              .levelCount     = 1,
              .baseArrayLayer = 0,
              .layerCount     = m_cascadeCount,
          },
  };
  VK_CHECK(vkCreateImageView(m_device, &arrayViewInfo, nullptr, &m_cascadeArrayView));
  dutil.setObjectName(m_cascadeArrayView, "CSM_CascadeArrayView");

  // Create per-layer views (for rendering each cascade)
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    const VkImageViewCreateInfo layerViewInfo{
        .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image      = m_cascadeArray.image,
        .viewType   = VK_IMAGE_VIEW_TYPE_2D,
        .format     = m_shadowFormat,
        .subresourceRange =
            {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = i,
                .layerCount     = 1,
            },
    };
    VK_CHECK(vkCreateImageView(m_device, &layerViewInfo, nullptr, &m_cascadeLayerViews[i]));

    char name[32];
    std::snprintf(name, sizeof(name), "CSM_CascadeLayerView_%u", i);
    dutil.setObjectName(m_cascadeLayerViews[i], name);
  }

  // Create uniform buffer for shadow data
  const VkBufferCreateInfo uniformInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size  = sizeof(shaderio::ShadowUniforms),
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

  VK_CHECK(vmaMapMemory(m_allocator, m_shadowUniformBuffer.allocation, &m_shadowUniformMapped));
  dutil.setObjectName(m_shadowUniformBuffer.buffer, "CSM_ShadowUniformBuffer");

  // Initialize shadow uniforms with defaults
  m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(0.0f, -1.0f, 0.0f, kShadowIntensity);
  m_shadowUniformsData.shadowMapMetrics = glm::vec4(
      1.0f / static_cast<float>(m_cascadeResolution),
      kDefaultMaxShadowDistance,
      0.0f,
      static_cast<float>(m_cascadeCount));
  m_shadowUniformsData.cascadeBiasScale = glm::vec4(
      kDepthBiasConstant,
      kDepthBiasSlope,
      kCascadeBiasScaleFactor,
      0.0f);  // normal bias placeholder

  std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
  vmaFlushAllocation(m_allocator, m_shadowUniformBuffer.allocation, 0, sizeof(m_shadowUniformsData));

  // Transition image to depth-stencil read-only optimal (ready for sampling)
  const VkImageMemoryBarrier2 initBarrier{
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
      .srcAccessMask       = VK_ACCESS_2_NONE,
      .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = m_cascadeArray.image,
      .subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_cascadeCount},
  };

  const VkDependencyInfo initDepInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &initBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &initDepInfo);
}

void CSMShadowResources::deinit()
{
  // Cleanup in reverse order of creation

  if(m_shadowUniformMapped != nullptr)
  {
    vmaUnmapMemory(m_allocator, m_shadowUniformBuffer.allocation);
    m_shadowUniformMapped = nullptr;
  }

  if(m_shadowUniformBuffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_shadowUniformBuffer.buffer, m_shadowUniformBuffer.allocation);
  }

  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    if(m_cascadeLayerViews[i] != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, m_cascadeLayerViews[i], nullptr);
    }
  }

  if(m_cascadeArrayView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_cascadeArrayView, nullptr);
  }

  if(m_cascadeArray.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_cascadeArray.image, m_cascadeArray.allocation);
  }

  *this = CSMShadowResources{};
}

void CSMShadowResources::updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir)
{
  const float cameraFar         = std::max(1.0f, extractFarPlane(camera.projection, m_projectionConvention));
  const float maxShadowDistance = std::min(kDefaultMaxShadowDistance, cameraFar);
  const float cameraNear        = std::max(0.01f, extractNearPlane(camera.projection, m_projectionConvention));
  const glm::vec3 lightDirection = safeNormalize(lightDir, glm::vec3(0.0f, -1.0f, 0.0f));

  // Compute cascade split distances using practical split scheme
  float cascadeSplits[shaderio::LCascadeCount];
  computeCascadeSplits(cascadeSplits, m_cascadeCount, cameraNear, maxShadowDistance, shaderio::LCascadeSplitLambda);

  // Store split distances in uniform data
  m_shadowUniformsData.cascadeSplitDistances = glm::vec4(
      cascadeSplits[0],
      m_cascadeCount > 1 ? cascadeSplits[1] : 0.0f,
      m_cascadeCount > 2 ? cascadeSplits[2] : 0.0f,
      m_cascadeCount > 3 ? cascadeSplits[3] : 0.0f);

  // Choose world-up vector for light view matrix (avoid near-parallel with light direction)
  const glm::vec3 worldUp = std::abs(lightDirection.y) > 0.95f
      ? glm::vec3(0.0f, 0.0f, 1.0f)
      : glm::vec3(0.0f, 1.0f, 0.0f);

  // Compute each cascade's view-projection matrix
  float prevSplitDistance = cameraNear;
  for(uint32_t cascadeIndex = 0; cascadeIndex < m_cascadeCount; ++cascadeIndex)
  {
    const float splitDistance = cascadeSplits[cascadeIndex];

    // Get frustum corners for this cascade slice
    const std::array<glm::vec3, 8> sliceCorners =
        computeFrustumSliceCornersWorld(camera, m_projectionConvention, prevSplitDistance, splitDistance);

    // Compute bounding sphere (rotation-stable)
    const BoundingSphere boundingSphere = computeBoundingSphere(sliceCorners);

    // Position light camera to view the bounding sphere
    const glm::vec3 lightPosition = boundingSphere.center - lightDirection * boundingSphere.radius;
    const glm::mat4 lightView     = glm::lookAt(lightPosition, boundingSphere.center, worldUp);

    // Transform corners to light space to find ortho bounds
    glm::vec3 minLightSpace(std::numeric_limits<float>::max());
    glm::vec3 maxLightSpace(std::numeric_limits<float>::lowest());

    for(const glm::vec3& corner : sliceCorners)
    {
      const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
      minLightSpace = glm::min(minLightSpace, lightSpaceCorner);
      maxLightSpace = glm::max(maxLightSpace, lightSpaceCorner);
    }

    // Use bounding sphere diameter for uniform projection (rotation stability)
    const float diameter  = boundingSphere.radius * 2.0f;
    const float halfSize  = diameter * 0.5f;
    const glm::vec2 centerXY = glm::vec2(
        (minLightSpace.x + maxLightSpace.x) * 0.5f,
        (minLightSpace.y + maxLightSpace.y) * 0.5f);

    float left   = centerXY.x - halfSize;
    float right  = centerXY.x + halfSize;
    float bottom = centerXY.y - halfSize;
    float top    = centerXY.y + halfSize;

    // Snap bounds to texel grid for stability
    snapToTexelGrid(left, right, bottom, top, m_cascadeResolution);

    // Compute near/far planes with padding
    const float depthPadding = std::max(kCascadeNearPlanePadding, boundingSphere.radius);
    const float nearPlane    = std::max(0.1f, -maxLightSpace.z - depthPadding);
    const float farPlane     = std::max(nearPlane + 1.0f, -minLightSpace.z + depthPadding);

    // Create orthographic projection
    const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
        left, right, bottom, top, nearPlane, farPlane, m_projectionConvention);

    const glm::mat4 lightViewProjection = lightProjection * lightView;

    // Store cascade matrices
    m_shadowUniformsData.cascadeViewProjection[cascadeIndex]          = lightViewProjection;
    m_shadowUniformsData.cascadeWorldToShadowTexture[cascadeIndex] =
        clipspace::makeNdcToShadowTextureMatrix(m_projectionConvention) * lightViewProjection;

    prevSplitDistance = splitDistance;
  }

  // Update light direction
  m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(lightDirection, kShadowIntensity);

  // Update shadow map metrics
  m_shadowUniformsData.shadowMapMetrics = glm::vec4(
      1.0f / static_cast<float>(m_cascadeResolution),
      maxShadowDistance,
      0.0f,
      static_cast<float>(m_cascadeCount));

  // Upload to GPU
  std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
  vmaFlushAllocation(m_allocator, m_shadowUniformBuffer.allocation, 0, sizeof(m_shadowUniformsData));
}

}  // namespace demo
