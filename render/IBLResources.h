#pragma once

#include "../common/Common.h"

namespace demo {

class IBLResources
{
public:
  struct CreateInfo
  {
    uint32_t cubeMapSize{128};   // Cube map face size
    uint32_t dfgLUTSize{256};    // DFG LUT size
    VkFormat cubeMapFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
    VkFormat dfgLUTFormat{VK_FORMAT_R16G16_SFLOAT};
  };

  IBLResources() = default;
  ~IBLResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  [[nodiscard]] VkImageView getPrefilteredMapView() const { return m_prefilteredMapView; }
  [[nodiscard]] VkImageView getDFGLUTView() const { return m_dfgLUTView; }
  [[nodiscard]] VkImageView getIrradianceMapView() const { return m_irradianceMapView; }
  [[nodiscard]] VkSampler getCubeMapSampler() const { return m_cubeMapSampler; }
  [[nodiscard]] VkSampler getLUTSampler() const { return m_lutSampler; }
  [[nodiscard]] uint32_t getMaxMipLevel() const { return m_maxMipLevel; }

private:
  VkDevice m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};

  utils::Image m_prefilteredMap{};
  VkImageView m_prefilteredMapView{VK_NULL_HANDLE};

  utils::Image m_irradianceMap{};
  VkImageView m_irradianceMapView{VK_NULL_HANDLE};

  utils::Image m_dfgLUT{};
  VkImageView m_dfgLUTView{VK_NULL_HANDLE};

  VkSampler m_cubeMapSampler{VK_NULL_HANDLE};
  VkSampler m_lutSampler{VK_NULL_HANDLE};

  uint32_t m_maxMipLevel{0};
};

}  // namespace demo