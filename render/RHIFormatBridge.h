#pragma once

#include "../rhi/vulkan/internal/VulkanCommon.h"  // VkFormat
#include "../rhi/RHITypes.h"    // rhi::TextureFormat

namespace demo
{
	// Maps a native VkFormat to the portable rhi::TextureFormat. Lives in the render layer
	// (the deliberate image-creation seam still deals in VkFormat) so SceneResources / IBL /
	// HiZ can populate TextureViewCreateDesc::format without each holding a private copy.
	[[nodiscard]] inline rhi::TextureFormat toPortableTextureFormat(VkFormat format)
	{
		switch (format)
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
		// Block-compressed formats (D-01/D-03)
		case VK_FORMAT_BC6H_SFLOAT_BLOCK: return rhi::TextureFormat::bc6hSfloatBlock;
		case VK_FORMAT_BC7_UNORM_BLOCK: return rhi::TextureFormat::bc7UnormBlock;
		case VK_FORMAT_BC7_SRGB_BLOCK: return rhi::TextureFormat::bc7SrgbBlock;
		default:
			return rhi::TextureFormat::undefined;
		}
	}

	[[nodiscard]] inline rhi::TextureFormat toPortableTextureFormat(rhi::TextureFormat format)
	{
		return format;
	}

	// Reverse mapping: portable → VkFormat (Vulkan backend only, call inside render layer)
	[[nodiscard]] inline VkFormat toNativeFormat(rhi::TextureFormat format)
	{
		switch (format)
		{
		case rhi::TextureFormat::rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case rhi::TextureFormat::bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
		case rhi::TextureFormat::rgba16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case rhi::TextureFormat::rg16Sfloat: return VK_FORMAT_R16G16_SFLOAT;
		case rhi::TextureFormat::r32Sfloat: return VK_FORMAT_R32_SFLOAT;
		case rhi::TextureFormat::d16Unorm: return VK_FORMAT_D16_UNORM;
		case rhi::TextureFormat::d32Sfloat: return VK_FORMAT_D32_SFLOAT;
		case rhi::TextureFormat::d24UnormS8: return VK_FORMAT_D24_UNORM_S8_UINT;
		case rhi::TextureFormat::d32SfloatS8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
		case rhi::TextureFormat::bc6hSfloatBlock: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
		case rhi::TextureFormat::bc7UnormBlock: return VK_FORMAT_BC7_UNORM_BLOCK;
		case rhi::TextureFormat::bc7SrgbBlock: return VK_FORMAT_BC7_SRGB_BLOCK;
		// ASTC — add as TextureFormat is extended
		default:
			assert(false && "toNativeFormat: unsupported TextureFormat");
			return VK_FORMAT_UNDEFINED;
		}
	}
} // namespace demo
