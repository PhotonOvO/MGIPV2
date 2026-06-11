#pragma once

// Backend-neutral construction entry points (RDEV init sink). The render layer
// creates Device/Surface through these factories instead of naming backend
// classes; backend selection lives entirely inside the RHI layer.
// Currently Vulkan-only; D3D12/Metal become selectable here once implemented.

#include <memory>

namespace demo::rhi
{
	class Device;
	class Surface;

	[[nodiscard]] std::unique_ptr<Device> createDevice();
	[[nodiscard]] std::unique_ptr<Surface> createSurface();
} // namespace demo::rhi
