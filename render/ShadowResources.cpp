#include "ShadowResources.h"

// GLM matrix functions (not included by default)
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <cmath>

namespace demo {

void ShadowResources::init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  m_device = device;
  m_allocator = allocator;
  m_shadowMapSize = createInfo.shadowMapSize;
  m_cascadeCount = createInfo.cascadeCount;

  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();

  // Create shadow map image as 2D array (one layer per cascade)
  const VkImageCreateInfo shadowInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = createInfo.shadowFormat,
      .extent = {m_shadowMapSize, m_shadowMapSize, 1},
      .mipLevels = 1,
      .arrayLayers = m_cascadeCount,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
  };

  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateImage(m_allocator, &shadowInfo, &imageAllocInfo,
      &m_shadowMapImage.image, &m_shadowMapImage.allocation, nullptr));
  dutil.setObjectName(m_shadowMapImage.image, "Shadow_DepthMap");

  // Create full array view
  const VkImageViewCreateInfo fullViewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_shadowMapImage.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = createInfo.shadowFormat,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = m_cascadeCount
      },
  };
  VK_CHECK(vkCreateImageView(m_device, &fullViewInfo, nullptr, &m_shadowMapView));
  dutil.setObjectName(m_shadowMapView, "Shadow_DepthMapView");

  // Create per-cascade views
  m_cascadeViews.resize(m_cascadeCount);
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    const VkImageViewCreateInfo cascadeViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_shadowMapImage.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = createInfo.shadowFormat,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = i,
            .layerCount = 1
        },
    };
    VK_CHECK(vkCreateImageView(m_device, &cascadeViewInfo, nullptr, &m_cascadeViews[i]));
    dutil.setObjectName(m_cascadeViews[i], "Shadow_CascadeView_" + std::to_string(i));
  }

  // Create shadow uniform buffer
  const VkBufferCreateInfo uniformInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(shaderio::ShadowUniforms),
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };
  VK_CHECK(vmaCreateBuffer(m_allocator, &uniformInfo, &imageAllocInfo,
      &m_shadowUniformBuffer.buffer, &m_shadowUniformBuffer.allocation, nullptr));
  dutil.setObjectName(m_shadowUniformBuffer.buffer, "Shadow_UniformBuffer");

  // Initialize shadow map image to depth attachment optimal layout
  utils::cmdInitImageLayout(cmd, m_shadowMapImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

  // Clear depth to 1.0 (far plane)
  const VkClearDepthStencilValue depthClearValue{1.0f, 0};
  const VkImageSubresourceRange depthRange{
      .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = m_cascadeCount,
  };
  vkCmdClearDepthStencilImage(cmd, m_shadowMapImage.image, VK_IMAGE_LAYOUT_GENERAL, &depthClearValue, 1, &depthRange);
}

void ShadowResources::deinit()
{
  // Clean up staging buffers
  for(utils::Buffer& staging : m_stagingBuffers)
  {
    if(staging.buffer != VK_NULL_HANDLE)
      vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
  }
  m_stagingBuffers.clear();

  // Clean up cascade views
  for(VkImageView view : m_cascadeViews)
  {
    if(view != VK_NULL_HANDLE)
      vkDestroyImageView(m_device, view, nullptr);
  }
  m_cascadeViews.clear();

  if(m_shadowMapView != VK_NULL_HANDLE)
    vkDestroyImageView(m_device, m_shadowMapView, nullptr);
  if(m_shadowMapImage.image != VK_NULL_HANDLE)
    vmaDestroyImage(m_allocator, m_shadowMapImage.image, m_shadowMapImage.allocation);
  if(m_shadowUniformBuffer.buffer != VK_NULL_HANDLE)
    vmaDestroyBuffer(m_allocator, m_shadowUniformBuffer.buffer, m_shadowUniformBuffer.allocation);

  *this = ShadowResources{};
}

void ShadowResources::updateCascadeMatrices(VkCommandBuffer cmd, const glm::mat4& cameraView, const glm::mat4& cameraProj, const glm::vec3& lightDir)
{
  // Create staging buffer for upload
  const VkBufferCreateInfo stagingInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(shaderio::ShadowUniforms),
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
  };
  const VmaAllocationCreateInfo stagingAllocInfo{.usage = VMA_MEMORY_USAGE_CPU_ONLY};

  utils::Buffer stagingBuffer{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
      &stagingBuffer.buffer, &stagingBuffer.allocation, nullptr));

  // Map and fill staging buffer
  void* mappedData = nullptr;
  vmaMapMemory(m_allocator, stagingBuffer.allocation, &mappedData);
  shaderio::ShadowUniforms* shadowUniforms = static_cast<shaderio::ShadowUniforms*>(mappedData);

  // Calculate cascade split distances using practical split scheme
  // Based on: 0.1 near, 100.0 far, logarithmic + linear blend
  const float nearPlane = 0.1f;
  const float farPlane = 100.0f;
  const float lambda = 0.5f;  // Blend factor (0 = uniform, 1 = logarithmic)

  // Calculate split depths for each cascade
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    const float p = static_cast<float>(i + 1) / static_cast<float>(m_cascadeCount);
    const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
    const float uniformSplit = nearPlane + (farPlane - nearPlane) * p;
    const float splitDepth = lambda * logSplit + (1.0f - lambda) * uniformSplit;

    shadowUniforms->cascades[i].splitDepth = glm::vec4(splitDepth, 0.0f, 0.0f, 0.0f);
  }

  // Calculate light-space view-projection matrices for each cascade
  // Light direction is assumed to be normalized
  const glm::vec3 normalizedLightDir = glm::normalize(lightDir);

  // Inverse camera matrices
  const glm::mat4 invCameraView = glm::inverse(cameraView);
  const glm::mat4 invCameraProj = glm::inverse(cameraProj);

  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    // Get cascade frustum corners in world space
    // Start with previous cascade's split depth (or near plane for first)
    const float prevSplit = (i == 0) ? nearPlane : shadowUniforms->cascades[i - 1].splitDepth.x;
    const float currSplit = shadowUniforms->cascades[i].splitDepth.x;

    // Calculate frustum corners in NDC space
    // For simplicity, use the full frustum slice
    const std::array<glm::vec4, 8> ndcCorners = {
      // Near plane corners
      glm::vec4(-1.0f, -1.0f, prevSplit * 2.0f - 1.0f, 1.0f),  // Bottom-left near
      glm::vec4( 1.0f, -1.0f, prevSplit * 2.0f - 1.0f, 1.0f),  // Bottom-right near
      glm::vec4(-1.0f,  1.0f, prevSplit * 2.0f - 1.0f, 1.0f),  // Top-left near
      glm::vec4( 1.0f,  1.0f, prevSplit * 2.0f - 1.0f, 1.0f),  // Top-right near
      // Far plane corners (at current split)
      glm::vec4(-1.0f, -1.0f, currSplit * 2.0f - 1.0f, 1.0f),  // Bottom-left far
      glm::vec4( 1.0f, -1.0f, currSplit * 2.0f - 1.0f, 1.0f),  // Bottom-right far
      glm::vec4(-1.0f,  1.0f, currSplit * 2.0f - 1.0f, 1.0f),  // Top-left far
      glm::vec4( 1.0f,  1.0f, currSplit * 2.0f - 1.0f, 1.0f),  // Top-right far
    };

    // Transform NDC corners to world space
    glm::vec3 minCorner(std::numeric_limits<float>::max());
    glm::vec3 maxCorner(std::numeric_limits<float>::lowest());

    for(const glm::vec4& ndc : ndcCorners)
    {
      glm::vec4 viewSpace = invCameraProj * ndc;
      viewSpace /= viewSpace.w;  // Perspective divide
      glm::vec3 worldSpace = glm::vec3(invCameraView * viewSpace);

      minCorner = glm::min(minCorner, worldSpace);
      maxCorner = glm::max(maxCorner, worldSpace);
    }

    // Calculate light-space matrix
    // Build light view matrix (look from light direction towards center)
    const glm::vec3 cascadeCenter = (minCorner + maxCorner) * 0.5f;
    const glm::vec3 lightPos = cascadeCenter - normalizedLightDir * 100.0f;  // Place light far enough

    // Create light view matrix using lookAt
    // Need to find up vector (perpendicular to light direction)
    glm::vec3 upVector = glm::vec3(0.0f, 1.0f, 0.0f);
    if(std::abs(glm::dot(normalizedLightDir, upVector)) > 0.99f)
    {
      upVector = glm::vec3(0.0f, 0.0f, 1.0f);  // Use Z-up if light is nearly vertical
    }
    const glm::mat4 lightView = glm::lookAt(lightPos, cascadeCenter, upVector);

    // Transform frustum corners to light space
    glm::vec3 lightSpaceMin(std::numeric_limits<float>::max());
    glm::vec3 lightSpaceMax(std::numeric_limits<float>::lowest());

    for(const glm::vec4& ndc : ndcCorners)
    {
      glm::vec4 viewSpace = invCameraProj * ndc;
      viewSpace /= viewSpace.w;
      glm::vec3 worldSpace = glm::vec3(invCameraView * viewSpace);
      glm::vec3 lightSpace = glm::vec3(lightView * glm::vec4(worldSpace, 1.0f));

      lightSpaceMin = glm::min(lightSpaceMin, lightSpace);
      lightSpaceMax = glm::max(lightSpaceMax, lightSpace);
    }

    // Build orthographic projection for light
    const glm::mat4 lightProj = glm::ortho(
        lightSpaceMin.x, lightSpaceMax.x,
        lightSpaceMin.y, lightSpaceMax.y,
        lightSpaceMin.z, lightSpaceMax.z
    );

    shadowUniforms->cascades[i].viewProjectionMatrix = lightProj * lightView;

    // Calculate texel size for filtering
    shadowUniforms->cascades[i].texelSize = 1.0f / static_cast<float>(m_shadowMapSize);
    shadowUniforms->cascades[i].cascadeIndex = i;
    shadowUniforms->cascades[i].cascadeBlendRegion = 0.1f;  // 10% blend region
  }

  // Fill shadow uniforms metadata
  shadowUniforms->lightDirection = normalizedLightDir;
  shadowUniforms->shadowIntensity = 0.5f;
  shadowUniforms->shadowBias = 0.0001f;
  shadowUniforms->normalBias = 0.01f;
  shadowUniforms->shadowMapSize = m_shadowMapSize;
  shadowUniforms->pcfKernelSize = 3;  // 3x3 PCF

  vmaUnmapMemory(m_allocator, stagingBuffer.allocation);

  // Copy to GPU buffer
  const VkBufferCopy2 copyRegion{
      .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = 0,
      .dstOffset = 0,
      .size = sizeof(shaderio::ShadowUniforms),
  };
  const VkCopyBufferInfo2 copyInfo{
      .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer = stagingBuffer.buffer,
      .dstBuffer = m_shadowUniformBuffer.buffer,
      .regionCount = 1,
      .pRegions = &copyRegion,
  };
  vkCmdCopyBuffer2(cmd, &copyInfo);

  // Add barrier to ensure copy completes before shader reads
  utils::cmdBufferMemoryBarrier(cmd, m_shadowUniformBuffer.buffer,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

  // Store staging buffer for deferred deletion
  m_stagingBuffers.push_back(stagingBuffer);
}

}  // namespace demo