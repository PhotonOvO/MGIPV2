#pragma once

#include "../RHIArgumentTable.h"
#include "VulkanFormatUtils.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] constexpr VkImageViewType toVkImageViewType(ImageViewType type) noexcept
{
  switch(type)
  {
  case ImageViewType::e2D: return VK_IMAGE_VIEW_TYPE_2D;
  case ImageViewType::e2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  case ImageViewType::eCube: return VK_IMAGE_VIEW_TYPE_CUBE;
  case ImageViewType::e3D: return VK_IMAGE_VIEW_TYPE_3D;
  }
  return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

[[nodiscard]] constexpr VkImageAspectFlags toVkImageAspect(TextureAspect aspect) noexcept
{
  switch(aspect)
  {
  case TextureAspect::color: return VK_IMAGE_ASPECT_COLOR_BIT;
  case TextureAspect::depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
  case TextureAspect::depthStencil:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  return 0;
}

[[nodiscard]] constexpr VkComponentSwizzle toVkSwizzle(ComponentSwizzle swizzle) noexcept
{
  switch(swizzle)
  {
  case ComponentSwizzle::identity: return VK_COMPONENT_SWIZZLE_IDENTITY;
  case ComponentSwizzle::zero: return VK_COMPONENT_SWIZZLE_ZERO;
  case ComponentSwizzle::one: return VK_COMPONENT_SWIZZLE_ONE;
  case ComponentSwizzle::r: return VK_COMPONENT_SWIZZLE_R;
  case ComponentSwizzle::g: return VK_COMPONENT_SWIZZLE_G;
  case ComponentSwizzle::b: return VK_COMPONENT_SWIZZLE_B;
  case ComponentSwizzle::a: return VK_COMPONENT_SWIZZLE_A;
  }
  return VK_COMPONENT_SWIZZLE_MAX_ENUM;
}

[[nodiscard]] inline VkFormat toVkViewFormat(TextureFormat format) noexcept
{
  return toNativeFormat(format);
}

[[nodiscard]] constexpr VkImageType toVkImageType(TextureDimension dimension) noexcept
{
  switch(dimension)
  {
  case TextureDimension::e2D:
  case TextureDimension::e2DArray:
  case TextureDimension::eCube:
    return VK_IMAGE_TYPE_2D;
  case TextureDimension::e3D:
    return VK_IMAGE_TYPE_3D;
  }
  return VK_IMAGE_TYPE_MAX_ENUM;
}

[[nodiscard]] constexpr VkImageCreateFlags toVkImageCreateFlags(TextureDimension dimension) noexcept
{
  switch(dimension)
  {
  case TextureDimension::e2D:
  case TextureDimension::e2DArray:
  case TextureDimension::e3D:
    return 0;
  case TextureDimension::eCube:
    return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  return 0;
}

[[nodiscard]] constexpr VkImageUsageFlags toVkImageUsage(TextureUsageFlags flags) noexcept
{
  VkImageUsageFlags usage = 0;
  const auto has = [flags](TextureUsageFlags bit) {
    return static_cast<uint32_t>(flags & bit) != 0;
  };
  if(has(TextureUsageFlags::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if(has(TextureUsageFlags::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if(has(TextureUsageFlags::colorAttachment)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if(has(TextureUsageFlags::depthAttachment)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if(has(TextureUsageFlags::transferSrc)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if(has(TextureUsageFlags::transferDst)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if(has(TextureUsageFlags::inputAttachment)) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  return usage;
}

[[nodiscard]] constexpr VkSampleCountFlagBits toVkSampleCount(SampleCount count) noexcept
{
  switch(count)
  {
  case SampleCount::count1: return VK_SAMPLE_COUNT_1_BIT;
  case SampleCount::count2: return VK_SAMPLE_COUNT_2_BIT;
  case SampleCount::count4: return VK_SAMPLE_COUNT_4_BIT;
  case SampleCount::count8: return VK_SAMPLE_COUNT_8_BIT;
  }
  return VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
}

[[nodiscard]] constexpr VkBufferUsageFlags toVkBufferUsage(
  BufferUsageFlags flags, bool allowGpuAddress, bool allowIndirect) noexcept
{
  VkBufferUsageFlags usage = 0;
  const auto has = [flags](BufferUsageFlags bit) {
    return static_cast<uint32_t>(flags & bit) != 0;
  };
  if(has(BufferUsageFlags::vertex)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if(has(BufferUsageFlags::index)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if(has(BufferUsageFlags::uniform)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if(has(BufferUsageFlags::storage)) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if(has(BufferUsageFlags::indirect) || allowIndirect) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if(has(BufferUsageFlags::transferSrc)) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if(has(BufferUsageFlags::transferDst)) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if(has(BufferUsageFlags::shaderDeviceAddress) || allowGpuAddress)
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  return usage;
}

[[nodiscard]] constexpr VkSamplerAddressMode toVkAddressMode(AddressMode mode) noexcept
{
  switch(mode)
  {
  case AddressMode::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case AddressMode::clampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case AddressMode::clampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case AddressMode::mirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  }
  return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
}

[[nodiscard]] constexpr VkDescriptorType toVkDescriptorType(
  ArgumentType type, bool dynamicOffset) noexcept
{
  switch(type)
  {
  case ArgumentType::uniformBuffer:
    return dynamicOffset ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case ArgumentType::storageBuffer:
    return dynamicOffset ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case ArgumentType::sampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case ArgumentType::storageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case ArgumentType::sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
  case ArgumentType::combinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  }
  return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

}  // namespace demo::rhi::vulkan
