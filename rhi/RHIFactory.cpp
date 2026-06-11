#include "RHIFactory.h"

#include "vulkan/VulkanDevice.h"
#include "vulkan/VulkanSurface.h"

namespace demo::rhi
{
	std::unique_ptr<Device> createDevice()
	{
		return std::make_unique<vulkan::VulkanDevice>();
	}

	std::unique_ptr<Surface> createSurface()
	{
		return std::make_unique<vulkan::VulkanSurface>();
	}
} // namespace demo::rhi
