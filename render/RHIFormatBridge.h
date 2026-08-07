#pragma once

#include "../rhi/RHITypes.h"

namespace demo
{
	// Compatibility identity retained for out-of-tree callers. Serialized native
	// format IDs are translated in their owning loaders, not in the render layer.
	[[nodiscard]] constexpr rhi::TextureFormat toPortableTextureFormat(rhi::TextureFormat format)
	{
		return format;
	}
} // namespace demo