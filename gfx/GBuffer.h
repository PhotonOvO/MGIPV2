#pragma once

#include "../common/Common.h"
#include "ResourceAllocator.h"

namespace utils {

struct GbufferCreateInfo
{
  VkDevice                  device{};  // Vulkan Device
  utils::ResourceAllocator* alloc{};   // Allocator for the images
  VkExtent2D                size{};    // Width and height of the buffers
  std::vector<VkFormat>     color;     // Array of formats for each color attachment (as many GBuffers as formats)
  VkFormat              depth{VK_FORMAT_UNDEFINED};  // Format of the depth buffer (VK_FORMAT_UNDEFINED for no depth)
  VkSampler             linearSampler{};             // Linear sampler for displaying the images
  VkSampleCountFlagBits sampleCount{VK_SAMPLE_COUNT_1_BIT};  // MSAA sample count (default: no MSAA)
};

/*--
 * GBuffer - Multiple render targets with depth management
 * 
 * This class manages multiple color buffers and a depth buffer for deferred rendering or 
 * other multi-target rendering techniques. It supports:
 * - Multiple color attachments with configurable formats
 * - Optional depth buffer
 * - MSAA support
 * - ImGui integration for debug visualization
 * - Automatic resource cleanup
 *
 * The GBuffer images can be used as:
 * - Color/Depth attachments (write)
 * - Texture sampling (read)
 * - Storage images (read/write)
 * - Transfer operations
-*/
class Gbuffer
{
public:
  Gbuffer() = default;
  ~Gbuffer() { assert(m_createInfo.device == VK_NULL_HANDLE && "Missing deinit()"); }

  /*--
   * Initialize the GBuffer with the specified configuration.
  -*/
  void init(VkCommandBuffer cmd, const GbufferCreateInfo& createInfo)
  {
    ASSERT(m_createInfo.color.empty(), "Missing deinit()");  // The buffer must be cleared before creating a new one
    m_createInfo = createInfo;                               // Copy the creation info
    create(cmd);
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    destroy();
    *this = {};
  }

  void update(VkCommandBuffer cmd, VkExtent2D newSize)
  {
    if(newSize.width == m_createInfo.size.width && newSize.height == m_createInfo.size.height)
      return;

    destroy();
    m_createInfo.size = newSize;
    create(cmd);
  }


  //--- Getters for the GBuffer resources -------------------------
  ImTextureID getImTextureID(uint32_t i = 0) const { return reinterpret_cast<ImTextureID>(m_descriptorSet[i]); }
  VkExtent2D  getSize() const { return m_createInfo.size; }
  VkImage     getColorImage(uint32_t i = 0) const { return m_res.gBufferColor[i].image; }
  VkImage     getDepthImage() const { return m_res.gBufferDepth.image; }
  VkImageView getColorImageView(uint32_t i = 0) const { return m_res.descriptor[i].imageView; }
  const VkDescriptorImageInfo& getDescriptorImageInfo(uint32_t i = 0) const { return m_res.descriptor[i]; }
  VkImageView                  getDepthImageView() const { return m_res.depthView; }
  VkFormat                     getColorFormat(uint32_t i = 0) const { return m_createInfo.color[i]; }
  VkFormat                     getDepthFormat() const { return m_createInfo.depth; }
  VkSampleCountFlagBits        getSampleCount() const { return m_createInfo.sampleCount; }
  float getAspectRatio() const { return float(m_createInfo.size.width) / float(m_createInfo.size.height); }

private:
  /*--
   * Create the GBuffer with the specified configuration
   *
   * Each color buffer is created with:
   * - Color attachment usage     : For rendering
   * - Sampled bit                : For sampling in shaders
   * - Storage bit                : For compute shader access
   * - Transfer dst bit           : For clearing/copying
   * 
   * The depth buffer is created with:
   * - Depth/Stencil attachment   : For depth testing
   * - Sampled bit                : For sampling in shaders
   *
   * All images are transitioned to GENERAL layout and cleared to black.
   * ImGui descriptors are created for debug visualization.
  -*/
  void create(VkCommandBuffer cmd)
  {
    DebugUtil&          dutil = DebugUtil::getInstance();
    const VkImageLayout layout{VK_IMAGE_LAYOUT_GENERAL};

    const auto numColor = static_cast<uint32_t>(m_createInfo.color.size());

    m_res.gBufferColor.resize(numColor);
    m_res.descriptor.resize(numColor);
    m_res.uiImageViews.resize(numColor);
    m_descriptorSet.resize(numColor);

    for(uint32_t c = 0; c < numColor; c++)
    {
      {  // Color image
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkImageCreateInfo info = {
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = m_createInfo.color[c],
            .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
            .mipLevels   = 1,
            .arrayLayers = 1,
            .samples     = m_createInfo.sampleCount,
            .usage       = usage,
        };
        m_res.gBufferColor[c] = m_createInfo.alloc->createImage(info);
        dutil.setObjectName(m_res.gBufferColor[c].image, "G-Color" + std::to_string(c));
      }
      {  // Image color view
        VkImageViewCreateInfo info = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = m_res.gBufferColor[c].image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = m_createInfo.color[c],
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        vkCreateImageView(m_createInfo.device, &info, nullptr, &m_res.descriptor[c].imageView);
        dutil.setObjectName(m_res.descriptor[c].imageView, "G-Color" + std::to_string(c));

        // UI Image color view
        info.components.a = VK_COMPONENT_SWIZZLE_ONE;  // Forcing the VIEW to have a 1 in the alpha channel
        vkCreateImageView(m_createInfo.device, &info, nullptr, &m_res.uiImageViews[c]);
        dutil.setObjectName(m_res.uiImageViews[c], "UI G-Color" + std::to_string(c));
      }

      // Set the sampler for the color attachment
      m_res.descriptor[c].sampler = m_createInfo.linearSampler;
    }

    if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
    {  // Depth buffer
      const VkImageCreateInfo createInfo = {
          .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType   = VK_IMAGE_TYPE_2D,
          .format      = m_createInfo.depth,
          .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
          .mipLevels   = 1,
          .arrayLayers = 1,
          .samples     = m_createInfo.sampleCount,
          .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      };
      m_res.gBufferDepth = m_createInfo.alloc->createImage(createInfo);
      dutil.setObjectName(m_res.gBufferDepth.image, "G-Depth");

      // Image depth view
      const VkImageViewCreateInfo viewInfo = {
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = m_res.gBufferDepth.image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = m_createInfo.depth,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1},
      };
      vkCreateImageView(m_createInfo.device, &viewInfo, nullptr, &m_res.depthView);
      dutil.setObjectName(m_res.depthView, "G-Depth");
    }

    {  // Change color image layout
      for(uint32_t c = 0; c < numColor; c++)
      {
        cmdInitImageLayout(cmd, m_res.gBufferColor[c].image);
        m_res.descriptor[c].imageLayout = layout;

        // Clear to avoid garbage data
        const VkClearColorValue                      clearValue = {{0.F, 0.F, 0.F, 0.F}};
        const std::array<VkImageSubresourceRange, 1> range      = {
            {{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}}};
        vkCmdClearColorImage(cmd, m_res.gBufferColor[c].image, layout, &clearValue, uint32_t(range.size()), range.data());
      }

      // Change depth image layout
      if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
      {
        cmdInitImageLayout(cmd, m_res.gBufferDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT);
      }
    }

    // Descriptor Set for ImGUI
    if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
    {
      for(size_t d = 0; d < m_res.descriptor.size(); ++d)
      {
        m_descriptorSet[d] = ImGui_ImplVulkan_AddTexture(m_createInfo.linearSampler, m_res.uiImageViews[d], layout);
      }
    }
  }

  /*--
   * Clean up all Vulkan resources
   * - Images and image views
   * - Samplers
   * - ImGui descriptors
   * 
   * This must be called before destroying the GBuffer or when
   * recreating with different parameters
  -*/
  void destroy()
  {
    if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
    {
      for(VkDescriptorSet set : m_descriptorSet)
      {
        ImGui_ImplVulkan_RemoveTexture(set);
      }
      m_descriptorSet.clear();
    }

    for(utils::Image bc : m_res.gBufferColor)
    {
      m_createInfo.alloc->destroyImage(bc);
    }

    if(m_res.gBufferDepth.image != VK_NULL_HANDLE)
    {
      m_createInfo.alloc->destroyImage(m_res.gBufferDepth);
    }

    vkDestroyImageView(m_createInfo.device, m_res.depthView, nullptr);

    for(const VkDescriptorImageInfo& desc : m_res.descriptor)
    {
      vkDestroyImageView(m_createInfo.device, desc.imageView, nullptr);
    }

    for(const VkImageView& view : m_res.uiImageViews)
    {
      vkDestroyImageView(m_createInfo.device, view, nullptr);
    }
  }


  /*--
   * Resources holds all Vulkan objects for the GBuffer
   * This separation makes it easier to cleanup and recreate resources
  -*/
  struct Resources
  {
    std::vector<utils::Image>          gBufferColor;    // Color attachments
    utils::Image                       gBufferDepth{};  // Optional depth attachment
    VkImageView                        depthView{};     // View for the depth attachment
    std::vector<VkDescriptorImageInfo> descriptor;      // Descriptor info for each color attachment
    std::vector<VkImageView>           uiImageViews;    // Special views for ImGui (alpha=1)
  };

  Resources m_res;  // All Vulkan resources

  GbufferCreateInfo            m_createInfo{};   // Configuration
  std::vector<VkDescriptorSet> m_descriptorSet;  // ImGui descriptor sets
};

}
