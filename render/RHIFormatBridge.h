#pragma once

#include "../common/Common.h"  // VkFormat
#include "../rhi/RHITypes.h"    // rhi::TextureFormat

namespace demo {

// Maps a native VkFormat to the portable rhi::TextureFormat. Lives in the render layer
// (the deliberate image-creation seam still deals in VkFormat) so SceneResources / IBL /
// HiZ can populate TextureViewCreateDesc::format without each holding a private copy.
[[nodiscard]] inline rhi::TextureFormat toPortableTextureFormat(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return rhi::TextureFormat::rgba8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return rhi::TextureFormat::bgra8Unorm;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return rhi::TextureFormat::rgba16Sfloat;
    case VK_FORMAT_R16G16_SFLOAT:
      return rhi::TextureFormat::rg16Sfloat;
    case VK_FORMAT_R32_SFLOAT:
      return rhi::TextureFormat::r32Sfloat;
    case VK_FORMAT_D16_UNORM:
      return rhi::TextureFormat::d16Unorm;
    case VK_FORMAT_D32_SFLOAT:
      return rhi::TextureFormat::d32Sfloat;
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return rhi::TextureFormat::d24UnormS8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return rhi::TextureFormat::d32SfloatS8;
    default:
      return rhi::TextureFormat::undefined;
  }
}

}  // namespace demo
