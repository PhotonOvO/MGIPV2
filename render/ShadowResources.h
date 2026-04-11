#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"

#include <vector>

namespace demo {

class ShadowResources
{
public:
  struct CreateInfo
  {
    uint32_t shadowMapSize{1024};    // Per-cascade shadow map size
    uint32_t cascadeCount{4};        // Number of CSM cascades
    VkFormat shadowFormat{VK_FORMAT_D32_SFLOAT};
  };

  ShadowResources() = default;
  ~ShadowResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  // Update cascade matrices based on camera frustum and light direction
  void updateCascadeMatrices(VkCommandBuffer cmd, const glm::mat4& cameraView, const glm::mat4& cameraProj, const glm::vec3& lightDir);

  [[nodiscard]] VkImage getShadowMapImage() const { return m_shadowMapImage.image; }
  [[nodiscard]] VkImageView getShadowMapView() const { return m_shadowMapView; }
  [[nodiscard]] VkImageView getCascadeView(uint32_t cascadeIndex) const { return m_cascadeViews[cascadeIndex]; }
  [[nodiscard]] VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }
  [[nodiscard]] VkDeviceAddress getShadowUniformBufferAddress() const { return m_shadowUniformBuffer.address; }
  [[nodiscard]] uint32_t getShadowMapSize() const { return m_shadowMapSize; }
  [[nodiscard]] uint32_t getCascadeCount() const { return m_cascadeCount; }

private:
  VkDevice m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};

  utils::Image m_shadowMapImage{};            // Shadow depth texture (array)
  VkImageView m_shadowMapView{VK_NULL_HANDLE}; // Full array view
  std::vector<VkImageView> m_cascadeViews;     // Per-cascade views

  utils::Buffer m_shadowUniformBuffer{};      // Shadow cascade matrices

  uint32_t m_shadowMapSize{1024};
  uint32_t m_cascadeCount{4};

  // Staging buffers for deferred deletion after GPU sync
  std::vector<utils::Buffer> m_stagingBuffers;
};

}  // namespace demo