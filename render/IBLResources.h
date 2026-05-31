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
    VkImageView sourceEnvironmentView{VK_NULL_HANDLE};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t sourceMipCount{1};
    uint32_t irradianceSampleCount{256};
    uint32_t prefilterSampleCount{512};
    uint32_t brdfSampleCount{1024};
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
  [[nodiscard]] bool isInitialized() const { return m_device != VK_NULL_HANDLE; }
  [[nodiscard]] bool isSplitSumReady() const { return m_splitSumReady; }

private:
  struct GeneratePushConstants
  {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t sourceMaxMip{0};
    uint32_t sampleCount{0};
    float roughness{0.0f};
    uint32_t reserved{0};
  };

  void createImages(VkCommandBuffer cmd, const CreateInfo& createInfo);
  void createGenerationPipeline(VkShaderModule shaderModule,
                                const char* entryPoint,
                                VkPipelineLayout pipelineLayout,
                                VkPipeline& pipeline) const;
  void generateIBLMaps(VkCommandBuffer cmd, const CreateInfo& createInfo);
  void transitionGeneratedImagesForSampling(VkCommandBuffer cmd) const;

  VkDevice m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};

  utils::Image m_prefilteredMap{};
  VkImageView m_prefilteredMapView{VK_NULL_HANDLE};

  utils::Image m_irradianceMap{};
  VkImageView m_irradianceMapView{VK_NULL_HANDLE};
  VkImageView m_irradianceStorageView{VK_NULL_HANDLE};

  utils::Image m_dfgLUT{};
  VkImageView m_dfgLUTView{VK_NULL_HANDLE};

  VkSampler m_cubeMapSampler{VK_NULL_HANDLE};
  VkSampler m_lutSampler{VK_NULL_HANDLE};

  VkDescriptorPool m_generationDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_envGenerationSetLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_lutGenerationSetLayout{VK_NULL_HANDLE};
  VkPipelineLayout m_envGenerationPipelineLayout{VK_NULL_HANDLE};
  VkPipelineLayout m_lutGenerationPipelineLayout{VK_NULL_HANDLE};
  VkPipeline m_dfgGenerationPipeline{VK_NULL_HANDLE};
  VkPipeline m_irradianceGenerationPipeline{VK_NULL_HANDLE};
  VkPipeline m_prefilterGenerationPipeline{VK_NULL_HANDLE};
  std::vector<VkImageView> m_generationMipViews;

  uint32_t m_maxMipLevel{0};
  uint32_t m_cubeMapSize{0};
  uint32_t m_dfgLUTSize{0};
  bool m_splitSumReady{false};
};

}  // namespace demo
