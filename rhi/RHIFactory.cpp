#include "RHIFactory.h"

#include "vulkan/VulkanDevice.h"
#include "vulkan/VulkanSurface.h"

#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
#include "d3d12/D3D12Device.h"
#include "d3d12/D3D12Surface.h"
#endif

#include <stdexcept>
#include <string>

namespace demo::rhi
{
	const char* toString(BackendType backend)
	{
		switch (backend)
		{
		case BackendType::vulkan: return "vulkan";
		case BackendType::d3d12: return "d3d12";
		case BackendType::metal: return "metal";
		}
		return "unknown";
	}

	bool isBackendAvailable(BackendType backend)
	{
		switch (backend)
		{
		case BackendType::vulkan:
			return true;
		case BackendType::d3d12:
#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
			return true;
#else
			return false;
#endif
		case BackendType::metal:
			return false;
		}
		return false;
	}

	std::unique_ptr<Device> createDevice(BackendType backend)
	{
		switch (backend)
		{
		case BackendType::vulkan:
			return std::make_unique<vulkan::VulkanDevice>();
		case BackendType::d3d12:
#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
			return std::make_unique<d3d12::D3D12Device>();
#else
			break;
#endif
		case BackendType::metal:
			break;
		}
		throw std::runtime_error(std::string("RHI backend is not available in this build: ") + toString(backend));
	}

	std::unique_ptr<Surface> createSurface(BackendType backend)
	{
		switch (backend)
		{
		case BackendType::vulkan:
			return std::make_unique<vulkan::VulkanSurface>();
		case BackendType::d3d12:
#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
			return std::make_unique<d3d12::D3D12Surface>();
#else
			break;
#endif
		case BackendType::metal:
			break;
		}
		throw std::runtime_error(std::string("RHI backend is not available in this build: ") + toString(backend));
	}
} // namespace demo::rhi
