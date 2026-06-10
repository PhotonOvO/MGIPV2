#pragma once

// rhi/vulkan internal format conversion utilities (IFACE-01/02).
// This file is a rhi/vulkan-layer tool header: it may only include rhi/RHITypes.h
// and Vulkan headers. It must NOT include anything from render/ to preserve the
// unidirectional rhi/vulkan -> render layering rule.

#include "rhi/RHITypes.h"   // rhi::TextureFormat, rhi::FormatFeatureFlag
#include "volk.h"           // VkFormat + VK_FORMAT_* constants

#include <cstdio>           // fprintf (warning fallback)

namespace demo::rhi::vulkan
{

// Maps a VkFormat to the portable rhi::TextureFormat.
// Covers swapchain-common formats and formats used by the depth/color pass queries.
// Unknown formats log a warning and return rhi::TextureFormat::undefined.
[[nodiscard]] inline rhi::TextureFormat toPortableTextureFormat(VkFormat vkFmt)
{
    switch (vkFmt)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:           return rhi::TextureFormat::rgba8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:           return rhi::TextureFormat::bgra8Unorm;
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return rhi::TextureFormat::rgba16Sfloat;
    case VK_FORMAT_R16G16_SFLOAT:            return rhi::TextureFormat::rg16Sfloat;
    case VK_FORMAT_R32_SFLOAT:               return rhi::TextureFormat::r32Sfloat;
    case VK_FORMAT_R16_SFLOAT:               return rhi::TextureFormat::r16Sfloat;
    case VK_FORMAT_D16_UNORM:                return rhi::TextureFormat::d16Unorm;
    case VK_FORMAT_D32_SFLOAT:               return rhi::TextureFormat::d32Sfloat;
    case VK_FORMAT_D24_UNORM_S8_UINT:        return rhi::TextureFormat::d24UnormS8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:       return rhi::TextureFormat::d32SfloatS8;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:        return rhi::TextureFormat::bc6hUfloatBlock;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:        return rhi::TextureFormat::bc6hSfloatBlock;
    case VK_FORMAT_BC7_UNORM_BLOCK:          return rhi::TextureFormat::bc7UnormBlock;
    case VK_FORMAT_BC7_SRGB_BLOCK:           return rhi::TextureFormat::bc7SrgbBlock;
    default:
        fprintf(stderr, "VulkanFormatUtils: unknown VkFormat %d\n", static_cast<int>(vkFmt));
        return rhi::TextureFormat::undefined;
    }
}

// Maps a portable rhi::TextureFormat to VkFormat.
// Unknown formats return VK_FORMAT_UNDEFINED.
[[nodiscard]] inline VkFormat toNativeFormat(rhi::TextureFormat fmt)
{
    switch (fmt)
    {
    case rhi::TextureFormat::rgba8Unorm:      return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::TextureFormat::bgra8Unorm:      return VK_FORMAT_B8G8R8A8_UNORM;
    case rhi::TextureFormat::rgba16Sfloat:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case rhi::TextureFormat::rg16Sfloat:      return VK_FORMAT_R16G16_SFLOAT;
    case rhi::TextureFormat::r32Sfloat:       return VK_FORMAT_R32_SFLOAT;
    case rhi::TextureFormat::r16Sfloat:       return VK_FORMAT_R16_SFLOAT;
    case rhi::TextureFormat::d16Unorm:        return VK_FORMAT_D16_UNORM;
    case rhi::TextureFormat::d32Sfloat:       return VK_FORMAT_D32_SFLOAT;
    case rhi::TextureFormat::d24UnormS8:      return VK_FORMAT_D24_UNORM_S8_UINT;
    case rhi::TextureFormat::d32SfloatS8:     return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case rhi::TextureFormat::bc6hUfloatBlock: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case rhi::TextureFormat::bc6hSfloatBlock: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case rhi::TextureFormat::bc7UnormBlock:   return VK_FORMAT_BC7_UNORM_BLOCK;
    case rhi::TextureFormat::bc7SrgbBlock:    return VK_FORMAT_BC7_SRGB_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

}  // namespace demo::rhi::vulkan
