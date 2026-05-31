#include "SceneResources.h"
#include "BatchUploadContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace demo {

namespace {

uint32_t computeMipCount(VkExtent2D extent)
{
  uint32_t mipCount = 1;
  uint32_t size = std::max(extent.width, extent.height);
  while(size > 1)
  {
    size /= 2u;
    ++mipCount;
  }
  return mipCount;
}

std::vector<uint8_t> generateBuiltInColorGradingLut()
{
  constexpr uint32_t kLutSize = SceneResources::kColorGradingLutSize;
  constexpr float    kInvMax  = 1.0f / static_cast<float>(kLutSize - 1u);
  std::vector<uint8_t> pixels(static_cast<size_t>(kLutSize) * kLutSize * kLutSize * 4u);

  const auto saturate = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
  const auto smoothstep = [](float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1.0e-5f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  const auto toByte = [&](float value) {
    return static_cast<uint8_t>(std::round(saturate(value) * 255.0f));
  };

  for(uint32_t b = 0; b < kLutSize; ++b)
  {
    for(uint32_t g = 0; g < kLutSize; ++g)
    {
      for(uint32_t r = 0; r < kLutSize; ++r)
      {
        float rr = static_cast<float>(r) * kInvMax;
        float gg = static_cast<float>(g) * kInvMax;
        float bb = static_cast<float>(b) * kInvMax;
        const float lum = rr * 0.2126f + gg * 0.7152f + bb * 0.0722f;

        const float shadowWeight = 1.0f - smoothstep(0.15f, 0.58f, lum);
        const float midWeight = 1.0f - std::abs(lum - 0.5f) * 2.0f;
        const float highlightWeight = smoothstep(0.38f, 1.0f, lum);

        rr = (rr - 0.5f) * 1.08f + 0.5f;
        gg = (gg - 0.5f) * 1.05f + 0.5f;
        bb = (bb - 0.5f) * 1.08f + 0.5f;

        rr += highlightWeight * 0.055f + midWeight * 0.018f;
        gg += highlightWeight * 0.018f + shadowWeight * 0.025f;
        bb += shadowWeight * 0.075f - highlightWeight * 0.035f;

        const float warmPush = highlightWeight * 0.045f;
        const float tealPush = shadowWeight * 0.055f;
        rr = rr + warmPush - tealPush * 0.015f;
        gg = gg + tealPush * 0.025f;
        bb = bb + tealPush - warmPush * 0.020f;

        const size_t index = (static_cast<size_t>(b) * kLutSize * kLutSize
                              + static_cast<size_t>(g) * kLutSize
                              + static_cast<size_t>(r)) * 4u;
        pixels[index + 0u] = toByte(rr);
        pixels[index + 1u] = toByte(gg);
        pixels[index + 2u] = toByte(bb);
        pixels[index + 3u] = 255u;
      }
    }
  }

  return pixels;
}

}  // namespace

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
        .format      = kOutputTextureFormat,
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
        .format           = kOutputTextureFormat,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &outputViewInfo, nullptr, &m_resources.outputTextureView));
    dutil.setObjectName(m_resources.outputTextureView, "OutputTextureView");
  }

  // Create HDR scene color and mobile bloom targets for the GPU-driven post chain.
  {
    const VkImageCreateInfo hdrInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = kSceneColorHdrFormat,
        .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    m_resources.sceneColorHdrImage = createImage(hdrInfo);
    dutil.setObjectName(m_resources.sceneColorHdrImage.image, "GPUDrivenSceneColorHDR");

    const VkImageViewCreateInfo hdrViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.sceneColorHdrImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = kSceneColorHdrFormat,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &hdrViewInfo, nullptr, &m_resources.sceneColorHdrView));
    dutil.setObjectName(m_resources.sceneColorHdrView, "GPUDrivenSceneColorHDRView");

    m_resources.bloomHalfExtent = {
        std::max(1u, (m_createInfo.size.width + 1u) / 2u),
        std::max(1u, (m_createInfo.size.height + 1u) / 2u),
    };
    m_resources.bloomQuarterExtent = {
        std::max(1u, (m_createInfo.size.width + 3u) / 4u),
        std::max(1u, (m_createInfo.size.height + 3u) / 4u),
    };
    const auto downsampledExtent = [](VkExtent2D baseExtent, uint32_t divisor) {
      return VkExtent2D{
          std::max(1u, (baseExtent.width + divisor - 1u) / divisor),
          std::max(1u, (baseExtent.height + divisor - 1u) / divisor),
      };
    };
    m_resources.bloomEighthExtent = downsampledExtent(m_createInfo.size, 8u);
    m_resources.bloomSixteenthExtent = downsampledExtent(m_createInfo.size, 16u);
    m_resources.bloomThirtySecondExtent = downsampledExtent(m_createInfo.size, 32u);
    m_resources.bloomUpsampleSixteenthExtent = m_resources.bloomSixteenthExtent;
    m_resources.bloomUpsampleEighthExtent = m_resources.bloomEighthExtent;
    m_resources.bloomUpsampleQuarterExtent = m_resources.bloomQuarterExtent;
    m_resources.bloomOutputExtent = m_resources.bloomHalfExtent;

    const auto createBloomTarget = [&](VkExtent2D extent, const char* imageName, const char* viewName,
                                       utils::Image& image, VkImageView& view) {
      const VkImageCreateInfo bloomInfo{
          .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType   = VK_IMAGE_TYPE_2D,
          .format      = kBloomFormat,
          .extent      = {extent.width, extent.height, 1},
          .mipLevels   = 1,
          .arrayLayers = 1,
          .samples     = VK_SAMPLE_COUNT_1_BIT,
          .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      image = createImage(bloomInfo);
      dutil.setObjectName(image.image, imageName);

      const VkImageViewCreateInfo bloomViewInfo{
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = image.image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = kBloomFormat,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
      };
      VK_CHECK(vkCreateImageView(m_device, &bloomViewInfo, nullptr, &view));
      dutil.setObjectName(view, viewName);
    };

    createBloomTarget(m_resources.bloomHalfExtent,
                      "GPUDrivenBloomHalf",
                      "GPUDrivenBloomHalfView",
                      m_resources.bloomHalfImage,
                      m_resources.bloomHalfView);
    createBloomTarget(m_resources.bloomQuarterExtent,
                      "GPUDrivenBloomQuarter",
                      "GPUDrivenBloomQuarterView",
                      m_resources.bloomQuarterImage,
                      m_resources.bloomQuarterView);
    createBloomTarget(m_resources.bloomEighthExtent,
                      "GPUDrivenBloomEighth",
                      "GPUDrivenBloomEighthView",
                      m_resources.bloomEighthImage,
                      m_resources.bloomEighthView);
    createBloomTarget(m_resources.bloomSixteenthExtent,
                      "GPUDrivenBloomSixteenth",
                      "GPUDrivenBloomSixteenthView",
                      m_resources.bloomSixteenthImage,
                      m_resources.bloomSixteenthView);
    createBloomTarget(m_resources.bloomThirtySecondExtent,
                      "GPUDrivenBloomThirtySecond",
                      "GPUDrivenBloomThirtySecondView",
                      m_resources.bloomThirtySecondImage,
                      m_resources.bloomThirtySecondView);
    createBloomTarget(m_resources.bloomUpsampleSixteenthExtent,
                      "GPUDrivenBloomUpsampleSixteenth",
                      "GPUDrivenBloomUpsampleSixteenthView",
                      m_resources.bloomUpsampleSixteenthImage,
                      m_resources.bloomUpsampleSixteenthView);
    createBloomTarget(m_resources.bloomUpsampleEighthExtent,
                      "GPUDrivenBloomUpsampleEighth",
                      "GPUDrivenBloomUpsampleEighthView",
                      m_resources.bloomUpsampleEighthImage,
                      m_resources.bloomUpsampleEighthView);
    createBloomTarget(m_resources.bloomUpsampleQuarterExtent,
                      "GPUDrivenBloomUpsampleQuarter",
                      "GPUDrivenBloomUpsampleQuarterView",
                      m_resources.bloomUpsampleQuarterImage,
                      m_resources.bloomUpsampleQuarterView);
    createBloomTarget(m_resources.bloomOutputExtent,
                      "GPUDrivenBloomOutput",
                      "GPUDrivenBloomOutputView",
                      m_resources.bloomOutputImage,
                      m_resources.bloomOutputView);

    const VkImageCreateInfo velocityInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = kVelocityFormat,
        .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    m_resources.velocityImage = createImage(velocityInfo);
    dutil.setObjectName(m_resources.velocityImage.image, "GPUDrivenVelocity");

    const VkImageViewCreateInfo velocityViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.velocityImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = kVelocityFormat,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &velocityViewInfo, nullptr, &m_resources.velocityView));
    dutil.setObjectName(m_resources.velocityView, "GPUDrivenVelocityView");

    for(uint32_t historyIndex = 0; historyIndex < static_cast<uint32_t>(m_resources.sceneColorHistoryImages.size());
        ++historyIndex)
    {
      const VkImageCreateInfo historyInfo{
          .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType   = VK_IMAGE_TYPE_2D,
          .format      = kSceneColorHdrFormat,
          .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
          .mipLevels   = 1,
          .arrayLayers = 1,
          .samples     = VK_SAMPLE_COUNT_1_BIT,
          .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      m_resources.sceneColorHistoryImages[historyIndex] = createImage(historyInfo);
      dutil.setObjectName(m_resources.sceneColorHistoryImages[historyIndex].image,
                          "GPUDrivenSceneColorHistory" + std::to_string(historyIndex));

      const VkImageViewCreateInfo historyViewInfo{
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = m_resources.sceneColorHistoryImages[historyIndex].image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = kSceneColorHdrFormat,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
      };
      VK_CHECK(vkCreateImageView(m_device,
                                 &historyViewInfo,
                                 nullptr,
                                 &m_resources.sceneColorHistoryViews[historyIndex]));
      dutil.setObjectName(m_resources.sceneColorHistoryViews[historyIndex],
                          "GPUDrivenSceneColorHistoryView" + std::to_string(historyIndex));
    }

    m_resources.colorGradingLutExtent = {kColorGradingLutSize * kColorGradingLutSize, kColorGradingLutSize};
    const VkImageCreateInfo lutInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = kColorGradingLutFormat,
        .extent      = {m_resources.colorGradingLutExtent.width, m_resources.colorGradingLutExtent.height, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    m_resources.colorGradingLutImage = createImage(lutInfo);
    dutil.setObjectName(m_resources.colorGradingLutImage.image, "BuiltInColorGradingLUT");

    const VkImageViewCreateInfo lutViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.colorGradingLutImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = kColorGradingLutFormat,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &lutViewInfo, nullptr, &m_resources.colorGradingLutView));
    dutil.setObjectName(m_resources.colorGradingLutView, "BuiltInColorGradingLUTView");

    const std::vector<uint8_t> lutPixels = generateBuiltInColorGradingLut();
    utils::cmdInitImageLayout(cmd, m_resources.colorGradingLutImage.image);
    BatchUploadContext upload;
    upload.init(m_device, m_allocator, static_cast<VkDeviceSize>(lutPixels.size()) + 16u);
    const BatchUploadContext::Slice slice = upload.allocate(lutPixels.size(), 16);
    std::memcpy(slice.cpuPtr, lutPixels.data(), lutPixels.size());
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .layerCount = 1},
        .imageExtent = {m_resources.colorGradingLutExtent.width, m_resources.colorGradingLutExtent.height, 1},
    };
    upload.recordTextureUpload(slice, m_resources.colorGradingLutImage.image, region);
    upload.executeUploads(cmd);
    m_resources.colorGradingLutStaging = upload.releaseStagingBuffer();
  }

  // Create fixed-resolution shadow map
  {
    const VkImageCreateInfo shadowInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = m_createInfo.depth,
        .extent      = {kShadowMapSize, kShadowMapSize, 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    m_resources.shadowMapImage = createImage(shadowInfo);
    dutil.setObjectName(m_resources.shadowMapImage.image, "ShadowMap");

    const VkImageViewCreateInfo shadowViewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = m_resources.shadowMapImage.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = m_createInfo.depth,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1},
    };
    VK_CHECK(vkCreateImageView(m_device, &shadowViewInfo, nullptr, &m_resources.shadowMapView));
    dutil.setObjectName(m_resources.shadowMapView, "ShadowMapView");
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

    m_resources.depthPyramidExtent = {
        std::max(1u, (m_createInfo.size.width + 1u) / 2u),
        std::max(1u, (m_createInfo.size.height + 1u) / 2u),
    };
    m_resources.depthPyramidMipCount = computeMipCount(m_resources.depthPyramidExtent);

    const VkImageCreateInfo depthPyramidInfo{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = VK_FORMAT_R32_SFLOAT,
        .extent      = {m_resources.depthPyramidExtent.width, m_resources.depthPyramidExtent.height, 1},
        .mipLevels   = m_resources.depthPyramidMipCount,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    m_resources.depthPyramidImage = createImage(depthPyramidInfo);
    dutil.setObjectName(m_resources.depthPyramidImage.image, "DepthPyramid");

    m_resources.depthPyramidMipViews.resize(m_resources.depthPyramidMipCount, VK_NULL_HANDLE);
    for(uint32_t mipLevel = 0; mipLevel < m_resources.depthPyramidMipCount; ++mipLevel)
    {
      const VkImageViewCreateInfo mipViewInfo{
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = m_resources.depthPyramidImage.image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = VK_FORMAT_R32_SFLOAT,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .baseMipLevel = mipLevel,
                               .levelCount = 1,
                               .layerCount = 1},
      };
      VK_CHECK(vkCreateImageView(m_device, &mipViewInfo, nullptr, &m_resources.depthPyramidMipViews[mipLevel]));
      dutil.setObjectName(m_resources.depthPyramidMipViews[mipLevel], "DepthPyramidMipView" + std::to_string(mipLevel));
    }
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
  utils::cmdInitImageLayout(cmd, m_resources.sceneColorHdrImage.image);
  vkCmdClearColorImage(cmd, m_resources.sceneColorHdrImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomHalfImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomHalfImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomQuarterImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomQuarterImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomEighthImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomEighthImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomSixteenthImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomSixteenthImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomThirtySecondImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomThirtySecondImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomUpsampleSixteenthImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomUpsampleSixteenthImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomUpsampleEighthImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomUpsampleEighthImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomUpsampleQuarterImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomUpsampleQuarterImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.bloomOutputImage.image);
  vkCmdClearColorImage(cmd, m_resources.bloomOutputImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &outputClearValue, 1, &outputRange);
  utils::cmdInitImageLayout(cmd, m_resources.velocityImage.image);
  const VkClearColorValue velocityClearValue = {{0.0f, 0.0f, 0.0f, 0.0f}};
  vkCmdClearColorImage(cmd, m_resources.velocityImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       &velocityClearValue, 1, &outputRange);
  for(const utils::Image& historyImage : m_resources.sceneColorHistoryImages)
  {
    utils::cmdInitImageLayout(cmd, historyImage.image);
    vkCmdClearColorImage(cmd, historyImage.image, VK_IMAGE_LAYOUT_GENERAL,
                         &outputClearValue, 1, &outputRange);
  }

  if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
  {
    utils::cmdInitImageLayout(cmd, m_resources.depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    utils::cmdInitImageLayout(cmd, m_resources.shadowMapImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    utils::cmdInitImageLayout(cmd, m_resources.depthPyramidImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
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
  if(m_resources.outputTextureImID && ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendPlatformUserData != nullptr)
  {
    using ImGuiTextureHandle = decltype(ImGui_ImplVulkan_AddTexture(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL));
    ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<ImGuiTextureHandle>(m_resources.outputTextureImID));
  }
  m_resources.outputTextureImID = {};

  if(m_resources.outputTextureView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.outputTextureView, nullptr);
    m_resources.outputTextureView = VK_NULL_HANDLE;
  }
  if(m_resources.sceneColorHdrView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.sceneColorHdrView, nullptr);
    m_resources.sceneColorHdrView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomHalfView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomHalfView, nullptr);
    m_resources.bloomHalfView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomQuarterView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomQuarterView, nullptr);
    m_resources.bloomQuarterView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomEighthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomEighthView, nullptr);
    m_resources.bloomEighthView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomSixteenthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomSixteenthView, nullptr);
    m_resources.bloomSixteenthView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomThirtySecondView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomThirtySecondView, nullptr);
    m_resources.bloomThirtySecondView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomUpsampleSixteenthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomUpsampleSixteenthView, nullptr);
    m_resources.bloomUpsampleSixteenthView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomUpsampleEighthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomUpsampleEighthView, nullptr);
    m_resources.bloomUpsampleEighthView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomUpsampleQuarterView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomUpsampleQuarterView, nullptr);
    m_resources.bloomUpsampleQuarterView = VK_NULL_HANDLE;
  }
  if(m_resources.bloomOutputView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.bloomOutputView, nullptr);
    m_resources.bloomOutputView = VK_NULL_HANDLE;
  }
  if(m_resources.colorGradingLutView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.colorGradingLutView, nullptr);
    m_resources.colorGradingLutView = VK_NULL_HANDLE;
  }
  if(m_resources.velocityView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.velocityView, nullptr);
    m_resources.velocityView = VK_NULL_HANDLE;
  }
  for(VkImageView& historyView : m_resources.sceneColorHistoryViews)
  {
    if(historyView != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, historyView, nullptr);
      historyView = VK_NULL_HANDLE;
    }
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

  for(VkImageView view : m_resources.depthPyramidMipViews)
  {
    if(view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, view, nullptr);
    }
  }
  m_resources.depthPyramidMipViews.clear();

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

  if(m_resources.depthView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.depthView, nullptr);
  }

  if(m_resources.shadowMapView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_resources.shadowMapView, nullptr);
  }

  if(m_resources.outputTextureImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.outputTextureImage.image, m_resources.outputTextureImage.allocation);
  }
  if(m_resources.sceneColorHdrImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.sceneColorHdrImage.image, m_resources.sceneColorHdrImage.allocation);
  }
  if(m_resources.bloomHalfImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomHalfImage.image, m_resources.bloomHalfImage.allocation);
  }
  if(m_resources.bloomQuarterImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomQuarterImage.image, m_resources.bloomQuarterImage.allocation);
  }
  if(m_resources.bloomEighthImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomEighthImage.image, m_resources.bloomEighthImage.allocation);
  }
  if(m_resources.bloomSixteenthImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomSixteenthImage.image, m_resources.bloomSixteenthImage.allocation);
  }
  if(m_resources.bloomThirtySecondImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomThirtySecondImage.image, m_resources.bloomThirtySecondImage.allocation);
  }
  if(m_resources.bloomUpsampleSixteenthImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomUpsampleSixteenthImage.image, m_resources.bloomUpsampleSixteenthImage.allocation);
  }
  if(m_resources.bloomUpsampleEighthImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomUpsampleEighthImage.image, m_resources.bloomUpsampleEighthImage.allocation);
  }
  if(m_resources.bloomUpsampleQuarterImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomUpsampleQuarterImage.image, m_resources.bloomUpsampleQuarterImage.allocation);
  }
  if(m_resources.bloomOutputImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.bloomOutputImage.image, m_resources.bloomOutputImage.allocation);
  }
  if(m_resources.colorGradingLutImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.colorGradingLutImage.image, m_resources.colorGradingLutImage.allocation);
  }
  if(m_resources.colorGradingLutStaging.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_resources.colorGradingLutStaging.buffer, m_resources.colorGradingLutStaging.allocation);
  }
  if(m_resources.velocityImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.velocityImage.image, m_resources.velocityImage.allocation);
  }
  for(utils::Image& historyImage : m_resources.sceneColorHistoryImages)
  {
    if(historyImage.image != VK_NULL_HANDLE)
    {
      vmaDestroyImage(m_allocator, historyImage.image, historyImage.allocation);
    }
  }

  if(m_resources.shadowMapImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.shadowMapImage.image, m_resources.shadowMapImage.allocation);
  }

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

  if(m_resources.depthPyramidImage.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_resources.depthPyramidImage.image, m_resources.depthPyramidImage.allocation);
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

uint64_t SceneResources::getOutputTextureEstimatedBytes() const
{
  return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 4u;
}

VkImage SceneResources::getSceneColorHdrImage() const
{
  return m_resources.sceneColorHdrImage.image;
}

VkImageView SceneResources::getSceneColorHdrView() const
{
  return m_resources.sceneColorHdrView;
}

uint64_t SceneResources::getSceneColorHdrEstimatedBytes() const
{
  return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 8u;
}

VkImage SceneResources::getBloomHalfImage() const
{
  return m_resources.bloomHalfImage.image;
}

VkImageView SceneResources::getBloomHalfView() const
{
  return m_resources.bloomHalfView;
}

VkExtent2D SceneResources::getBloomHalfExtent() const
{
  return m_resources.bloomHalfExtent;
}

VkImage SceneResources::getBloomQuarterImage() const
{
  return m_resources.bloomQuarterImage.image;
}

VkImageView SceneResources::getBloomQuarterView() const
{
  return m_resources.bloomQuarterView;
}

VkExtent2D SceneResources::getBloomQuarterExtent() const
{
  return m_resources.bloomQuarterExtent;
}

VkImage SceneResources::getBloomEighthImage() const
{
  return m_resources.bloomEighthImage.image;
}

VkImageView SceneResources::getBloomEighthView() const
{
  return m_resources.bloomEighthView;
}

VkExtent2D SceneResources::getBloomEighthExtent() const
{
  return m_resources.bloomEighthExtent;
}

VkImage SceneResources::getBloomSixteenthImage() const
{
  return m_resources.bloomSixteenthImage.image;
}

VkImageView SceneResources::getBloomSixteenthView() const
{
  return m_resources.bloomSixteenthView;
}

VkExtent2D SceneResources::getBloomSixteenthExtent() const
{
  return m_resources.bloomSixteenthExtent;
}

VkImage SceneResources::getBloomThirtySecondImage() const
{
  return m_resources.bloomThirtySecondImage.image;
}

VkImageView SceneResources::getBloomThirtySecondView() const
{
  return m_resources.bloomThirtySecondView;
}

VkExtent2D SceneResources::getBloomThirtySecondExtent() const
{
  return m_resources.bloomThirtySecondExtent;
}

VkImage SceneResources::getBloomUpsampleSixteenthImage() const
{
  return m_resources.bloomUpsampleSixteenthImage.image;
}

VkImageView SceneResources::getBloomUpsampleSixteenthView() const
{
  return m_resources.bloomUpsampleSixteenthView;
}

VkExtent2D SceneResources::getBloomUpsampleSixteenthExtent() const
{
  return m_resources.bloomUpsampleSixteenthExtent;
}

VkImage SceneResources::getBloomUpsampleEighthImage() const
{
  return m_resources.bloomUpsampleEighthImage.image;
}

VkImageView SceneResources::getBloomUpsampleEighthView() const
{
  return m_resources.bloomUpsampleEighthView;
}

VkExtent2D SceneResources::getBloomUpsampleEighthExtent() const
{
  return m_resources.bloomUpsampleEighthExtent;
}

VkImage SceneResources::getBloomUpsampleQuarterImage() const
{
  return m_resources.bloomUpsampleQuarterImage.image;
}

VkImageView SceneResources::getBloomUpsampleQuarterView() const
{
  return m_resources.bloomUpsampleQuarterView;
}

VkExtent2D SceneResources::getBloomUpsampleQuarterExtent() const
{
  return m_resources.bloomUpsampleQuarterExtent;
}

VkImage SceneResources::getBloomOutputImage() const
{
  return m_resources.bloomOutputImage.image;
}

VkImageView SceneResources::getBloomOutputView() const
{
  return m_resources.bloomOutputView;
}

VkExtent2D SceneResources::getBloomOutputExtent() const
{
  return m_resources.bloomOutputExtent;
}

VkImage SceneResources::getColorGradingLutImage() const
{
  return m_resources.colorGradingLutImage.image;
}

VkImageView SceneResources::getColorGradingLutView() const
{
  return m_resources.colorGradingLutView;
}

VkExtent2D SceneResources::getColorGradingLutExtent() const
{
  return m_resources.colorGradingLutExtent;
}

uint64_t SceneResources::getBloomEstimatedBytes() const
{
  const auto estimateBytes = [](VkExtent2D extent) {
    return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * 8u;
  };
  return estimateBytes(m_resources.bloomHalfExtent)
       + estimateBytes(m_resources.bloomQuarterExtent)
       + estimateBytes(m_resources.bloomEighthExtent)
       + estimateBytes(m_resources.bloomSixteenthExtent)
       + estimateBytes(m_resources.bloomThirtySecondExtent)
       + estimateBytes(m_resources.bloomUpsampleSixteenthExtent)
       + estimateBytes(m_resources.bloomUpsampleEighthExtent)
       + estimateBytes(m_resources.bloomUpsampleQuarterExtent)
       + estimateBytes(m_resources.bloomOutputExtent);
}

VkImage SceneResources::getVelocityImage() const
{
  return m_resources.velocityImage.image;
}

VkImageView SceneResources::getVelocityView() const
{
  return m_resources.velocityView;
}

uint64_t SceneResources::getVelocityEstimatedBytes() const
{
  return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 4u;
}

VkImage SceneResources::getSceneColorHistoryImage(uint32_t index) const
{
  return m_resources.sceneColorHistoryImages[index % static_cast<uint32_t>(m_resources.sceneColorHistoryImages.size())].image;
}

VkImageView SceneResources::getSceneColorHistoryView(uint32_t index) const
{
  return m_resources.sceneColorHistoryViews[index % static_cast<uint32_t>(m_resources.sceneColorHistoryViews.size())];
}

uint64_t SceneResources::getSceneColorHistoryEstimatedBytes() const
{
  return getSceneColorHdrEstimatedBytes()
         * static_cast<uint64_t>(m_resources.sceneColorHistoryImages.size());
}

VkImage SceneResources::getShadowMapImage() const
{
  return m_resources.shadowMapImage.image;
}

VkImageView SceneResources::getShadowMapView() const
{
  return m_resources.shadowMapView;
}

VkExtent2D SceneResources::getShadowMapExtent() const
{
  return {kShadowMapSize, kShadowMapSize};
}

VkImage SceneResources::getDepthPyramidImage() const
{
  return m_resources.depthPyramidImage.image;
}

VkImageView SceneResources::getDepthPyramidMipView(uint32_t mipLevel) const
{
  if(m_resources.depthPyramidMipViews.empty())
  {
    return VK_NULL_HANDLE;
  }
  return m_resources.depthPyramidMipViews[std::min<uint32_t>(mipLevel, static_cast<uint32_t>(m_resources.depthPyramidMipViews.size() - 1))];
}

VkExtent2D SceneResources::getDepthPyramidExtent() const
{
  return m_resources.depthPyramidExtent;
}

uint32_t SceneResources::getDepthPyramidMipCount() const
{
  return m_resources.depthPyramidMipCount;
}

}  // namespace demo
