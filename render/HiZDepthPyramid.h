#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"

#include <vector>

namespace demo {

class HiZDepthPyramid
{
public:
  void init(VkDevice device, VmaAllocator allocator, uint32_t frameCount, VkExtent2D sourceSize);
  void shutdown();
  void resize(VkExtent2D sourceSize);
  void generate(uint32_t frameIndex,
                VkCommandBuffer cmd,
                VkExtent2D sourceSize,
                VkImage sourceDepthImage,
                VkImageView sourceDepthView,
                TextureHandle sourceDepth);
  void bindForCulling(VkDescriptorSet set, uint32_t binding);
  [[nodiscard]] uint32_t getMipCount() const { return m_mipCount; }
  [[nodiscard]] VkExtent2D getExtent() const { return m_size; }
  [[nodiscard]] VkImage getImage() const { return m_image; }
  [[nodiscard]] VkImageView getMipView(uint32_t mipLevel) const;
  [[nodiscard]] const VkImageView* getMipViewsData() const { return m_mipViews.empty() ? nullptr : m_mipViews.data(); }
  [[nodiscard]] TextureHandle getSourceDepth() const { return m_sourceDepth; }
  [[nodiscard]] bool isValid() const { return m_valid; }
  [[nodiscard]] uint64_t getGenerationCount() const { return m_generationCount; }
  [[nodiscard]] VkDescriptorSet getLastBoundSet() const { return m_lastBoundSet; }
  [[nodiscard]] uint32_t getLastBoundBinding() const { return m_lastBoundBinding; }

private:
  struct PerFrameResources
  {
    utils::Buffer   uniformBuffer{};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  };

  void updateDescriptorSet(uint32_t frameIndex, VkImageView sourceDepthView);
  void recreateResources();
  void destroyImageResources();

  VkDevice          m_device{VK_NULL_HANDLE};
  VmaAllocator      m_allocator{VK_NULL_HANDLE};
  uint32_t          m_frameCount{0};
  VkExtent2D m_size{};
  uint32_t   m_mipCount{0};
  VkImage    m_image{VK_NULL_HANDLE};
  VmaAllocation m_imageAllocation{nullptr};
  TextureHandle m_sourceDepth{};
  std::vector<VkImageView> m_mipViews;
  std::vector<PerFrameResources> m_perFrame;
  VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
  VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
  VkPipeline       m_pipeline{VK_NULL_HANDLE};
  VkDescriptorSet m_lastBoundSet{VK_NULL_HANDLE};
  uint32_t        m_lastBoundBinding{0};
  uint64_t        m_generationCount{0};
  bool            m_valid{false};
};

}  // namespace demo
