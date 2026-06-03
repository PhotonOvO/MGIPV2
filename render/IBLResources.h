#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"

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
    rhi::TextureViewHandle sourceEnvironmentView{};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t sourceMipCount{1};
    uint32_t irradianceSampleCount{256};
    uint32_t prefilterSampleCount{512};
    uint32_t brdfSampleCount{1024};
  };

  IBLResources() = default;
  ~IBLResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(rhi::Device& device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  [[nodiscard]] rhi::TextureViewHandle getPrefilteredMapView() const { return m_prefilteredMapView; }
  [[nodiscard]] rhi::TextureViewHandle getDFGLUTView() const { return m_dfgLUTView; }
  [[nodiscard]] rhi::TextureViewHandle getIrradianceMapView() const { return m_irradianceMapView; }
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
  rhi::Device* m_rhiDevice{nullptr};
  VmaAllocator m_allocator{nullptr};

  utils::Image m_prefilteredMap{};
  rhi::TextureViewHandle m_prefilteredMapView{};

  utils::Image m_irradianceMap{};
  rhi::TextureViewHandle m_irradianceMapView{};
  rhi::TextureViewHandle m_irradianceStorageView{};

  utils::Image m_dfgLUT{};
  rhi::TextureViewHandle m_dfgLUTView{};

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
  std::vector<rhi::TextureViewHandle> m_generationMipViews;

  uint32_t m_maxMipLevel{0};
  uint32_t m_cubeMapSize{0};
  uint32_t m_dfgLUTSize{0};
  bool m_splitSumReady{false};
};

}  // namespace demo
