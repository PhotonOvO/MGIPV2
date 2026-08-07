#pragma once

// Backend-neutral construction entry points. Backend selection stays inside RHI.

#include "RHIBackend.h"

#include <memory>

namespace demo::rhi
{
	class Device;
	class Surface;


	[[nodiscard]] constexpr BackendType defaultBackend()
	{
		return BackendType::vulkan;
	}

	[[nodiscard]] const char* toString(BackendType backend);
	[[nodiscard]] bool isBackendAvailable(BackendType backend);
	[[nodiscard]] std::unique_ptr<Device> createDevice(BackendType backend = defaultBackend());
	[[nodiscard]] std::unique_ptr<Surface> createSurface(BackendType backend = defaultBackend());
} // namespace demo::rhi
