#pragma once

#include "../rhi/RHIDevice.h"
#include "ShaderInterop.h"

namespace demo {

namespace rhi {
class CommandBuffer;
}

class IBLResources
{
public:
  struct CreateInfo
  {
    uint32_t cubeMapSize{128};   // Cube map face size
    uint32_t dfgLUTSize{256};    // DFG LUT size
    rhi::TextureFormat cubeMapFormat{rhi::TextureFormat::rgba16Sfloat};
    rhi::TextureFormat dfgLUTFormat{rhi::TextureFormat::rg16Sfloat};
    rhi::TextureViewHandle sourceEnvironmentView{};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t sourceMipCount{1};
    uint32_t irradianceSampleCount{256};
    uint32_t prefilterSampleCount{512};
    uint32_t brdfSampleCount{1024};
  };

  IBLResources() = default;
  ~IBLResources() { assert(m_backendDeviceToken == 0 && "Missing deinit()"); }

  void init(rhi::Device& device, uintptr_t backendAllocatorToken, rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo);
  void deinit();

  [[nodiscard]] rhi::TextureViewHandle getPrefilteredMapView() const { return m_prefilteredMapView; }
  [[nodiscard]] rhi::TextureViewHandle getDFGLUTView() const { return m_dfgLUTView; }
  [[nodiscard]] rhi::TextureViewHandle getIrradianceMapView() const { return m_irradianceMapView; }
  [[nodiscard]] rhi::SamplerHandle getCubeMapSampler() const { return m_cubeMapSampler; }
  [[nodiscard]] rhi::SamplerHandle getLUTSampler() const { return m_lutSampler; }
  [[nodiscard]] uint32_t getMaxMipLevel() const { return m_maxMipLevel; }
  [[nodiscard]] bool isInitialized() const { return m_backendDeviceToken != 0; }
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

  struct NativeImageResource
  {
    uintptr_t image{0};
    uintptr_t allocation{0};
    rhi::TextureHandle handle{};
  };

  void createImages(rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo);
  [[nodiscard]] rhi::PipelineHandle createGenerationPipeline(const uint32_t* spirvCode,
                                                             size_t spirvSize,
                                                             const char* entryPoint,
                                                             rhi::ArgumentLayoutHandle layout,
                                                             uint32_t variant) const;
  void generateIBLMaps(rhi::CommandBuffer& rhiCmd, const CreateInfo& createInfo);
  void transitionGeneratedImagesForSampling(rhi::CommandBuffer& rhiCmd) const;

  uintptr_t m_backendDeviceToken{0};
  rhi::Device* m_rhiDevice{nullptr};
  uintptr_t m_backendAllocatorToken{0};

  NativeImageResource m_prefilteredMap{};
  rhi::TextureViewHandle m_prefilteredMapView{};

  NativeImageResource m_irradianceMap{};
  rhi::TextureViewHandle m_irradianceMapView{};
  rhi::TextureViewHandle m_irradianceStorageView{};

  NativeImageResource m_dfgLUT{};
  rhi::TextureViewHandle m_dfgLUTView{};

  rhi::SamplerHandle m_cubeMapSampler{};
  rhi::SamplerHandle m_lutSampler{};

  rhi::ArgumentLayoutHandle m_envGenerationArgumentLayout{};
  rhi::ArgumentLayoutHandle m_lutGenerationArgumentLayout{};
  std::vector<rhi::ArgumentTableHandle> m_generationArgumentTables;
  rhi::PipelineHandle m_dfgGenerationPipeline{};
  rhi::PipelineHandle m_irradianceGenerationPipeline{};
  rhi::PipelineHandle m_prefilterGenerationPipeline{};
  std::vector<rhi::TextureViewHandle> m_generationMipViews;

  uint32_t m_maxMipLevel{0};
  uint32_t m_cubeMapSize{0};
  uint32_t m_dfgLUTSize{0};
  bool m_splitSumReady{false};
};

}  // namespace demo
