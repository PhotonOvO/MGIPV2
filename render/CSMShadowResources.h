#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"
#include "ClipSpaceConvention.h"

namespace demo {

class CSMShadowResources
{
public:
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
  [[nodiscard]] VkExtent2D getCascadeExtent() const
  {
    return {m_cascadeResolution, m_cascadeResolution};
  }

  // Uniform buffer access
  [[nodiscard]] VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }

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
  void*                    m_shadowUniformMapped{nullptr};

  uint32_t m_cascadeCount{4};
  uint32_t m_cascadeResolution{1024};
};

}  // namespace demo