#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"
#include "ClipSpaceConvention.h"

namespace demo {

class ShadowResources
{
public:
  struct CreateInfo
  {
    uint32_t                          shadowMapSize{2048};
    VkFormat                          shadowFormat{VK_FORMAT_D32_SFLOAT};
    clipspace::ProjectionConvention   projectionConvention{
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};
  };

  ShadowResources() = default;
  ~ShadowResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  void updateShadowMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir);

  [[nodiscard]] VkImage getShadowMapImage() const { return m_shadowMapImage.image; }
  [[nodiscard]] VkImageView getShadowMapView() const { return m_shadowMapView; }
  [[nodiscard]] VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }
  [[nodiscard]] VkDeviceAddress getShadowUniformBufferAddress() const { return m_shadowUniformBuffer.address; }
  [[nodiscard]] uint32_t getShadowMapSize() const { return m_shadowMapSize; }
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }

private:
  VkDevice                        m_device{VK_NULL_HANDLE};
  VmaAllocator                    m_allocator{nullptr};
  clipspace::ProjectionConvention m_projectionConvention{
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};

  utils::Image             m_shadowMapImage{};
  VkImageView              m_shadowMapView{VK_NULL_HANDLE};
  utils::Buffer            m_shadowUniformBuffer{};
  shaderio::ShadowUniforms m_shadowUniformsData{};
  void*                    m_shadowUniformMapped{nullptr};

  uint32_t m_shadowMapSize{2048};
};

}  // namespace demo
