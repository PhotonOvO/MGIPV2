#include "SceneResources.h"

namespace demo {

void SceneResources::init(rhi::Device& device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  ASSERT(m_createInfo.color.empty(), "Missing deinit()");
  m_device     = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getNativeDevice()));
  m_allocator  = allocator;
  m_createInfo = createInfo;
  create(cmd);
}

void SceneResources::deinit()
{
  destroy();
  *this = SceneResources{};
}

void SceneResources::update(VkCommandBuffer cmd, VkExtent2D newSize)
{
  if(newSize.width == m_createInfo.size.width && newSize.height == m_createInfo.size.height)
  {
    return;
  }

  destroy();
  m_createInfo.size = newSize;
  create(cmd);
}

ImTextureID SceneResources::getImTextureID(uint32_t i) const
{
  return m_imguiTextureIds[i];
}

VkExtent2D SceneResources::getSize() const
{
  return m_createInfo.size;
}

VkImage SceneResources::getColorImage(uint32_t i) const
{
  return m_resources.colorImages[i].image;
}

VkImage SceneResources::getDepthImage() const
{
  return m_resources.depthImage.image;
}

VkImageView SceneResources::getColorImageView(uint32_t i) const
{
  return m_resources.descriptors[i].imageView;
}

const VkDescriptorImageInfo& SceneResources::getDescriptorImageInfo(uint32_t i) const
{
  return m_resources.descriptors[i];
}

VkImageView SceneResources::getDepthImageView() const
{
  return m_resources.depthView;
}

VkFormat SceneResources::getColorFormat(uint32_t i) const
{
  return m_createInfo.color[i];
}

VkFormat SceneResources::getDepthFormat() const
{
  return m_createInfo.depth;
}

VkSampleCountFlagBits SceneResources::getSampleCount() const
{
  return m_createInfo.sampleCount;
}

float SceneResources::getAspectRatio() const
{
  return float(m_createInfo.size.width) / float(m_createInfo.size.height);
}

void SceneResources::create(VkCommandBuffer cmd)
{
  utils::DebugUtil&   dutil    = utils::DebugUtil::getInstance();
  const VkImageLayout layout   = VK_IMAGE_LAYOUT_GENERAL;
  const auto          numColor = static_cast<uint32_t>(m_createInfo.color.size());

  m_resources.colorImages.resize(numColor);
  m_resources.descriptors.resize(numColor);
  m_resources.uiImageViews.resize(numColor);
  m_imguiTextureIds.resize(numColor);

  for(uint32_t c = 0; c < numColor; ++c)
  {
    const VkImageCreateInfo imageInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = m_createInfo.color[c],
        .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = m_createInfo.sampleCount,
        .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    m_resources.colorImages[c] = createImage(imageInfo);
    dutil.setObjectName(m_resources.colorImages[c].image, "SceneColor" + std::to_string(c));

    VkImageViewCreateInfo viewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.colorImages[c].image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = m_createInfo.color[c],
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_resources.descriptors[c].imageView));
    dutil.setObjectName(m_resources.descriptors[c].imageView, "SceneColorView" + std::to_string(c));

    viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_resources.uiImageViews[c]));
    dutil.setObjectName(m_resources.uiImageViews[c], "SceneColorUIView" + std::to_string(c));

    m_resources.descriptors[c].sampler     = m_createInfo.linearSampler;
    m_resources.descriptors[c].imageLayout = layout;
  }

  // Create output texture (follows screen size, like Unity/UE)
  {
    const VkImageCreateInfo outputInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = VK_FORMAT_B8G8R8A8_UNORM,
        .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    m_resources.outputTextureImage = createImage(outputInfo);
    dutil.setObjectName(m_resources.outputTextureImage.image, "OutputTexture");

    const VkImageViewCreateInfo outputViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.outputTextureImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &outputViewInfo, nullptr, &m_resources.outputTextureView));
    dutil.setObjectName(m_resources.outputTextureView, "OutputTextureView");
  }

  if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
  {
    const VkImageCreateInfo depthInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = m_createInfo.depth,
        .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = m_createInfo.sampleCount,
        .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    m_resources.depthImage = createImage(depthInfo);
    dutil.setObjectName(m_resources.depthImage.image, "SceneDepth");

    const VkImageViewCreateInfo depthViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.depthImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = m_createInfo.depth,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &depthViewInfo, nullptr, &m_resources.depthView));
    dutil.setObjectName(m_resources.depthView, "SceneDepthView");
  }

  for(uint32_t c = 0; c < numColor; ++c)
  {
    utils::cmdInitImageLayout(cmd, m_resources.colorImages[c].image);
    const VkClearColorValue                      clearValue = {{0.F, 0.F, 0.F, 0.F}};
    const std::array<VkImageSubresourceRange, 1> range      = {
        {{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}}};
    vkCmdClearColorImage(cmd, m_resources.colorImages[c].image, layout, &clearValue, uint32_t(range.size()), range.data());
  }

  // Initialize output texture layout
  utils::cmdInitImageLayout(cmd, m_resources.outputTextureImage.image);
  const VkClearColorValue outputClearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
  const VkImageSubresourceRange outputRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
  vkCmdClearColorImage(cmd, m_resources.outputTextureImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);

  if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
  {
    utils::cmdInitImageLayout(cmd, m_resources.depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
  }

  if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
  {
    for(size_t d = 0; d < m_resources.descriptors.size(); ++d)
    {
      m_imguiTextureIds[d] = reinterpret_cast<ImTextureID>(
          ImGui_ImplVulkan_AddTexture(m_createInfo.linearSampler, m_resources.uiImageViews[d], layout));
    }
  }

  // Create ImGui descriptor for output texture
  if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
  {
    m_resources.outputTextureImID = reinterpret_cast<ImTextureID>(
        ImGui_ImplVulkan_AddTexture(m_createInfo.linearSampler, m_resources.outputTextureView,
                                    VK_IMAGE_LAYOUT_GENERAL));
  }
}

void SceneResources::destroy()
{
  // Cleanup output texture
  if(m_resources.outputTextureImID && ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendPlatformUserData != nullptr)
  {
    using ImGuiTextureHandle = decltype(ImGui_ImplVulkan_AddTexture(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL));
    ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<ImGuiTextureHandle>(m_resources.outputTextureImID));
  }
  m_resources.outputTextureImID = {};
  if(m_resources.outputTextureImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.outputTextureImage.image, m_resources.outputTextureImage.allocation);
    m_resources.outputTextureImage = {};
  }
  if(m_resources.outputTextureView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.outputTextureView, nullptr);
    m_resources.outputTextureView = VK_NULL_HANDLE;
  }

  if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
  {
    using ImGuiTextureHandle = decltype(ImGui_ImplVulkan_AddTexture(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL));
    for(ImTextureID textureId : m_imguiTextureIds)
    {
      ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<ImGuiTextureHandle>(textureId));
    }
  }
  m_imguiTextureIds.clear();

  for(const utils::Image& image : m_resources.colorImages)
  {
    if(image.image != VK_NULL_HANDLE)
    {
      vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
  }

  if(m_resources.depthImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.depthImage.image, m_resources.depthImage.allocation);
  }

  if(m_resources.depthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.depthView, nullptr);
  }

  for(const VkDescriptorImageInfo& descriptor : m_resources.descriptors)
  {
    if(descriptor.imageView != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, descriptor.imageView, nullptr);
    }
  }

  for(const VkImageView view : m_resources.uiImageViews)
  {
    if(view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, view, nullptr);
    }
  }

  m_resources = {};
}

utils::Image SceneResources::createImage(const VkImageCreateInfo& imageInfo) const
{
  const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};

  utils::Image      image{};
  VmaAllocationInfo allocInfo{};
  VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &image.image, &image.allocation, &allocInfo));
  return image;
}

VkImageView SceneResources::getOutputTextureView() const
{
  return m_resources.outputTextureView;
}

ImTextureID SceneResources::getOutputTextureImID() const
{
  return m_resources.outputTextureImID;
}

VkImage SceneResources::getOutputTextureImage() const
{
  return m_resources.outputTextureImage.image;
}

}  // namespace demo
