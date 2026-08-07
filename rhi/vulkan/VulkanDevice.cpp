#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include "VulkanDevice.h"
#include "../../common/ProfilerMarkers.h"
#include "VulkanCommandAllocator.h"
#include "internal/VulkanCommon.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDeviceLossPolicy.h"
#include "VulkanPipelines.h"
#include "VulkanResourceConversions.h"
#include "VulkanResourceTable.h"
#include "VulkanShaderConversions.h"
#include "VulkanSurface.h"
#include "VulkanSwapchain.h"
#include "rhi/vulkan/VulkanFormatUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <csignal>
#endif

namespace demo::rhi::vulkan
{
	namespace
	{
		constexpr CapabilityReport kDeterminismProbeReport{};
		constexpr CapabilityRequirements kDeterminismProbeRequirements{};
		static_assert(
			evaluateCapabilityRequirements(kDeterminismProbeReport, kDeterminismProbeRequirements) ==
			RHICapabilityError::MissingCoreGraphics,
			"Mandatory capability failures must be deterministic");

		constexpr uint32_t kDefaultCombinedImageSamplerPoolCapacity = 16384u;

		uint64_t asNativeU64(const void* handle)
		{
			return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
		}

		bool cstrEqual(const char* lhs, const char* rhs)
		{
			if (lhs == nullptr || rhs == nullptr)
			{
				return false;
			}
			return std::strcmp(lhs, rhs) == 0;
		}

		void pushUnique(std::vector<const char*>& values, const char* value)
		{
			if (value == nullptr)
			{
				return;
			}
			const bool found =
				std::any_of(values.begin(), values.end(), [value](const char* current)
				{
					return cstrEqual(current, value);
				});
			if (!found)
			{
				values.push_back(value);
			}
		}

		void ensure(bool condition, const char* message)
		{
			if (!condition)
			{
				throw std::runtime_error(message);
			}
		}

		void checkVk(VkResult result, const char* message)
		{
			if (result != VK_SUCCESS)
			{
				const char* resultName = string_VkResult(result);
				LOGE("%s (VkResult=%s)", message, resultName != nullptr ? resultName : "UNKNOWN");
				throw std::runtime_error(message);
			}
		}
	} // namespace

	VulkanDevice::VulkanDevice() = default;

	VulkanDevice::~VulkanDevice()
	{
		deinit();
	}

	void VulkanDevice::init(const DeviceCreateInfo& createInfo)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.Init");
		// Delegate to initVulkan with only the base fields; Vulkan extension/layer
		// fields remain empty. RenderDevice calls initVulkan() directly to supply them.
		VulkanDeviceCreateInfo vkCreateInfo;
		vkCreateInfo.base = createInfo;
		initVulkan(vkCreateInfo);
	}

	void VulkanDevice::initVulkan(const VulkanDeviceCreateInfo& createInfo)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.InitVulkan");
		ensure(!m_initialized, "VulkanDevice::initVulkan called twice");
		m_queueGeneration = demo::detail::encodeHandleGeneration(
			demo::detail::acquireHandlePoolOwner(), 1);
		m_createInfo = createInfo;
		LOGI("VulkanDevice::init: begin");

		// Vulkan loader bootstrap — sunk from RenderDevice (RDEV-06).
		VK_CHECK(volkInitialize());

		// Engine-default Vulkan extensions and feature chains (RDEV-06): the render
		// layer only supplies backend-neutral DeviceCreateInfo fields; everything
		// Vulkan-specific is appended here. The feature structs only need to live
		// until initLogicalDevice() consumes the pNext chain below.
		static VkPhysicalDeviceExtendedDynamicState3FeaturesEXT sDynamicState3Features;
		static VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR sUnifiedImageLayoutsFeature;
		sDynamicState3Features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT
		};
		sUnifiedImageLayoutsFeature = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR
		};
		// Window-system-integration surface extensions per platform.
		m_createInfo.instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __ANDROID__
		m_createInfo.instanceExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(_WIN32)
		m_createInfo.instanceExtensions.push_back("VK_KHR_win32_surface");
#endif
		// Debug utils for event markers (RenderDoc, PIX, etc.)
		m_createInfo.instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		m_createInfo.deviceExtensions.push_back({VK_KHR_SWAPCHAIN_EXTENSION_NAME, true, nullptr});
		m_createInfo.deviceExtensions.push_back({VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, false, nullptr});
		m_createInfo.deviceExtensions.push_back({
			VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, false, &sUnifiedImageLayoutsFeature
		});
		m_createInfo.deviceExtensions.push_back({
			VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, false, &sDynamicState3Features
		});
		m_createInfo.deviceExtensions.push_back({"VK_EXT_full_screen_exclusive", false, nullptr});

		initInstance();
		ensure(m_instance != VK_NULL_HANDLE, "VulkanDevice::init failed: Vulkan instance is null");
		selectPhysicalDevice();
		initLogicalDevice();

		m_resourceTable = std::make_unique<VulkanResourceTable>();
		m_queueSyncRegistry = std::make_unique<VulkanQueueSyncRegistry>();
		m_graphicsQueueApi = std::make_unique<VulkanQueue>();
		m_computeQueueApi = std::make_unique<VulkanQueue>();
		m_transferQueueApi = std::make_unique<VulkanQueue>();

		QueueInfo graphicsInfo = m_graphicsQueue.toRhi();
		QueueInfo computeInfo = m_computeQueue.toRhi();
		computeInfo.queueClass = QueueClass::compute;
		computeInfo.capabilities.supportsPresent = false;
		computeInfo.dedicated = m_computeQueue.familyIndex != m_graphicsQueue.familyIndex;
		QueueInfo transferInfo = m_transferQueue.toRhi();
		transferInfo.queueClass = QueueClass::transfer;
		transferInfo.capabilities.supportsPresent = false;
		transferInfo.dedicated = m_transferQueue.familyIndex != m_graphicsQueue.familyIndex
			&& m_transferQueue.familyIndex != m_computeQueue.familyIndex;

		m_graphicsQueueApi->init(
			m_device, m_graphicsQueue.queue, m_graphicsQueue.familyIndex,
			m_graphicsQueue.queueIndex, QueueIdentity{1, m_queueGeneration}, graphicsInfo,
			m_queueSyncRegistry.get());
		m_computeQueueApi->init(
			m_device, m_computeQueue.queue, m_computeQueue.familyIndex,
			m_computeQueue.queueIndex, QueueIdentity{2, m_queueGeneration}, computeInfo,
			m_queueSyncRegistry.get());
		m_transferQueueApi->init(
			m_device, m_transferQueue.queue, m_transferQueue.familyIndex,
			m_transferQueue.queueIndex, QueueIdentity{3, m_queueGeneration}, transferInfo,
			m_queueSyncRegistry.get());

		const std::array<VulkanQueue*, 3> queueRegistry{
			m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()
		};
		m_graphicsQueueApi->setQueueRegistry(queueRegistry);
		m_computeQueueApi->setQueueRegistry(queueRegistry);
		m_transferQueueApi->setQueueRegistry(queueRegistry);

		// VMA allocator — created and owned backend-internally (RDEV-06).
		{
			VmaAllocatorCreateInfo allocatorInfo{
				.physicalDevice = m_physicalDevice,
				.device = m_device,
				.instance = m_instance,
				.vulkanApiVersion = m_apiVersion,
			};
			allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
			allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
			allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

			const VmaVulkanFunctions functions{
				.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
				.vkGetDeviceProcAddr = vkGetDeviceProcAddr,
			};
			allocatorInfo.pVulkanFunctions = &functions;
			VK_CHECK(vmaCreateAllocator(&allocatorInfo, &m_allocator));
		}

		m_initialized = true;
		LOGI("VulkanDevice::init: completed");
	}

	void VulkanDevice::deinit()
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.Deinit");
		if (m_device != VK_NULL_HANDLE)
		{
			const VkResult idleResult = vkDeviceWaitIdle(m_device);
			if (!permitsEmergencyRetirementDrain(idleResult))
				checkVk(idleResult, "VulkanDevice::deinit failed waiting for device idle");
			drainRetirements();
			releaseResourceTableObjects();
			drainRetirements();
			m_transferQueueApi.reset();
			m_computeQueueApi.reset();
			m_graphicsQueueApi.reset();
			m_queueSyncRegistry.reset();
			for (VkDescriptorPool pool : m_argumentPools)
			{
				vkDestroyDescriptorPool(m_device, pool, nullptr);
			}
			m_argumentPools.clear();
			m_argumentSetPools.clear();
			m_argumentPool = VK_NULL_HANDLE;
			m_combinedImageSamplerPoolCapacity = kDefaultCombinedImageSamplerPoolCapacity;
			// VMA allocator — destroyed after all retirements drained, before the device.
			if (m_allocator != nullptr)
			{
				vmaDestroyAllocator(m_allocator);
				m_allocator = nullptr;
			}
			m_resourceTable.reset();
			vkDestroyDevice(m_device, nullptr);
			m_device = VK_NULL_HANDLE;
		}

		destroyDebugMessenger();

		if (m_instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(m_instance, nullptr);
			m_instance = VK_NULL_HANDLE;
		}

		m_physicalDevice = VK_NULL_HANDLE;
		m_graphicsQueue = {};
		m_computeQueue = {};
		m_transferQueue = {};
		m_apiVersion = 0;
		m_physicalDeviceInfo = {};
		m_featureInfo = {};
		m_capabilities = {};
		m_capabilityError = RHICapabilityError::None;
		m_memoryProperties = {};
		m_vkMemoryProperties = {};
		m_availableDeviceExtensions.clear();
		m_availableInstanceExtensions.clear();
		m_availableInstanceLayers.clear();
		m_enabledDeviceExtensions.clear();
		m_featuresChainHead = nullptr;
		m_deviceFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		m_features11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		m_features12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		m_features13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
		m_features14 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#else
		m_maintenance5Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
		m_maintenance6Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
#endif
		m_meshShaderFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
		m_accelerationStructureFeatures = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
		};
		m_rayTracingPipelineFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
		m_initialized = false;
		m_pendingRetirements.clear();
	}

	BackendInfo VulkanDevice::getBackendInfo() const
	{
		return BackendInfo{
			.type = BackendType::vulkan,
			.apiName = "Vulkan",
			.version = BackendVersion{
				.major = VK_API_VERSION_MAJOR(m_apiVersion),
				.minor = VK_API_VERSION_MINOR(m_apiVersion),
				.patch = VK_API_VERSION_PATCH(m_apiVersion),
				.nativeValue = m_apiVersion,
			},
		};
	}

	const char* VulkanDevice::getDeviceName() const
	{
		return m_physicalDeviceInfo.deviceName.c_str();
	}

	const PhysicalDeviceInfo& VulkanDevice::getPhysicalDeviceInfo() const
	{
		return m_physicalDeviceInfo;
	}

	const DeviceFeatureInfo& VulkanDevice::getEnabledFeatureInfo() const
	{
		return m_featureInfo;
	}

	CapabilityReport VulkanDevice::queryCapabilities() const
	{
		return m_capabilities;
	}

	bool VulkanDevice::supports(CapabilityTier tier) const
	{
		return supportsTier(m_capabilities, tier);
	}

	const MemoryProperties& VulkanDevice::getPhysicalMemoryProperties() const
	{
		return m_memoryProperties;
	}

	Queue* VulkanDevice::getQueue(QueueClass queueClass)
	{
		switch (queueClass)
		{
		case QueueClass::graphics: return m_graphicsQueueApi.get();
		case QueueClass::compute: return m_computeQueueApi.get();
		case QueueClass::transfer: return m_transferQueueApi.get();
		}
		return nullptr;
	}

	std::unique_ptr<CommandAllocator> VulkanDevice::createCommandAllocator(QueueClass queueClass)
	{
		auto* queue = dynamic_cast<VulkanQueue*>(getQueue(queueClass));
		ensure(queue != nullptr, "VulkanDevice::createCommandAllocator requires an available queue");
		ensure(m_resourceTable != nullptr,
		       "VulkanDevice::createCommandAllocator requires the resource table");
		return std::make_unique<VulkanCommandAllocator>(*queue, m_device, m_resourceTable.get());
	}

	void VulkanDevice::collectGarbage()
	{
		processRetirements();
	}
	void VulkanDevice::initSurface(Surface& surface, const WindowHandle& window)
	{
		ensure(m_instance != VK_NULL_HANDLE && m_physicalDevice != VK_NULL_HANDLE,
		       "VulkanDevice::initSurface requires an initialized device");
		auto* vulkanSurface = dynamic_cast<VulkanSurface*>(&surface);
		ensure(vulkanSurface != nullptr,
		       "VulkanDevice::initSurface received a surface from another backend");
		vulkanSurface->initVulkan(m_instance, m_physicalDevice, window);
	}

	std::unique_ptr<Swapchain> VulkanDevice::createSwapchain(Surface& surface, bool vSync)
	{
		ensure(m_device != VK_NULL_HANDLE, "VulkanDevice::createSwapchain requires an initialized device");
		const VkSurfaceKHR nativeSurface = static_cast<VulkanSurface&>(surface).backendHandle();
		ensure(nativeSurface != VK_NULL_HANDLE, "VulkanDevice::createSwapchain requires an initialized surface");
		DBG_VK_NAME(nativeSurface);
		auto swapchain = std::make_unique<VulkanSwapchain>();
		swapchain->init(static_cast<void*>(m_physicalDevice), static_cast<void*>(m_device),
		                static_cast<void*>(m_graphicsQueue.queue), static_cast<void*>(nativeSurface),
		                m_queueSyncRegistry.get(), m_resourceTable.get(), vSync);
		return swapchain;
	}

	float VulkanDevice::getTimestampPeriodNs() const
	{
		if (m_physicalDevice == VK_NULL_HANDLE)
		{
			return 0.0f;
		}
		VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		vkGetPhysicalDeviceProperties2(m_physicalDevice, &deviceProperties2);
		return deviceProperties2.properties.limits.timestampPeriod;
	}


	bool VulkanDevice::isFormatSupported(TextureFormat format, FormatFeatureFlag feature) const
	{
		const VkFormat vkFmt = vulkan::toNativeFormat(format);
		if (vkFmt == VK_FORMAT_UNDEFINED)
		{
			return false;
		}

		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, vkFmt, &props);

		const auto featureBits = static_cast<uint32_t>(feature);
		// sampledImage: check optimalTilingFeatures (matches original supportsSampledImageFormat)
		if ((featureBits & static_cast<uint32_t>(FormatFeatureFlag::sampledImage)) != 0)
		{
			return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
		}
		if ((featureBits & static_cast<uint32_t>(FormatFeatureFlag::colorAttachment)) != 0)
		{
			return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
		}
		if ((featureBits & static_cast<uint32_t>(FormatFeatureFlag::storageImage)) != 0)
		{
			return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
		}
		if ((featureBits & static_cast<uint32_t>(FormatFeatureFlag::depthStencilAttachment)) != 0)
		{
			return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
		}
		return false;
	}

	void VulkanDevice::waitIdle()
	{
		if (m_device != VK_NULL_HANDLE)
		{
			VK_CHECK(vkDeviceWaitIdle(m_device));
			drainRetirements();
		}
	}

	SubmissionTokenSet VulkanDevice::retirementDependencies() const
	{
		SubmissionTokenSet dependencies{};
		const std::array<VulkanQueue*, 3> queues{
			m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()};
		for (const VulkanQueue* queue : queues)
		{
			if (queue != nullptr)
			{
				const bool recorded = dependencies.record(queue->lastSubmittedToken());
				ensure(recorded, "Vulkan retirement dependency capacity exceeded");
			}
		}
		return dependencies;
	}

	bool VulkanDevice::isRetirementComplete(const SubmissionTokenSet& dependencies) const
	{
		const std::array<VulkanQueue*, 3> queues{
			m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()};
		for (uint32_t dependencyIndex = 0; dependencyIndex < dependencies.count; ++dependencyIndex)
		{
			const SubmissionToken dependency = dependencies.tokens[dependencyIndex];
			bool complete = false;
			for (const VulkanQueue* queue : queues)
			{
				if (queue == nullptr || queue->identity().index != dependency.queue.index)
					continue;
				if (queue->identity() != dependency.queue)
				{
					// Timeline recreation waits the old queue idle before changing identity.
					complete = true;
				}
				else
				{
					complete = queue->completedValue() >= dependency.value;
				}
				break;
			}
			if (!complete)
				return false;
		}
		return true;
	}

	void VulkanDevice::enqueueRetirement(NativeRetirement retirement)
	{
		if (!retirement.owned && !retirement.ownsSecondary)
		{
			return;
		}

		if (!retirement.preciseDependencies &&
		    retirement.retirementDependencies.count == 0)
			retirement.retirementDependencies = retirementDependencies();
		m_pendingRetirements.push_back(retirement);
	}

	uint32_t VulkanDevice::processRetirements()
	{
		uint32_t destroyed = 0;
		auto it = std::remove_if(m_pendingRetirements.begin(), m_pendingRetirements.end(),
		                         [this, &destroyed](const NativeRetirement& retirement)
		                         {
			                         if (!isRetirementComplete(retirement.retirementDependencies))
			                         {
				                         return false;
			                         }

			                         destroyRetiredResource(retirement);
			                         ++destroyed;
			                         return true;
		                         });
		m_pendingRetirements.erase(it, m_pendingRetirements.end());
		return destroyed;
	}

	void VulkanDevice::drainRetirements()
	{
		for (const NativeRetirement& retirement : m_pendingRetirements)
		{
			destroyRetiredResource(retirement);
		}
		m_pendingRetirements.clear();
	}

	void VulkanDevice::releaseResourceTableObjects()
	{
		if (m_resourceTable == nullptr)
		{
			return;
		}

		std::vector<ArgumentTableHandle> argumentTables;
		m_resourceTable->forEachArgumentTable(
			[&argumentTables](ArgumentTableHandle handle, const ArgumentTableRecord&)
			{
				argumentTables.push_back(handle);
			});
		for (ArgumentTableHandle handle : argumentTables) destroyArgumentTable(handle);

		std::vector<PipelineHandle> pipelines;
		m_resourceTable->forEachPipeline(
			[&pipelines](PipelineHandle handle, const PipelineRecord&)
			{
				pipelines.push_back(handle);
			});
		for (PipelineHandle handle : pipelines) destroyPipeline(handle);

		std::vector<ShaderLibraryHandle> shaderLibraries;
		m_resourceTable->forEachShaderLibrary(
			[&shaderLibraries](ShaderLibraryHandle handle, const ShaderLibraryRecord&)
			{
				shaderLibraries.push_back(handle);
			});
		for (ShaderLibraryHandle handle : shaderLibraries) destroyShaderLibrary(handle);

		std::vector<TextureViewHandle> textureViews;
		m_resourceTable->forEachTextureView(
			[&textureViews](TextureViewHandle handle, const TextureViewHotRecord&)
			{
				textureViews.push_back(handle);
			});
		for (TextureViewHandle handle : textureViews) destroyTextureView(handle);

		std::vector<SamplerHandle> samplers;
		m_resourceTable->forEachSampler(
			[&samplers](SamplerHandle handle, const SamplerRecord&)
			{
				samplers.push_back(handle);
			});
		for (SamplerHandle handle : samplers) destroySampler(handle);

		std::vector<QueryPoolHandle> queryPools;
		m_resourceTable->forEachQueryPool(
			[&queryPools](QueryPoolHandle handle, const QueryPoolRecord&)
			{
				queryPools.push_back(handle);
			});
		for (QueryPoolHandle handle : queryPools) destroyQueryPool(handle);

		std::vector<TextureHandle> textures;
		m_resourceTable->forEachTexture(
			[&textures](TextureHandle handle, const TextureHotRecord&)
			{
				textures.push_back(handle);
			});
		for (TextureHandle handle : textures) destroyTexture(handle);

		std::vector<BufferHandle> buffers;
		m_resourceTable->forEachBuffer(
			[&buffers](BufferHandle handle, const BufferHotRecord&)
			{
				buffers.push_back(handle);
			});
		for (BufferHandle handle : buffers) destroyBuffer(handle);

		std::vector<ArgumentLayoutHandle> argumentLayouts;
		m_resourceTable->forEachArgumentLayout(
			[&argumentLayouts](ArgumentLayoutHandle handle, const ArgumentLayoutRecord&)
			{
				argumentLayouts.push_back(handle);
			});
		for (ArgumentLayoutHandle handle : argumentLayouts) destroyArgumentLayout(handle);
	}
	void VulkanDevice::destroyRetiredResource(const NativeRetirement& retirement)
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		switch (retirement.resource.kind)
		{
		case ResourceKind::TextureView:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				vkDestroyImageView(
					m_device, reinterpret_cast<VkImageView>(static_cast<uintptr_t>(retirement.nativeObject)), nullptr);
			}
			break;
		case ResourceKind::Texture:
			if (retirement.owned && retirement.nativeObject != 0 && m_allocator != nullptr)
			{
				vmaDestroyImage(m_allocator, reinterpret_cast<VkImage>(static_cast<uintptr_t>(retirement.nativeObject)),
				                reinterpret_cast<VmaAllocation>(static_cast<uintptr_t>(retirement.nativeAllocation)));
			}
			break;
		case ResourceKind::Buffer:
			if (retirement.owned && retirement.nativeObject != 0 && m_allocator != nullptr)
			{
				vmaDestroyBuffer(m_allocator,
				                 reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(retirement.nativeObject)),
				                 reinterpret_cast<VmaAllocation>(static_cast<uintptr_t>(retirement.nativeAllocation)));
			}
			break;
		case ResourceKind::Sampler:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				vkDestroySampler(m_device, reinterpret_cast<VkSampler>(static_cast<uintptr_t>(retirement.nativeObject)),
				                 nullptr);
			}
			break;
		case ResourceKind::QueryPool:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				vkDestroyQueryPool(
					m_device, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(retirement.nativeObject)), nullptr);
			}
			break;
		case ResourceKind::ArgumentLayout:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				vkDestroyDescriptorSetLayout(
					m_device, reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(retirement.nativeObject)),
					nullptr);
			}
			break;
		case ResourceKind::ArgumentTable:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				const auto poolIt = m_argumentSetPools.find(retirement.nativeObject);
				if (poolIt != m_argumentSetPools.end())
				{
					VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(retirement.
						nativeObject));
					vkFreeDescriptorSets(m_device, poolIt->second, 1, &set);
					m_argumentSetPools.erase(poolIt);
				}
			}
			break;
		case ResourceKind::Pipeline:
			if (retirement.owned && retirement.nativeObject != 0)
			{
				vkDestroyPipeline(
					m_device, reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(retirement.nativeObject)), nullptr);
			}
			if (retirement.ownsSecondary && retirement.secondaryNativeObject != 0)
			{
				vkDestroyPipelineLayout(
					m_device,
					reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(retirement.secondaryNativeObject)),
					nullptr);
			}
			break;
		default:
			break;
		}
	}

	QueueInfo VulkanDevice::NativeQueueInfo::toRhi() const
	{
		return QueueInfo{
			.queueClass = QueueClass::graphics,
			.capabilities = {
				.supportsTimestamps = true,
				.supportsPresent = true,
				.supportsSparseBinding = false,
			},
			.dedicated = false,
			.available = queue != VK_NULL_HANDLE,
		};
	}

	void VulkanDevice::initInstance()
	{
		ensure(vkCreateInstance != nullptr,
		       "Vulkan loader is not initialized. Call volkInitialize() before VulkanDevice::init");
		checkVk(vkEnumerateInstanceVersion(&m_apiVersion), "Failed to enumerate Vulkan instance version");
#ifdef VK_API_VERSION_1_4
		ensure(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 loader");
#else
		ensure(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0), "Require Vulkan 1.3 loader");
#endif

		queryInstanceExtensions();
		queryInstanceLayers();

		std::vector<const char*> enabledInstanceExtensions = m_createInfo.instanceExtensions;

		if (m_createInfo.base.enableValidationLayers && extensionAvailable(
			VK_EXT_DEBUG_UTILS_EXTENSION_NAME, m_availableInstanceExtensions))
		{
			pushUnique(enabledInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
		if (extensionAvailable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, m_availableInstanceExtensions))
		{
			pushUnique(enabledInstanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		}
		if (extensionAvailable(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, m_availableInstanceExtensions))
		{
			pushUnique(enabledInstanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
		}

		for (const char* ext : enabledInstanceExtensions)
		{
			ensure(extensionAvailable(ext, m_availableInstanceExtensions),
			       "Requested instance extension is not supported");
		}

		std::vector<const char*> enabledLayers = m_createInfo.instanceLayers;
		if (m_createInfo.base.enableValidationLayers)
		{
			pushUnique(enabledLayers, "VK_LAYER_KHRONOS_validation");
		}
		for (const char* layer : enabledLayers)
		{
			ensure(layerAvailable(layer, m_availableInstanceLayers), "Requested instance layer is not supported");
		}

		const VkApplicationInfo appInfo{
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "minimal_latest",
			.applicationVersion = 1,
			.pEngineName = "minimal_latest",
			.engineVersion = 1,
			.apiVersion = m_apiVersion,
		};

		// Validation settings with GPU-assisted validation enabled
		utils::ValidationSettings validationSettings{};

		const VkInstanceCreateInfo instanceCreateInfo{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pNext = m_createInfo.base.enableValidationLayers ? validationSettings.buildPNextChain() : nullptr,
			.pApplicationInfo = &appInfo,
			.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
			.ppEnabledLayerNames = enabledLayers.data(),
			.enabledExtensionCount = static_cast<uint32_t>(enabledInstanceExtensions.size()),
			.ppEnabledExtensionNames = enabledInstanceExtensions.data(),
		};

		LOGI("VulkanDevice::initInstance: creating Vulkan instance");
		const VkResult createResult = vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance);
		if (createResult != VK_SUCCESS)
		{
			const char* resultName = string_VkResult(createResult);
			LOGE("Failed to create Vulkan instance (VkResult=%s)", resultName != nullptr ? resultName : "UNKNOWN");
			throw std::runtime_error("Failed to create Vulkan instance");
		}

		ensure(m_instance != VK_NULL_HANDLE, "vkCreateInstance returned success but instance handle is null");
		volkLoadInstance(m_instance);
		ensure(vkEnumeratePhysicalDevices != nullptr,
		       "Failed to load instance-level Vulkan entry points after vkCreateInstance");

		LOGI("VulkanDevice::initInstance: Vulkan instance created and function pointers loaded");
		initDebugMessenger();
	}

	void VulkanDevice::selectPhysicalDevice()
	{
		ensure(m_instance != VK_NULL_HANDLE, "VulkanDevice::selectPhysicalDevice called with null instance");
		ensure(vkEnumeratePhysicalDevices != nullptr,
		       "vkEnumeratePhysicalDevices is null (instance functions not loaded)");

		uint32_t deviceCount = 0;
		checkVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr), "Failed to enumerate physical devices");
		ensure(deviceCount > 0, "Failed to find Vulkan physical device");

		std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
		checkVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data()),
		        "Failed to enumerate physical devices list");

		size_t selectedIndex = 0;
		for (size_t i = 0; i < physicalDevices.size(); ++i)
		{
			VkPhysicalDeviceProperties2 candidateProps{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			vkGetPhysicalDeviceProperties2(physicalDevices[i], &candidateProps);
			if (candidateProps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				selectedIndex = i;
				break;
			}
		}

		m_physicalDevice = physicalDevices[selectedIndex];
		vkGetPhysicalDeviceProperties2(m_physicalDevice, &m_vkProperties2);

		m_physicalDeviceInfo.deviceName = m_vkProperties2.properties.deviceName;
		m_physicalDeviceInfo.nativeApiVersion = m_vkProperties2.properties.apiVersion;
		m_physicalDeviceInfo.driverVersion = m_vkProperties2.properties.driverVersion;
		m_physicalDeviceInfo.vendorId = m_vkProperties2.properties.vendorID;
		m_physicalDeviceInfo.deviceId = m_vkProperties2.properties.deviceID;
		m_physicalDeviceInfo.deviceType = static_cast<uint32_t>(m_vkProperties2.properties.deviceType);

#ifdef VK_API_VERSION_1_4
		ensure(m_physicalDeviceInfo.nativeApiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0),
		       "Require Vulkan 1.4 physical device");
#else
		ensure(m_physicalDeviceInfo.nativeApiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0),
		       "Require Vulkan 1.3 physical device");
#endif

		queryDeviceExtensions();
		queryMemoryProperties();
		selectQueues();
	}

	void VulkanDevice::initLogicalDevice()
	{
		ensure(m_physicalDevice != VK_NULL_HANDLE, "VulkanDevice::initLogicalDevice missing physical device");

		std::vector<uint32_t> queueFamilyIndices;
		queueFamilyIndices.push_back(m_graphicsQueue.familyIndex);
		if (m_computeQueue.familyIndex != m_graphicsQueue.familyIndex)
		{
			queueFamilyIndices.push_back(m_computeQueue.familyIndex);
		}
		if (m_transferQueue.familyIndex != m_graphicsQueue.familyIndex && m_transferQueue.familyIndex != m_computeQueue.
			familyIndex)
		{
			queueFamilyIndices.push_back(m_transferQueue.familyIndex);
		}

		const float queuePriority = 1.0f;
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		queueCreateInfos.reserve(queueFamilyIndices.size());
		for (uint32_t familyIndex : queueFamilyIndices)
		{
			queueCreateInfos.push_back(VkDeviceQueueCreateInfo{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = familyIndex,
				.queueCount = 1,
				.pQueuePriorities = &queuePriority,
			});
		}

		m_deviceFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		m_features11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		m_features12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceVulkan12Features supportedFeatures12{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
		};
		m_features13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
		m_features14 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#else
		m_maintenance5Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
		m_maintenance6Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
#endif

		m_enabledDeviceExtensions.clear();

		VkBaseOutStructure* featureChain = nullptr;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
		appendFeatureNode(featureChain, &m_features14);
#else
		if (extensionAvailable(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			pushUnique(m_enabledDeviceExtensions, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
			appendFeatureNode(featureChain, &m_maintenance5Features);
		}
		if (extensionAvailable(VK_KHR_MAINTENANCE_6_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			pushUnique(m_enabledDeviceExtensions, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
			appendFeatureNode(featureChain, &m_maintenance6Features);
		}
#endif
		appendFeatureNode(featureChain, &m_features13);
		appendFeatureNode(featureChain, &supportedFeatures12);
		appendFeatureNode(featureChain, &m_features11);

		for (const ExtensionRequest& request : m_createInfo.deviceExtensions)
		{
			const bool supported = extensionAvailable(request.name, m_availableDeviceExtensions);
			if (supported)
			{
				pushUnique(m_enabledDeviceExtensions, request.name);
				appendFeatureNode(featureChain, request.featuresStruct);
			}
			else if (request.required)
			{
				ensure(false, "Required device extension is not supported");
			}
		}

		m_deviceFeatures.pNext = featureChain;
		vkGetPhysicalDeviceFeatures2(m_physicalDevice, &m_deviceFeatures);

		// Keep queried support separate from the feature struct passed to
		// vkCreateDevice. Unsupported descriptor-indexing bits remain VK_FALSE.
		m_features12 = supportedFeatures12;

		featureChain = nullptr;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
		appendFeatureNode(featureChain, &m_features14);
#else
		if (extensionAvailable(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			appendFeatureNode(featureChain, &m_maintenance5Features);
		}
		if (extensionAvailable(VK_KHR_MAINTENANCE_6_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			appendFeatureNode(featureChain, &m_maintenance6Features);
		}
#endif
		appendFeatureNode(featureChain, &m_features13);
		appendFeatureNode(featureChain, &m_features12);
		appendFeatureNode(featureChain, &m_features11);
		for (const ExtensionRequest& request : m_createInfo.deviceExtensions)
		{
			if (extensionAvailable(request.name, m_availableDeviceExtensions))
			{
				appendFeatureNode(featureChain, request.featuresStruct);
			}
		}
		m_featuresChainHead = featureChain;
		m_deviceFeatures.pNext = m_featuresChainHead;

		m_featureInfo.timelineSemaphore = m_features12.timelineSemaphore == VK_TRUE;
		m_featureInfo.synchronization2 = m_features13.synchronization2 == VK_TRUE;
		m_featureInfo.dynamicRendering = m_features13.dynamicRendering == VK_TRUE;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
		m_featureInfo.maintenance5 = m_features14.maintenance5 == VK_TRUE;
		m_featureInfo.maintenance6 = m_features14.maintenance6 == VK_TRUE;
#else
		m_featureInfo.maintenance5 = m_maintenance5Features.maintenance5 == VK_TRUE;
		m_featureInfo.maintenance6 = m_maintenance6Features.maintenance6 == VK_TRUE;
#endif

		detectCapabilities();
		m_capabilityError = validateCapabilities();
		ensure(m_capabilityError == RHICapabilityError::None, toString(m_capabilityError));

		const VkDeviceCreateInfo deviceCreateInfo{
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &m_deviceFeatures,
			.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
			.pQueueCreateInfos = queueCreateInfos.data(),
			.enabledExtensionCount = static_cast<uint32_t>(m_enabledDeviceExtensions.size()),
			.ppEnabledExtensionNames = m_enabledDeviceExtensions.data(),
		};

		checkVk(vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device),
		        "Failed to create logical device");

		if (m_graphicsQueue.familyIndex != ~0u)
		{
			vkGetDeviceQueue(m_device, m_graphicsQueue.familyIndex, m_graphicsQueue.queueIndex, &m_graphicsQueue.queue);
		}
		if (m_computeQueue.familyIndex != ~0u)
		{
			vkGetDeviceQueue(m_device, m_computeQueue.familyIndex, m_computeQueue.queueIndex, &m_computeQueue.queue);
		}
		if (m_transferQueue.familyIndex != ~0u)
		{
			vkGetDeviceQueue(m_device, m_transferQueue.familyIndex, m_transferQueue.queueIndex, &m_transferQueue.queue);
		}
	}

	void VulkanDevice::initDebugMessenger()
	{
		if (!m_createInfo.base.enableValidationLayers || vkCreateDebugUtilsMessengerEXT == nullptr)
		{
			return;
		}

		const VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
			.pfnUserCallback = VulkanDevice::debugCallback,
			.pUserData = nullptr,
		};

		checkVk(vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_debugMessenger),
		        "Failed to create debug messenger");
	}

	void VulkanDevice::destroyDebugMessenger()
	{
		if (m_debugMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
		{
			vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
			m_debugMessenger = VK_NULL_HANDLE;
		}
	}

	void VulkanDevice::queryInstanceExtensions()
	{
		uint32_t count = 0;
		checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
		        "Failed querying instance extension count");
		m_availableInstanceExtensions.resize(count);
		checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, m_availableInstanceExtensions.data()),
		        "Failed querying instance extensions");
	}

	void VulkanDevice::queryInstanceLayers()
	{
		uint32_t count = 0;
		checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr), "Failed querying instance layer count");
		m_availableInstanceLayers.resize(count);
		checkVk(vkEnumerateInstanceLayerProperties(&count, m_availableInstanceLayers.data()),
		        "Failed querying instance layers");
	}

	void VulkanDevice::queryDeviceExtensions()
	{
		uint32_t count = 0;
		checkVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr),
		        "Failed querying device extension count");
		m_availableDeviceExtensions.resize(count);
		checkVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count,
		                                             m_availableDeviceExtensions.data()),
		        "Failed querying device extensions");
	}

	void VulkanDevice::queryMemoryProperties()
	{
		vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_vkMemoryProperties);

		m_memoryProperties.memoryTypes.clear();
		m_memoryProperties.memoryHeaps.clear();

		m_memoryProperties.memoryTypes.reserve(m_vkMemoryProperties.memoryTypeCount);
		for (uint32_t i = 0; i < m_vkMemoryProperties.memoryTypeCount; ++i)
		{
			m_memoryProperties.memoryTypes.push_back(MemoryTypeInfo{
				.propertyFlags = static_cast<uint32_t>(m_vkMemoryProperties.memoryTypes[i].propertyFlags),
				.heapIndex = m_vkMemoryProperties.memoryTypes[i].heapIndex,
			});
		}

		m_memoryProperties.memoryHeaps.reserve(m_vkMemoryProperties.memoryHeapCount);
		for (uint32_t i = 0; i < m_vkMemoryProperties.memoryHeapCount; ++i)
		{
			m_memoryProperties.memoryHeaps.push_back(MemoryHeapInfo{
				.size = static_cast<uint64_t>(m_vkMemoryProperties.memoryHeaps[i].size),
				.flags = static_cast<uint32_t>(m_vkMemoryProperties.memoryHeaps[i].flags),
			});
		}
	}

	void VulkanDevice::selectQueues()
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
		ensure(queueFamilyCount > 0, "No queue family found for selected physical device");

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

		auto findQueueFamily = [&](VkQueueFlags required, VkQueueFlags preferredAbsent) -> uint32_t
		{
			uint32_t firstMatch = ~0u;
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				if ((queueFamilies[i].queueFlags & required) == 0 || queueFamilies[i].queueCount == 0)
				{
					continue;
				}
				if ((queueFamilies[i].queueFlags & preferredAbsent) == 0)
				{
					return i;
				}
				if (firstMatch == ~0u)
				{
					firstMatch = i;
				}
			}
			return firstMatch;
		};

		const uint32_t graphicsFamily = findQueueFamily(VK_QUEUE_GRAPHICS_BIT, 0);
		ASSERT(graphicsFamily != ~0u, "No graphics queue family available");

		const uint32_t computeFamily = findQueueFamily(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
		const uint32_t transferFamily = findQueueFamily(VK_QUEUE_TRANSFER_BIT,
		                                                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

		m_graphicsQueue.familyIndex = graphicsFamily;
		m_graphicsQueue.queueCount = queueFamilies[graphicsFamily].queueCount;
		m_graphicsQueue.queueIndex = 0;

		const uint32_t computeFallback = computeFamily == ~0u ? graphicsFamily : computeFamily;
		m_computeQueue.familyIndex = computeFallback;
		m_computeQueue.queueCount = queueFamilies[computeFallback].queueCount;
		m_computeQueue.queueIndex = 0;

		const uint32_t transferFallback = transferFamily == ~0u ? computeFallback : transferFamily;
		m_transferQueue.familyIndex = transferFallback;
		m_transferQueue.queueCount = queueFamilies[transferFallback].queueCount;
		m_transferQueue.queueIndex = 0;
	}

	void VulkanDevice::detectCapabilities()
	{
		m_capabilities = {};

		m_capabilities.explicitResidency = ExplicitResidencyLevel::validatedNoOp;

		m_capabilities.coreGraphics = m_graphicsQueue.familyIndex != ~0u;
		m_capabilities.coreCompute = m_computeQueue.familyIndex != ~0u;

		const bool bindlessDescriptorIndexing = m_features12.descriptorIndexing == VK_TRUE;
		const bool bindlessRuntimeArray = m_features12.runtimeDescriptorArray == VK_TRUE;
		const bool bindlessPartiallyBound = m_features12.descriptorBindingPartiallyBound == VK_TRUE;
		const bool bindlessNonUniformSampling = m_features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
		const bool coreFrameFeatures = m_featureInfo.timelineSemaphore && m_featureInfo.synchronization2 &&
			m_featureInfo.dynamicRendering
			&& m_featureInfo.maintenance5 && m_featureInfo.maintenance6;
		m_capabilities.coreBindless = bindlessDescriptorIndexing && bindlessRuntimeArray
			&& bindlessPartiallyBound && bindlessNonUniformSampling && coreFrameFeatures;

		m_capabilities.extensionAsyncCompute =
			m_computeQueue.familyIndex != ~0u && m_computeQueue.familyIndex != m_graphicsQueue.familyIndex;

		VkPhysicalDeviceFeatures2 probeFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		m_meshShaderFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
		m_accelerationStructureFeatures = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
		};
		m_rayTracingPipelineFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

		VkBaseOutStructure* probeChain = nullptr;
		if (extensionAvailable(VK_EXT_MESH_SHADER_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			appendFeatureNode(probeChain, &m_meshShaderFeatures);
		}
		if (extensionAvailable(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, m_availableDeviceExtensions)
			&& extensionAvailable(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, m_availableDeviceExtensions))
		{
			appendFeatureNode(probeChain, &m_accelerationStructureFeatures);
			appendFeatureNode(probeChain, &m_rayTracingPipelineFeatures);
		}

		probeFeatures.pNext = probeChain;
		vkGetPhysicalDeviceFeatures2(m_physicalDevice, &probeFeatures);

		m_capabilities.extensionMeshShader = extensionAvailable(
				VK_EXT_MESH_SHADER_EXTENSION_NAME, m_availableDeviceExtensions)
			&& m_meshShaderFeatures.meshShader == VK_TRUE;
		m_capabilities.extensionRayTracing =
			extensionAvailable(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, m_availableDeviceExtensions)
			&& extensionAvailable(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, m_availableDeviceExtensions)
			&& m_accelerationStructureFeatures.accelerationStructure == VK_TRUE
			&& m_rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;
	}

	RHICapabilityError VulkanDevice::validateCapabilities() const
	{
		return evaluateCapabilityRequirements(m_capabilities, m_createInfo.base.capabilityRequirements);
	}

	bool VulkanDevice::extensionAvailable(const char* name, const std::vector<VkExtensionProperties>& extensions)
	{
		if (name == nullptr)
		{
			return false;
		}

		for (const VkExtensionProperties& ext : extensions)
		{
			if (std::strcmp(name, ext.extensionName) == 0)
			{
				return true;
			}
		}
		return false;
	}

	bool VulkanDevice::layerAvailable(const char* name, const std::vector<VkLayerProperties>& layers)
	{
		if (name == nullptr)
		{
			return false;
		}

		for (const VkLayerProperties& layer : layers)
		{
			if (std::strcmp(name, layer.layerName) == 0)
			{
				return true;
			}
		}
		return false;
	}

	void VulkanDevice::appendFeatureNode(VkBaseOutStructure*& chainHead, void* featureStruct)
	{
		if (featureStruct == nullptr)
		{
			return;
		}

		VkBaseOutStructure* node = reinterpret_cast<VkBaseOutStructure*>(featureStruct);
		node->pNext = chainHead;
		chainHead = node;
	}

	VkBool32 VulkanDevice::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	                                     VkDebugUtilsMessageTypeFlagsEXT type,
	                                     const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
	                                     void*)
	{
		const char* severityName = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0
			                           ? "ERROR"
			                           : ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0
				                              ? "WARNING"
				                              : "INFO");
		const char* typeName = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0
			                       ? "VALIDATION"
			                       : ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0
				                          ? "PERFORMANCE"
				                          : "GENERAL");
		LOGE("Vulkan validation %s/%s: %s",
		     severityName,
		     typeName,
		     callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "<null>");
		if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
#if defined(_MSC_VER)
			__debugbreak();
#elif defined(__linux__) || defined(__ANDROID__)
			raise(SIGTRAP);
#endif
		}
		return VK_FALSE;
	}



	TextureViewHandle VulkanDevice::createTextureView(const TextureViewCreateDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateTextureView");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::textureViewCreations);
		assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createTextureView");
		const VkImage nativeImage = resolveTexture(desc.image);
		const VkImageViewCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = nativeImage,
			.viewType = toVkImageViewType(desc.viewType),
			.format = toVkViewFormat(desc.format),
			.components = {
				toVkSwizzle(desc.components.r), toVkSwizzle(desc.components.g), toVkSwizzle(desc.components.b),
				toVkSwizzle(desc.components.a)
			},
			.subresourceRange = {
				.aspectMask = toVkImageAspect(desc.aspect),
				.baseMipLevel = desc.baseMipLevel,
				.levelCount = desc.levelCount,
				.baseArrayLayer = desc.baseArrayLayer,
				.layerCount = desc.layerCount
			},
		};
		VkImageView view = VK_NULL_HANDLE;
		VK_CHECK(vkCreateImageView(m_device, &info, nullptr, &view));
		if (desc.debugName != nullptr)
		{
			utils::DebugUtil::getInstance().setObjectName(view, desc.debugName);
		}
		TextureViewCreateDesc coldDesc = desc;
		coldDesc.debugName = nullptr;
		return m_resourceTable->registerTextureView(
			view,
			TextureViewColdRecord{
				.desc = coldDesc,
				.parentTexture = desc.image,
				.debugName = desc.debugName != nullptr ? desc.debugName : "",
				.owned = true,
			});
	}

	void VulkanDevice::destroyTextureView(TextureViewHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const TextureViewRecord record = m_resourceTable->removeTextureView(handle);
		if (record.cold.owned && record.hot.nativeView != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::TextureView, handle.index, handle.generation},
				.retirementDependencies = record.hot.pendingUses,
				.nativeObject = asNativeU64(record.hot.nativeView),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}


	namespace
	{
		[[nodiscard]] VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage);
	} // namespace

	TextureHandle VulkanDevice::createTexture(const TextureDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateTexture");
		assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createTexture");
		assert(m_allocator != nullptr && "VulkanDevice::setAllocator must be called before createTexture");
		assert(desc.extent.width > 0 && desc.extent.height > 0 && desc.extent.depth > 0);
		assert(desc.mipLevels > 0);
		assert(desc.arrayLayers > 0);

		const VkImageCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.flags = toVkImageCreateFlags(desc.dimension),
			.imageType = toVkImageType(desc.dimension),
			.format = toVkViewFormat(desc.format),
			.extent = {
				.width = desc.extent.width,
				.height = desc.extent.height,
				.depth = desc.dimension == TextureDimension::e3D ? desc.extent.depth : 1u
			},
			.mipLevels = desc.mipLevels,
			.arrayLayers = desc.dimension == TextureDimension::e3D ? 1u : desc.arrayLayers,
			.samples = toVkSampleCount(desc.sampleCount),
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = toVkImageUsage(desc.usage),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = nullptr;
		VmaAllocationCreateInfo allocInfo{.usage = toVmaMemoryUsage(desc.memoryUsage)};
		VK_CHECK(vmaCreateImage(m_allocator, &info, &allocInfo, &image, &allocation, nullptr));
		if (desc.debugName != nullptr)
		{
			utils::DebugUtil::getInstance().setObjectName(image, desc.debugName);
		}

		TextureDesc coldDesc = desc;
		coldDesc.debugName = nullptr;
		return m_resourceTable->registerTexture(
			image,
			TextureColdRecord{
				.nativeAllocation = allocation,
				.desc = coldDesc,
				.debugName = desc.debugName != nullptr ? desc.debugName : "",
				.owned = true,
			});
	}

	void VulkanDevice::destroyTexture(TextureHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const TextureRecord record = m_resourceTable->removeTexture(handle);
		if (record.cold.owned && record.hot.nativeImage != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Texture, handle.index, handle.generation},
				.retirementDependencies = record.hot.pendingUses,
				.nativeObject = asNativeU64(record.hot.nativeImage),
				.nativeAllocation = asNativeU64(record.cold.nativeAllocation),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}


	namespace
	{
		[[nodiscard]] VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage)
		{
			switch (usage)
			{
			case MemoryUsage::cpuToGpu: return VMA_MEMORY_USAGE_CPU_TO_GPU;
			case MemoryUsage::gpuToCpu: return VMA_MEMORY_USAGE_GPU_TO_CPU;
			case MemoryUsage::transientAttachment: return VMA_MEMORY_USAGE_GPU_ONLY;
			case MemoryUsage::gpuOnly: return VMA_MEMORY_USAGE_GPU_ONLY;
			}
			return VMA_MEMORY_USAGE_UNKNOWN;
		}

		[[nodiscard]] bool isCpuVisible(MemoryUsage usage)
		{
			return usage == MemoryUsage::cpuToGpu || usage == MemoryUsage::gpuToCpu;
		}
	} // namespace

	BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateBuffer");
		assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createBuffer");
		assert(m_allocator != nullptr && "VulkanDevice::setAllocator must be called before createBuffer");

		const VkBufferCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = desc.size,
			.usage = toVkBufferUsage(desc.usage, desc.allowGpuAddress, desc.allowIndirectArgument),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		VmaAllocationCreateInfo allocInfo{.usage = toVmaMemoryUsage(desc.memoryUsage)};
		if (isCpuVisible(desc.memoryUsage))
		{
			allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
			// 回读路径（gpuCulling stats/results 等）依赖 coherent 内存，以便 CPU 直接
			// 读取 GPU 写入结果而无需 vmaInvalidateAllocation。Vulkan 规范保证每个实现
			// 均存在 HOST_VISIBLE|HOST_COHERENT 内存类型，此 requiredFlags 始终可满足。
			allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = nullptr;
		VmaAllocationInfo allocResult{};
		VK_CHECK(vmaCreateBuffer(m_allocator, &info, &allocInfo, &buffer, &allocation, &allocResult));
		if (desc.debugName != nullptr)
		{
			utils::DebugUtil::getInstance().setObjectName(buffer, desc.debugName);
		}

		uint64_t gpuAddress = 0;
		if (desc.allowGpuAddress || static_cast<uint32_t>(desc.usage & BufferUsageFlags::shaderDeviceAddress) != 0)
		{
			const VkBufferDeviceAddressInfo addressInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer
			};
			gpuAddress = vkGetBufferDeviceAddress(m_device, &addressInfo);
		}

		BufferDesc coldDesc = desc;
		coldDesc.debugName = nullptr;
		return m_resourceTable->registerBuffer(
			BufferHotRecord{
				.nativeBuffer = buffer,
				.gpuAddress = gpuAddress,
				.mapped = allocResult.pMappedData,
			},
			BufferColdRecord{
				.nativeAllocation = allocation,
				.desc = coldDesc,
				.debugName = desc.debugName != nullptr ? desc.debugName : "",
				.hostCoherent = isCpuVisible(desc.memoryUsage),
				.owned = true,
			});
	}

	void VulkanDevice::destroyBuffer(BufferHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const BufferRecord record = m_resourceTable->removeBuffer(handle);
		if (record.cold.owned && record.hot.nativeBuffer != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Buffer, handle.index, handle.generation},
				.retirementDependencies = record.hot.pendingUses,
				.nativeObject = asNativeU64(record.hot.nativeBuffer),
				.nativeAllocation = asNativeU64(record.cold.nativeAllocation),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}

	GpuPtr VulkanDevice::getBufferGpuAddress(BufferHandle handle) const
	{
		const BufferHotRecord* record =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferHot(handle) : nullptr;
		return record != nullptr ? GpuPtr{record->gpuAddress} : GpuPtr{};
	}

	void* VulkanDevice::mapBuffer(BufferHandle handle)
	{
		const BufferHotRecord* record =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferHot(handle) : nullptr;
		return record != nullptr ? record->mapped : nullptr;
	}

	void VulkanDevice::unmapBuffer(BufferHandle)
	{
		// Buffers use persistent mapping (VMA_ALLOCATION_CREATE_MAPPED_BIT); nothing to unmap.
	}

	Result<MappedBufferRange> VulkanDevice::mapBufferRange(
		BufferHandle handle, const BufferMapDesc& desc)
	{
		const BufferHotRecord* hot =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferHot(handle) : nullptr;
		const BufferColdRecord* cold =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferCold(handle) : nullptr;
		if (hot == nullptr || cold == nullptr)
			return Result<MappedBufferRange>::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
		if (hot->mapped == nullptr || cold->desc.memoryUsage == MemoryUsage::gpuOnly ||
		    cold->desc.memoryUsage == MemoryUsage::transientAttachment)
			return Result<MappedBufferRange>::fail(RHIErrorCode::invalidState, "Buffer is not CPU visible");
		if (desc.offset > cold->desc.size)
			return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument, "Buffer map offset is out of range");
		const uint64_t size =
			desc.size == 0 ? cold->desc.size - desc.offset : desc.size;
		if (size > cold->desc.size - desc.offset)
			return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument, "Buffer map size is out of range");
		if (desc.invalidateBeforeRead && desc.mode != BufferMapMode::write)
		{
			if (vmaInvalidateAllocation(
			      m_allocator, cold->nativeAllocation, desc.offset, size) != VK_SUCCESS)
				return Result<MappedBufferRange>::fail(RHIErrorCode::backendFailure, "Buffer invalidate failed");
		}
		return Result<MappedBufferRange>::ok(MappedBufferRange{
			.data = static_cast<char*>(hot->mapped) + desc.offset,
			.offset = desc.offset,
			.size = size,
			.coherent = cold->hostCoherent,
			.persistent = true,
		});
	}

	RHIResult VulkanDevice::flushMappedBufferRange(
		BufferHandle handle, uint64_t offset, uint64_t size)
	{
		const BufferHotRecord* hot =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferHot(handle) : nullptr;
		const BufferColdRecord* cold =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferCold(handle) : nullptr;
		if (hot == nullptr || cold == nullptr)
			return RHIResult::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
		if (hot->mapped == nullptr || offset > cold->desc.size ||
		    (size != 0 && size > cold->desc.size - offset))
			return RHIResult::fail(RHIErrorCode::invalidArgument, "Mapped flush range is invalid");
		const uint64_t flushSize =
			size == 0 ? cold->desc.size - offset : size;
		if (vmaFlushAllocation(
		      m_allocator, cold->nativeAllocation, offset, flushSize) != VK_SUCCESS)
			return RHIResult::fail(RHIErrorCode::backendFailure, "Buffer flush failed");
		return RHIResult::ok();
	}

	RHIResult VulkanDevice::invalidateMappedBufferRange(
		BufferHandle handle, uint64_t offset, uint64_t size)
	{
		const BufferHotRecord* hot =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferHot(handle) : nullptr;
		const BufferColdRecord* cold =
			m_resourceTable != nullptr ? m_resourceTable->tryGetBufferCold(handle) : nullptr;
		if (hot == nullptr || cold == nullptr)
			return RHIResult::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
		if (hot->mapped == nullptr || offset > cold->desc.size ||
		    (size != 0 && size > cold->desc.size - offset))
			return RHIResult::fail(RHIErrorCode::invalidArgument, "Mapped invalidate range is invalid");
		const uint64_t invalidateSize =
			size == 0 ? cold->desc.size - offset : size;
		if (vmaInvalidateAllocation(
		      m_allocator, cold->nativeAllocation, offset, invalidateSize) != VK_SUCCESS)
			return RHIResult::fail(RHIErrorCode::backendFailure, "Buffer invalidate failed");
		return RHIResult::ok();
	}

	SamplerHandle VulkanDevice::createSampler(const SamplerDesc& desc)
	{
		assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createSampler");
		const VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = static_cast<VkFilter>(desc.magFilter),
			.minFilter = static_cast<VkFilter>(desc.minFilter),
			.mipmapMode = static_cast<VkSamplerMipmapMode>(desc.mipmapMode),
			.addressModeU = toVkAddressMode(desc.addressModeU),
			.addressModeV = toVkAddressMode(desc.addressModeV),
			.addressModeW = toVkAddressMode(desc.addressModeW),
			.mipLodBias = desc.mipLodBias,
			.anisotropyEnable = desc.anisotropyEnable ? VK_TRUE : VK_FALSE,
			.maxAnisotropy = desc.maxAnisotropy,
			.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE,
			.compareOp = static_cast<VkCompareOp>(desc.compareOp),
			.minLod = desc.minLod,
			.maxLod = desc.maxLod,
		};
		VkSampler sampler = VK_NULL_HANDLE;
		VK_CHECK(vkCreateSampler(m_device, &info, nullptr, &sampler));
		if (desc.debugName != nullptr)
		{
			utils::DebugUtil::getInstance().setObjectName(sampler, desc.debugName);
		}
		return m_resourceTable->registerSampler(sampler);
	}

	void VulkanDevice::destroySampler(SamplerHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const SamplerRecord record = m_resourceTable->removeSampler(handle);
		if (record.nativeSampler != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Sampler, handle.index, handle.generation},
				.retirementDependencies = record.pendingUses,
				.nativeObject = asNativeU64(record.nativeSampler),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}

	// -------------------------------------------------------------------------
	// VulkanDeviceInterop typed resolve accessors (D-07)
	// -------------------------------------------------------------------------

	VkImage VulkanDevice::resolveTexture(rhi::TextureHandle handle) const
	{
		return m_resourceTable != nullptr ? m_resourceTable->resolveTexture(handle) : VK_NULL_HANDLE;
	}

	VkImageView VulkanDevice::resolveTextureView(rhi::TextureViewHandle handle) const
	{
		return m_resourceTable != nullptr ? m_resourceTable->resolveTextureView(handle) : VK_NULL_HANDLE;
	}

	VkSampler VulkanDevice::resolveSampler(rhi::SamplerHandle handle) const
	{
		return m_resourceTable != nullptr ? m_resourceTable->resolveSampler(handle) : VK_NULL_HANDLE;
	}

	QueryPoolHandle VulkanDevice::createQueryPool(uint32_t queryCount)
	{
		assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createQueryPool");
		const VkQueryPoolCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = queryCount,
		};
		VkQueryPool pool = VK_NULL_HANDLE;
		VK_CHECK(vkCreateQueryPool(m_device, &info, nullptr, &pool));
		return m_resourceTable->registerQueryPool(pool, queryCount);
	}

	void VulkanDevice::destroyQueryPool(QueryPoolHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const QueryPoolRecord record = m_resourceTable->removeQueryPool(handle);
		if (record.nativePool != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::QueryPool, handle.index, handle.generation},
				.retirementDependencies = record.pendingUses,
				.nativeObject = asNativeU64(record.nativePool),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}

	uint64_t VulkanDevice::getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex)
	{
		if (m_resourceTable == nullptr)
		{
			return 0;
		}
		const VkQueryPool nativePool = m_resourceTable->resolveQueryPool(handle);
		if (nativePool == VK_NULL_HANDLE)
		{
			return 0;
		}
		uint64_t result = 0;
		vkGetQueryPoolResults(m_device, nativePool, queryIndex,
		                      1,
		                      sizeof(result), &result, sizeof(result),
		                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		return result;
	}

	bool VulkanDevice::getQueryPoolResultsWithAvailability(
		QueryPoolHandle handle, uint32_t firstQuery,
		std::span<uint64_t> outValueAvailabilityPairs)
	{
		if (m_resourceTable == nullptr || outValueAvailabilityPairs.empty()
		    || (outValueAvailabilityPairs.size() % 2u) != 0)
		{
			return false;
		}
		const VkQueryPool nativePool = m_resourceTable->resolveQueryPool(handle);
		if (nativePool == VK_NULL_HANDLE)
		{
			return false;
		}
		const uint32_t queryCount =
			static_cast<uint32_t>(outValueAvailabilityPairs.size() / 2u);
		const VkResult result = vkGetQueryPoolResults(
			m_device, nativePool, firstQuery, queryCount,
			outValueAvailabilityPairs.size_bytes(), outValueAvailabilityPairs.data(),
			sizeof(uint64_t) * 2u,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		return result == VK_SUCCESS || result == VK_NOT_READY;
	}



	ArgumentLayoutHandle VulkanDevice::createArgumentLayout(const ArgumentLayoutDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateArgumentLayout");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::argumentLayoutCreations);
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentLayout");
		std::vector<VkDescriptorSetLayoutBinding> bindings(static_cast<uint32_t>(desc.bindings.size()));
		std::vector<VkDescriptorBindingFlags> bindingFlags(static_cast<uint32_t>(desc.bindings.size()), 0);
		std::vector<uint32_t> dynamicBindings;
		bool hasBindingFlags = false;
		for (uint32_t i = 0; i < static_cast<uint32_t>(desc.bindings.size()); ++i)
		{
			const ArgumentBinding& b = desc.bindings[i];
			bindings[i] = VkDescriptorSetLayoutBinding{
				.binding = b.binding,
				.descriptorType = toVkDescriptorType(b.type, b.dynamicOffset),
				.descriptorCount = b.arrayCount,
				.stageFlags = toVkShaderStageFlags(b.visibility),
			};
			if (b.dynamicOffset)
			{
				dynamicBindings.push_back(b.binding);
			}
			if (b.bindless)
			{
				ensure(b.arrayCount > 1, "Bindless argument binding requires an array");
				ensure(
					m_features12.descriptorBindingPartiallyBound == VK_TRUE,
					"Bindless argument arrays require descriptorBindingPartiallyBound to be enabled");
				bindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
				hasBindingFlags = true;
			}
		}
		VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
		if (hasBindingFlags)
		{
			flagsInfo = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
				.bindingCount = static_cast<uint32_t>(bindingFlags.size()),
				.pBindingFlags = bindingFlags.data(),
			};
		}
		const VkDescriptorSetLayoutCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = hasBindingFlags ? &flagsInfo : nullptr,
			.flags = 0,
			.bindingCount = static_cast<uint32_t>(bindings.size()),
			.pBindings = bindings.empty() ? nullptr : bindings.data(),
		};
		VkDescriptorSetLayoutSupport layoutSupport{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT
		};
		vkGetDescriptorSetLayoutSupport(m_device, &info, &layoutSupport);
		ensure(layoutSupport.supported == VK_TRUE, "Argument layout exceeds Vulkan descriptor limits");
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VK_CHECK(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout));
		return m_resourceTable->registerArgumentLayout(layout, std::move(dynamicBindings));
	}

	void VulkanDevice::destroyArgumentLayout(ArgumentLayoutHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const ArgumentLayoutRecord record = m_resourceTable->removeArgumentLayout(handle);
		if (record.nativeLayout != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::ArgumentLayout, handle.index, handle.generation},
				.nativeObject = asNativeU64(record.nativeLayout),
				.owned = true,
			});
		}
	}

	ArgumentTableHandle VulkanDevice::createArgumentTable(const ArgumentTableCreateDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateArgumentTable");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::descriptorAllocations);
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentTable");
		const std::array<VkDescriptorPoolSize, 8> sizes{
			{
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
				{VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_combinedImageSamplerPoolCapacity},
			}
		};
		const VkDescriptorPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = 4096,
			.poolSizeCount = static_cast<uint32_t>(sizes.size()),
			.pPoolSizes = sizes.data(),
		};

		if (m_argumentPool == VK_NULL_HANDLE)
		{
			VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_argumentPool));
			m_argumentPools.push_back(m_argumentPool);
		}

		const VkDescriptorSetLayout nativeLayout = m_resourceTable->resolveArgumentLayout(desc.layout);
		assert(nativeLayout != VK_NULL_HANDLE && "createArgumentTable requires a valid argument layout");
		VkDescriptorSetLayout setLayout = nativeLayout;
		VkDescriptorSetAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = m_argumentPool,
			.descriptorSetCount = 1,
			.pSetLayouts = &setLayout,
		};
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkResult allocationResult = vkAllocateDescriptorSets(m_device, &allocInfo, &set);
		if (allocationResult == VK_ERROR_OUT_OF_POOL_MEMORY || allocationResult == VK_ERROR_FRAGMENTED_POOL)
		{
			VkDescriptorPool retryPool = VK_NULL_HANDLE;
			VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &retryPool));
			m_argumentPools.push_back(retryPool);
			m_argumentPool = retryPool;
			allocInfo.descriptorPool = retryPool;
			allocationResult = vkAllocateDescriptorSets(m_device, &allocInfo, &set);
		}
		checkVk(allocationResult, "Failed to allocate argument descriptor set");
		m_argumentSetPools.emplace(asNativeU64(set), m_argumentPool);
		return m_resourceTable->registerArgumentTable(set, desc.layout, desc.lifetime);
	}

	void VulkanDevice::destroyArgumentTable(ArgumentTableHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const ArgumentTableRecord record = m_resourceTable->removeArgumentTable(handle);
		if (record.owned && record.nativeSet != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::ArgumentTable, handle.index, handle.generation},
				.retirementDependencies = record.pendingUses,
				.nativeObject = asNativeU64(record.nativeSet),
				.owned = true,
				.preciseDependencies = true,
			});
		}
	}

	void VulkanDevice::updateArgumentTable(ArgumentTableHandle table, ArgumentWriteBatch writes)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.UpdateArgumentTable");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::descriptorUpdates, writes.size());
		if (m_resourceTable == nullptr || writes.empty())
		{
			return;
		}
		const uint32_t writeCount = static_cast<uint32_t>(writes.size());
		ArgumentTableRecord* tableRecord = m_resourceTable->tryGetArgumentTableMutable(table);
		if (tableRecord == nullptr || tableRecord->nativeSet == VK_NULL_HANDLE)
			throw std::runtime_error("VulkanDevice::updateArgumentTable received a stale table");
		if (tableRecord->pendingUses.count != 0)
		{
			if (!isRetirementComplete(tableRecord->pendingUses))
				throw std::runtime_error(
					"VulkanDevice::updateArgumentTable rejected a table with pending GPU work");
			tableRecord->pendingUses = {};
		}
		VkDescriptorSet set = tableRecord->nativeSet;

		// Per-binding dynamic-ness comes from the layout: a write's descriptorType must
		// match the layout, so dynamic UBO/SSBO bindings need the *_DYNAMIC variant.
		const ArgumentLayoutRecord* layoutRecord = m_resourceTable->tryGetArgumentLayout(tableRecord->layout);
		const auto isDynamicBinding = [layoutRecord](uint32_t binding) -> bool
		{
			if (layoutRecord == nullptr)
			{
				return false;
			}
			for (uint32_t dyn : layoutRecord->dynamicBindings)
			{
				if (dyn == binding)
				{
					return true;
				}
			}
			return false;
		};

		std::vector<VkDescriptorBufferInfo> bufferInfos(writeCount);
		std::vector<VkDescriptorImageInfo> imageInfos(writeCount);
		std::vector<VkWriteDescriptorSet> vkWrites(writeCount);
		for (uint32_t i = 0; i < writeCount; ++i)
		{
			const ArgumentWrite& w = writes[i];
			const VkDescriptorType type = toVkDescriptorType(w.type, isDynamicBinding(w.binding));
			VkWriteDescriptorSet& out = vkWrites[i];
			out = VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = w.binding,
				.dstArrayElement = w.arrayElement,
				.descriptorCount = 1,
				.descriptorType = type,
			};
			switch (w.type)
			{
			case ArgumentType::sampledTexture:
			case ArgumentType::storageTexture:
				m_resourceTable->recordArgumentTableResource(
					table, w.binding, w.arrayElement, residencyResource(w.textureView));
				imageInfos[i] = VkDescriptorImageInfo{
					.imageView = m_resourceTable->resolveTextureView(w.textureView),
					.imageLayout = (w.type == ArgumentType::storageTexture
						               || w.accessIntent == ArgumentAccessIntent::readWrite)
						               ? VK_IMAGE_LAYOUT_GENERAL
						               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				out.pImageInfo = &imageInfos[i];
				break;
			case ArgumentType::sampler:
				m_resourceTable->recordArgumentTableResource(
					table, w.binding, w.arrayElement,
					ResidencyResource{
						ResidencyResourceKind::sampler,
						w.sampler.index,
						w.sampler.generation,
					});
				imageInfos[i] = VkDescriptorImageInfo{
					.sampler = m_resourceTable->resolveSampler(
						w.sampler)
				};
				out.pImageInfo = &imageInfos[i];
				break;
			case ArgumentType::combinedImageSampler:
				m_resourceTable->recordArgumentTableResource(
					table, w.binding, w.arrayElement, residencyResource(w.textureView));
				m_resourceTable->recordArgumentTableResource(
					table, w.binding, w.arrayElement,
					ResidencyResource{
						ResidencyResourceKind::sampler,
						w.sampler.index,
						w.sampler.generation,
					});
				imageInfos[i] = VkDescriptorImageInfo{
					.sampler = m_resourceTable->resolveSampler(
						w.sampler),
					.imageView = m_resourceTable->resolveTextureView(w.textureView),
					.imageLayout = (w.accessIntent == ArgumentAccessIntent::readWrite)
						               ? VK_IMAGE_LAYOUT_GENERAL
						               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				out.pImageInfo = &imageInfos[i];
				break;
			default: // buffer types
				m_resourceTable->recordArgumentTableResource(
					table, w.binding, w.arrayElement, residencyResource(w.buffer));
				bufferInfos[i] = VkDescriptorBufferInfo{
					.buffer = m_resourceTable->resolveBuffer(w.buffer),
					.offset = w.offset,
					.range = w.size == 0 ? VK_WHOLE_SIZE : w.size,
				};
				out.pBufferInfo = &bufferInfos[i];
				break;
			}
		}
		vkUpdateDescriptorSets(m_device, writeCount, vkWrites.data(), 0, nullptr);
	}
  ArgumentLayoutHandle VulkanDevice::getArgumentTableLayout(ArgumentTableHandle table) const
  {
    if(m_resourceTable == nullptr)
      return {};
    const ArgumentTableRecord* record = m_resourceTable->tryGetArgumentTable(table);
    return record != nullptr ? record->layout : ArgumentLayoutHandle{};
  }

	namespace
	{
		[[nodiscard]] uint32_t alignRootBindingOffset(uint32_t offset, uint32_t alignment)
		{
			const uint32_t safeAlignment = alignment == 0 ? 4u : alignment;
			return (offset + safeAlignment - 1u) & ~(safeAlignment - 1u);
		}

		struct VulkanPipelineLayoutBuildResult
		{
			VkPipelineLayout layout{VK_NULL_HANDLE};
			std::vector<PipelineRecord::RootBindingLowering> rootBindings;
		};

		VulkanPipelineLayoutBuildResult makePipelineLayoutFromPipelineDesc(
			VkDevice device, const VulkanResourceTable& resourceTable,
			const PipelineBindingSchemaDesc& bindingSchema)
		{
			const PipelineBindingSchemaValidationResult validation = validatePipelineBindingSchema(bindingSchema);
			ASSERT(validation.valid(), "Pipeline binding schema must be valid before Vulkan layout lowering");

			uint32_t maxSetSlot = 0;
			bool hasSetSlot = false;
			for (uint32_t i = 0; i < static_cast<uint32_t>(bindingSchema.argumentSlots.size()); ++i)
			{
				maxSetSlot = (std::max)(maxSetSlot, bindingSchema.argumentSlots[i].slot);
				hasSetSlot = true;
			}

			std::vector<VkDescriptorSetLayout> setLayouts(hasSetSlot ? maxSetSlot + 1u : 0u, VK_NULL_HANDLE);
			for (uint32_t i = 0; i < static_cast<uint32_t>(bindingSchema.argumentSlots.size()); ++i)
			{
				const PipelineArgumentSlotDesc& slot = bindingSchema.argumentSlots[i];
				const VkDescriptorSetLayout nativeLayout = resourceTable.resolveArgumentLayout(slot.layout);
				ASSERT(nativeLayout != VK_NULL_HANDLE,
				       "Pipeline argument slot layout must resolve to a native descriptor set layout");
				setLayouts[slot.slot] = nativeLayout;
			}

			while (!setLayouts.empty() && setLayouts.back() == VK_NULL_HANDLE)
			{
				setLayouts.pop_back();
			}			std::vector<VkPushConstantRange> vkPushConstants;
			std::vector<PipelineRecord::RootBindingLowering> rootLowering;
			vkPushConstants.reserve(static_cast<uint32_t>(bindingSchema.rootBindings.size()));
			rootLowering.reserve(static_cast<uint32_t>(bindingSchema.rootBindings.size()));
			uint32_t nextOffset = 0;
			for (uint32_t i = 0; i < static_cast<uint32_t>(bindingSchema.rootBindings.size()); ++i)
			{
				const RootBindingDesc& binding = bindingSchema.rootBindings[i];
				if (binding.kind == RootBindingKind::dynamicBuffer)
				{
					rootLowering.push_back(PipelineRecord::RootBindingLowering{
						.slot = binding.slot,
						.offset = 0,
						.size = 0,
						.kind = static_cast<uint32_t>(binding.kind),
						.stages = static_cast<uint32_t>(binding.visibility),
					});
					continue;
				}

				const uint32_t offset = alignRootBindingOffset(nextOffset, binding.alignment);
				vkPushConstants.push_back(VkPushConstantRange{
					.stageFlags = toVkShaderStageFlags(binding.visibility),
					.offset = offset,
					.size = binding.size,
				});
				rootLowering.push_back(PipelineRecord::RootBindingLowering{
					.slot = binding.slot,
					.offset = offset,
					.size = binding.size,
					.kind = static_cast<uint32_t>(binding.kind),
					.stages = static_cast<uint32_t>(binding.visibility),
				});
				nextOffset = offset + binding.size;
			}

			const VkPipelineLayoutCreateInfo createInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
				.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data(),
				.pushConstantRangeCount = static_cast<uint32_t>(vkPushConstants.size()),
				.pPushConstantRanges = vkPushConstants.empty() ? nullptr : vkPushConstants.data(),
			};
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VK_CHECK(vkCreatePipelineLayout(device, &createInfo, nullptr, &layout));
			return VulkanPipelineLayoutBuildResult{.layout = layout, .rootBindings = std::move(rootLowering)};
		}

		[[nodiscard]] VulkanPipelineLayoutBuildResult makePipelineLayoutFromGraphicsDesc(
			VkDevice device, const VulkanResourceTable& resourceTable, const GraphicsPipelineDesc& desc)
		{
			return makePipelineLayoutFromPipelineDesc(device,
			                                          resourceTable,
			                                          desc.bindingSchema);
		}

		[[nodiscard]] VulkanPipelineLayoutBuildResult makePipelineLayoutFromComputeDesc(
			VkDevice device, const VulkanResourceTable& resourceTable, const ComputePipelineDesc& desc)
		{
			return makePipelineLayoutFromPipelineDesc(device,
			                                          resourceTable,
			                                          desc.bindingSchema);
		}
	} // namespace

	ShaderLibraryHandle VulkanDevice::createShaderLibrary(const ShaderLibraryDesc& desc)
	{
		ensure(m_device != VK_NULL_HANDLE && m_resourceTable != nullptr,
		       "VulkanDevice::createShaderLibrary requires an initialized device");
		ensure(desc.format == ShaderIRFormat::spirv,
		       "Vulkan shader libraries require SPIR-V payloads");
		ensure(desc.data.size() >= sizeof(uint32_t)
		       && (desc.data.size() % sizeof(uint32_t)) == 0
		       && (reinterpret_cast<uintptr_t>(desc.data.data()) % alignof(uint32_t)) == 0,
		       "Vulkan shader libraries require non-empty word-aligned SPIR-V");

		const auto* words = reinterpret_cast<const uint32_t*>(desc.data.data());
		const VkShaderModule module = utils::createShaderModule(
			m_device, std::span<const uint32_t>{words, desc.data.size() / sizeof(uint32_t)});
		return m_resourceTable->registerShaderLibrary(module, desc.format);
	}

	void VulkanDevice::destroyShaderLibrary(ShaderLibraryHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const ShaderLibraryRecord record = m_resourceTable->removeShaderLibrary(handle);
		if (record.nativeModule != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(
				m_device,
				record.nativeModule,
				nullptr);
		}
	}

	PipelineHandle VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateGraphicsPipeline");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::pipelineCreations);
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::setResourceTable must be called before createGraphicsPipeline");

		VulkanPipelineLayoutBuildResult ownedLayout{};
		ownedLayout = makePipelineLayoutFromGraphicsDesc(m_device, *m_resourceTable, desc);
		VkPipelineLayout nativeLayout = ownedLayout.layout;

		std::vector<VkShaderModule> shaderModules;
		shaderModules.reserve(static_cast<uint32_t>(desc.shaderStages.size()));
		for (uint32_t stageIndex = 0; stageIndex < static_cast<uint32_t>(desc.shaderStages.size()); ++stageIndex)
		{
			const ShaderLibraryRecord* library =
				m_resourceTable->tryGetShaderLibrary(desc.shaderStages[stageIndex].library);
			ensure(library != nullptr && library->format == ShaderIRFormat::spirv
			       && library->nativeModule != VK_NULL_HANDLE,
			       "Vulkan graphics pipeline requires active SPIR-V shader libraries");
			shaderModules.push_back(
				library->nativeModule);
		}
		const GraphicsPipelineCreateInfo createInfo{
			.desc = &desc,
			.layout = nativeLayout,
			.shaderModules = shaderModules.data(),
			.shaderModuleCount = static_cast<uint32_t>(shaderModules.size()),
		};
		const VkPipeline pipeline = vulkan::createGraphicsPipeline(m_device, createInfo);
		const PipelineHandle handle = m_resourceTable->registerPipeline(
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline,
			desc.specializationVariant,
			nativeLayout,
			std::move(ownedLayout.rootBindings),
			true,
			true);
		return handle;
	}

	PipelineHandle VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc)
	{
		VKDEMO_CPU_SCOPE("RHI.Vulkan.Device.CreateComputePipeline");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::pipelineCreations);
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createComputePipeline");

		VulkanPipelineLayoutBuildResult ownedLayout{};
		ownedLayout = makePipelineLayoutFromComputeDesc(m_device, *m_resourceTable, desc);
		VkPipelineLayout nativeLayout = ownedLayout.layout;

		const ShaderLibraryRecord* library =
			m_resourceTable->tryGetShaderLibrary(desc.shaderStage.library);
		ensure(library != nullptr && library->format == ShaderIRFormat::spirv
		       && library->nativeModule != VK_NULL_HANDLE,
		       "Vulkan compute pipeline requires an active SPIR-V shader library");
		const ComputePipelineCreateInfo createInfo{
			.desc = &desc,
			.layout = nativeLayout,
			.pipelineFlags = desc.pipelineFlags,
			.shaderModule =
				library->nativeModule,
		};
		const VkPipeline pipeline = vulkan::createComputePipeline(m_device, createInfo);
		const PipelineHandle handle = m_resourceTable->registerPipeline(
			VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline,
			desc.specializationVariant,
			nativeLayout,
			std::move(ownedLayout.rootBindings),
			true,
			true);
		return handle;
	}

	void VulkanDevice::destroyPipeline(PipelineHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const PipelineRecord* record = m_resourceTable->tryGetPipeline(handle);
		if (record != nullptr)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Pipeline, handle.index, handle.generation},
				.retirementDependencies = record->pendingUses,
				.nativeObject = asNativeU64(record->nativePipeline),
				.secondaryNativeObject = asNativeU64(record->nativeLayout),
				.owned = record->owned,
				.ownsSecondary = record->ownsLayout,
				.preciseDependencies = true,
			});
		}
		m_resourceTable->destroyPipeline(handle);
	}

	Result<ResidencySetHandle> VulkanDevice::createResidencySet(const ResidencySetDesc& desc)
	{
		if (m_capabilities.explicitResidency == ExplicitResidencyLevel::unsupported)
		{
			return Result<ResidencySetHandle>::fail(
				RHIErrorCode::unsupported, "Vulkan explicit residency is unavailable");
		}
		if (m_resourceTable == nullptr)
		{
			return Result<ResidencySetHandle>::fail(
				RHIErrorCode::invalidState, "Vulkan device is not initialized");
		}
		return m_resourceTable->registerResidencySet(desc);
	}

	RHIResult VulkanDevice::destroyResidencySet(ResidencySetHandle handle)
	{
		if (m_resourceTable == nullptr)
			return RHIResult::fail(RHIErrorCode::invalidState, "Vulkan device is not initialized");
		return m_resourceTable->removeResidencySet(handle);
	}

	RHIResult VulkanDevice::updateResidencySet(
		ResidencySetHandle handle, const ResidencyUpdateBatch& batch)
	{
		if (m_resourceTable == nullptr)
			return RHIResult::fail(RHIErrorCode::invalidState, "Vulkan device is not initialized");
		return m_resourceTable->updateResidencySet(handle, batch);
	}
} // namespace demo::rhi::vulkan
