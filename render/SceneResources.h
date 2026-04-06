#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"

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
  [[nodiscard]] VkImageView                  getColorImageView(uint32_t i = 0) const;
  [[nodiscard]] const VkDescriptorImageInfo& getDescriptorImageInfo(uint32_t i = 0) const;
  [[nodiscard]] VkImageView                  getDepthImageView() const;
  [[nodiscard]] VkFormat                     getColorFormat(uint32_t i = 0) const;
  [[nodiscard]] VkFormat                     getDepthFormat() const;
  [[nodiscard]] VkSampleCountFlagBits        getSampleCount() const;
  [[nodiscard]] float                        getAspectRatio() const;

  // GBuffer MRT accessors (alias for existing color accessors)
  [[nodiscard]] VkImageView                  getGBufferImageView(uint32_t index) const { return getColorImageView(index); }
  [[nodiscard]] const VkDescriptorImageInfo& getGBufferDescriptor(uint32_t index) const { return getDescriptorImageInfo(index); }

  // Fixed resolution output texture (for PBR lighting result)
  static constexpr uint32_t kOutputTextureWidth = 1920;
  static constexpr uint32_t kOutputTextureHeight = 1080;
  static constexpr uint32_t kOutputTextureIndex = 3;  // After GBuffer[0-2]

  [[nodiscard]] VkImageView getOutputTextureView() const;
  [[nodiscard]] ImTextureID getOutputTextureImID() const;
  [[nodiscard]] VkImage getOutputTextureImage() const;

private:
  struct Resources
  {
    std::vector<utils::Image>          colorImages;
    utils::Image                       depthImage{};
    VkImageView                        depthView{};
    std::vector<VkDescriptorImageInfo> descriptors;
    std::vector<VkImageView>           uiImageViews;
    utils::Image                       outputTextureImage{};  // Fixed-res output for PBR result
    VkImageView                        outputTextureView{};
    ImTextureID                        outputTextureImID{};
  };

  void                       create(VkCommandBuffer cmd);
  void                       destroy();
  [[nodiscard]] utils::Image createImage(const VkImageCreateInfo& imageInfo) const;

  VkDevice                 m_device{VK_NULL_HANDLE};
  VmaAllocator             m_allocator{nullptr};
  CreateInfo               m_createInfo{};
  Resources                m_resources{};
  std::vector<ImTextureID> m_imguiTextureIds;
};

}  // namespace demo
