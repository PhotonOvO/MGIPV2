#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"
#include "ClipSpaceConvention.h"

#include <array>

namespace demo {

class CSMShadowResources
{
public:
  struct CascadeData
  {
    float splitNear{0.0f};
    float splitFar{0.0f};
    float texelSize{0.0f};
    float receiverRadius{0.0f};
    float nearPlane{0.0f};
    float farPlane{0.0f};
    glm::vec3 receiverCenter{0.0f};
    glm::vec3 lightPosition{0.0f};
    glm::mat4 lightView{1.0f};
    glm::mat4 lightProjection{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::mat4 cullingViewProjection{1.0f};
    glm::mat4 worldToShadowTexture{1.0f};
    glm::vec3 receiverMinLightSpace{0.0f};
    glm::vec3 receiverMaxLightSpace{0.0f};
    glm::vec3 casterMinLightSpace{0.0f};
    glm::vec3 casterMaxLightSpace{0.0f};
    std::array<glm::vec3, 8> receiverCornersWorld{};
    std::array<glm::vec4, shaderio::LGPUCullingFrustumPlaneCount> cullingPlanes{};
  };

  struct FrameData
  {
    std::array<CascadeData, shaderio::LCascadeCount> cascades{};
    uint32_t cascadeCount{0};
    glm::vec4 splitDistances{0.0f};
    glm::vec3 lightDirection{0.0f, -1.0f, 0.0f};
    float maxShadowDistance{0.0f};
    glm::vec3 casterBoundsMin{0.0f};
    glm::vec3 casterBoundsMax{0.0f};
    bool casterBoundsValid{false};
  };

  struct CreateInfo
  {
    uint32_t                          cascadeCount{4};
    uint32_t                          cascadeResolution{1024};  // Per cascade
    VkFormat                          shadowFormat{VK_FORMAT_D32_SFLOAT};
    clipspace::ProjectionConvention   projectionConvention{
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};
  };

  CSMShadowResources() = default;
  ~CSMShadowResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  void updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir);
  void updateCascadeMatrices(const shaderio::CameraUniforms& camera,
                             const glm::vec3& lightDir,
                             float maxShadowDistance);
  void updateCascadeMatrices(const shaderio::CameraUniforms& camera,
                             const glm::vec3& lightDir,
                             float maxShadowDistance,
                             const glm::vec3& casterBoundsMin,
                             const glm::vec3& casterBoundsMax,
                             bool casterBoundsValid);

  // Texture2DArray access (all cascades)
  [[nodiscard]] VkImage getCascadeImage() const { return m_cascadeArray.image; }
  [[nodiscard]] VkImageView getCascadeView() const { return m_cascadeArrayView; }

  // Per-layer access (for rendering each cascade)
  [[nodiscard]] VkImageView getCascadeLayerView(uint32_t index) const
  {
    assert(index < m_cascadeCount);
    return m_cascadeLayerViews[index];
  }

  [[nodiscard]] uint32_t getCascadeCount() const { return m_cascadeCount; }
  [[nodiscard]] uint32_t getCascadeResolution() const { return m_cascadeResolution; }
  [[nodiscard]] VkFormat getShadowFormat() const { return m_shadowFormat; }
  [[nodiscard]] VkExtent2D getCascadeExtent() const
  {
    return {m_cascadeResolution, m_cascadeResolution};
  }

  // Uniform buffer access
  [[nodiscard]] VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }
  [[nodiscard]] const shaderio::ShadowUniforms* getShadowUniformsData() const { return &m_shadowUniformsData; }
  [[nodiscard]] const FrameData& getFrameData() const { return m_frameData; }
  [[nodiscard]] const CascadeData& getCascadeData(uint32_t cascadeIndex) const
  {
    assert(cascadeIndex < m_cascadeCount);
    return m_frameData.cascades[cascadeIndex];
  }

private:
  VkDevice                        m_device{VK_NULL_HANDLE};
  VmaAllocator                    m_allocator{nullptr};
  clipspace::ProjectionConvention m_projectionConvention{
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};

  utils::Image             m_cascadeArray{};  // Texture2DArray (arrayLayers = cascadeCount)
  VkImageView              m_cascadeArrayView{VK_NULL_HANDLE};  // Full array view for sampling
  VkImageView              m_cascadeLayerViews[shaderio::LCascadeCount];  // Per-layer views for rendering

  utils::Buffer            m_shadowUniformBuffer{};
  shaderio::ShadowUniforms m_shadowUniformsData{};
  FrameData                m_frameData{};
  void*                    m_shadowUniformMapped{nullptr};

  uint32_t m_cascadeCount{4};
  uint32_t m_cascadeResolution{1024};
  VkFormat m_shadowFormat{VK_FORMAT_D32_SFLOAT};
};

}  // namespace demo
