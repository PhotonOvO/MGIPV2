#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"
#include "Pass.h"

#include <array>

namespace demo {

class SceneResources
{
public:
  struct CreateInfo
  {
    VkExtent2D            size{};
    std::vector<VkFormat> color;
    VkFormat              depth{VK_FORMAT_UNDEFINED};
    VkSampler             linearSampler{VK_NULL_HANDLE};
    VkSampleCountFlagBits sampleCount{VK_SAMPLE_COUNT_1_BIT};
  };

  SceneResources() = default;
  ~SceneResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(rhi::Device& device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();
  void update(VkCommandBuffer cmd, VkExtent2D newSize);

  [[nodiscard]] ImTextureID                  getImTextureID(uint32_t i = 0) const;
  [[nodiscard]] VkExtent2D                   getSize() const;
  [[nodiscard]] VkImage                      getColorImage(uint32_t i = 0) const;
  [[nodiscard]] VkImage                      getDepthImage() const;
  [[nodiscard]] rhi::TextureViewHandle       getColorImageView(uint32_t i = 0) const;
  [[nodiscard]] const VkDescriptorImageInfo& getDescriptorImageInfo(uint32_t i = 0) const;
  [[nodiscard]] rhi::TextureViewHandle       getDepthImageView() const;
  [[nodiscard]] VkFormat                     getColorFormat(uint32_t i = 0) const;
  [[nodiscard]] VkFormat                     getDepthFormat() const;
  [[nodiscard]] VkSampleCountFlagBits        getSampleCount() const;
  [[nodiscard]] float                        getAspectRatio() const;

  // GBuffer MRT accessors (alias for existing color accessors)
  [[nodiscard]] rhi::TextureViewHandle       getGBufferImageView(uint32_t index) const { return getColorImageView(index); }
  [[nodiscard]] const VkDescriptorImageInfo& getGBufferDescriptor(uint32_t index) const { return getDescriptorImageInfo(index); }

  // Output texture for PBR lighting result (follows screen size, like Unity/UE)
  static constexpr uint32_t kOutputTextureIndex = kPackedGBufferTargetCount;  // After packed GBuffer targets

  [[nodiscard]] rhi::TextureViewHandle getOutputTextureView() const;
  [[nodiscard]] ImTextureID getOutputTextureImID() const;
  [[nodiscard]] VkImage getOutputTextureImage() const;
  [[nodiscard]] VkFormat getOutputTextureFormat() const { return kOutputTextureFormat; }
  [[nodiscard]] uint64_t getOutputTextureEstimatedBytes() const;
  [[nodiscard]] VkImage getSceneColorHdrImage() const;
  [[nodiscard]] rhi::TextureViewHandle getSceneColorHdrView() const;
  [[nodiscard]] VkFormat getSceneColorHdrFormat() const { return kSceneColorHdrFormat; }
  [[nodiscard]] uint64_t getSceneColorHdrEstimatedBytes() const;
  [[nodiscard]] VkImage getBloomHalfImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomHalfView() const;
  [[nodiscard]] VkExtent2D getBloomHalfExtent() const;
  [[nodiscard]] VkImage getBloomQuarterImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomQuarterView() const;
  [[nodiscard]] VkExtent2D getBloomQuarterExtent() const;
  [[nodiscard]] VkImage getBloomEighthImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomEighthView() const;
  [[nodiscard]] VkExtent2D getBloomEighthExtent() const;
  [[nodiscard]] VkImage getBloomSixteenthImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomSixteenthView() const;
  [[nodiscard]] VkExtent2D getBloomSixteenthExtent() const;
  [[nodiscard]] VkImage getBloomThirtySecondImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomThirtySecondView() const;
  [[nodiscard]] VkExtent2D getBloomThirtySecondExtent() const;
  [[nodiscard]] VkImage getBloomUpsampleSixteenthImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleSixteenthView() const;
  [[nodiscard]] VkExtent2D getBloomUpsampleSixteenthExtent() const;
  [[nodiscard]] VkImage getBloomUpsampleEighthImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleEighthView() const;
  [[nodiscard]] VkExtent2D getBloomUpsampleEighthExtent() const;
  [[nodiscard]] VkImage getBloomUpsampleQuarterImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleQuarterView() const;
  [[nodiscard]] VkExtent2D getBloomUpsampleQuarterExtent() const;
  [[nodiscard]] VkImage getBloomOutputImage() const;
  [[nodiscard]] rhi::TextureViewHandle getBloomOutputView() const;
  [[nodiscard]] VkExtent2D getBloomOutputExtent() const;
  [[nodiscard]] VkImage getColorGradingLutImage() const;
  [[nodiscard]] rhi::TextureViewHandle getColorGradingLutView() const;
  [[nodiscard]] VkExtent2D getColorGradingLutExtent() const;
  [[nodiscard]] uint64_t getBloomEstimatedBytes() const;
  [[nodiscard]] VkImage getVelocityImage() const;
  [[nodiscard]] rhi::TextureViewHandle getVelocityView() const;
  [[nodiscard]] VkFormat getVelocityFormat() const { return kVelocityFormat; }
  [[nodiscard]] uint64_t getVelocityEstimatedBytes() const;
  [[nodiscard]] VkImage getSceneColorHistoryImage(uint32_t index) const;
  [[nodiscard]] rhi::TextureViewHandle getSceneColorHistoryView(uint32_t index) const;
  [[nodiscard]] uint64_t getSceneColorHistoryEstimatedBytes() const;

  static constexpr VkFormat kOutputTextureFormat = VK_FORMAT_B8G8R8A8_UNORM;
  static constexpr VkFormat kSceneColorHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
  static constexpr VkFormat kColorGradingLutFormat = VK_FORMAT_R8G8B8A8_UNORM;

  static constexpr uint32_t kShadowMapSize = 2048;
  static constexpr uint32_t kColorGradingLutSize = 32;

  [[nodiscard]] VkImage getShadowMapImage() const;
  [[nodiscard]] rhi::TextureViewHandle getShadowMapView() const;
  [[nodiscard]] VkExtent2D getShadowMapExtent() const;
  [[nodiscard]] VkImage getDepthPyramidImage() const;
  [[nodiscard]] rhi::TextureViewHandle getDepthPyramidMipView(uint32_t mipLevel) const;
  [[nodiscard]] VkExtent2D getDepthPyramidExtent() const;
  [[nodiscard]] uint32_t getDepthPyramidMipCount() const;

private:
  struct Resources
  {
    std::vector<utils::Image>          colorImages;
    utils::Image                       depthImage{};
    rhi::TextureViewHandle             depthView{};
    std::vector<VkDescriptorImageInfo> descriptors;
    std::vector<rhi::TextureViewHandle> colorViews;       // RHI handles backing descriptors[].imageView
    std::vector<rhi::TextureViewHandle> uiImageViews;
    utils::Image                       outputTextureImage{};  // Fixed-res output for PBR result
    rhi::TextureViewHandle             outputTextureView{};
    ImTextureID                        outputTextureImID{};
    utils::Image                       sceneColorHdrImage{};
    rhi::TextureViewHandle             sceneColorHdrView{};
    utils::Image                       bloomHalfImage{};
    rhi::TextureViewHandle             bloomHalfView{};
    VkExtent2D                         bloomHalfExtent{};
    utils::Image                       bloomQuarterImage{};
    rhi::TextureViewHandle             bloomQuarterView{};
    VkExtent2D                         bloomQuarterExtent{};
    utils::Image                       bloomEighthImage{};
    rhi::TextureViewHandle             bloomEighthView{};
    VkExtent2D                         bloomEighthExtent{};
    utils::Image                       bloomSixteenthImage{};
    rhi::TextureViewHandle             bloomSixteenthView{};
    VkExtent2D                         bloomSixteenthExtent{};
    utils::Image                       bloomThirtySecondImage{};
    rhi::TextureViewHandle             bloomThirtySecondView{};
    VkExtent2D                         bloomThirtySecondExtent{};
    utils::Image                       bloomUpsampleSixteenthImage{};
    rhi::TextureViewHandle             bloomUpsampleSixteenthView{};
    VkExtent2D                         bloomUpsampleSixteenthExtent{};
    utils::Image                       bloomUpsampleEighthImage{};
    rhi::TextureViewHandle             bloomUpsampleEighthView{};
    VkExtent2D                         bloomUpsampleEighthExtent{};
    utils::Image                       bloomUpsampleQuarterImage{};
    rhi::TextureViewHandle             bloomUpsampleQuarterView{};
    VkExtent2D                         bloomUpsampleQuarterExtent{};
    utils::Image                       bloomOutputImage{};
    rhi::TextureViewHandle             bloomOutputView{};
    VkExtent2D                         bloomOutputExtent{};
    utils::Image                       colorGradingLutImage{};
    rhi::TextureViewHandle             colorGradingLutView{};
    VkExtent2D                         colorGradingLutExtent{};
    utils::Buffer                      colorGradingLutStaging{};
    utils::Image                       velocityImage{};
    rhi::TextureViewHandle             velocityView{};
    std::array<utils::Image, 2>        sceneColorHistoryImages{};
    std::array<rhi::TextureViewHandle, 2> sceneColorHistoryViews{};
    utils::Image                       shadowMapImage{};
    rhi::TextureViewHandle             shadowMapView{};
    utils::Image                       depthPyramidImage{};
    std::vector<rhi::TextureViewHandle> depthPyramidMipViews;
    VkExtent2D                         depthPyramidExtent{};
    uint32_t                           depthPyramidMipCount{0};
  };

  void                       create(VkCommandBuffer cmd);
  void                       destroy();
  [[nodiscard]] utils::Image createImage(const VkImageCreateInfo& imageInfo) const;

  VkDevice                 m_device{VK_NULL_HANDLE};
  rhi::Device*             m_rhiDevice{nullptr};
  VmaAllocator             m_allocator{nullptr};
  CreateInfo               m_createInfo{};
  Resources                m_resources{};
  std::vector<ImTextureID> m_imguiTextureIds;
};

}  // namespace demo
