#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include "VulkanDevice.h"
#include "internal/VulkanCommon.h"
#include "VulkanCommandBuffer.h"
#include "VulkanFrameContext.h"
#include "VulkanPipelines.h"
#include "VulkanResourceTable.h"

#include <algorithm>
#include <array>
#include <cstring>
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

	VulkanDevice::~VulkanDevice()
	{
		deinit();
	}

	void VulkanDevice::init(const DeviceCreateInfo& createInfo)
	{
		// Delegate to initVulkan with only the base fields; Vulkan extension/layer
		// fields remain empty. RenderDevice calls initVulkan() directly to supply them.
		VulkanDeviceCreateInfo vkCreateInfo;
		vkCreateInfo.base = createInfo;
		initVulkan(vkCreateInfo);
	}

	void VulkanDevice::initVulkan(const VulkanDeviceCreateInfo& createInfo)
	{
		ensure(!m_initialized, "VulkanDevice::initVulkan called twice");
		m_createInfo = createInfo;
		LOGI("VulkanDevice::init: begin");

		initInstance();
		ensure(m_instance != VK_NULL_HANDLE, "VulkanDevice::init failed: Vulkan instance is null");
		selectPhysicalDevice();
		initLogicalDevice();

		// Upload command pool for async uploads — migrated from RenderDevice (UPL-02)
		const VkCommandPoolCreateInfo uploadPoolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = m_graphicsQueue.familyIndex,
		};
		VK_CHECK(vkCreateCommandPool(m_device, &uploadPoolInfo, nullptr, &m_uploadCmdPool));

		m_initialized = true;
		LOGI("VulkanDevice::init: completed");
	}

	void VulkanDevice::deinit()
	{
		if (m_device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_device);
			drainRetirements();
			if (m_argumentPool != VK_NULL_HANDLE)
			{
				vkDestroyDescriptorPool(m_device, m_argumentPool, nullptr);
				m_argumentPool = VK_NULL_HANDLE;
			}
			// Upload cmd pool — destroy before logical device (UPL-02)
			if (m_uploadCmdPool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_device, m_uploadCmdPool, nullptr);
				m_uploadCmdPool = VK_NULL_HANDLE;
			}
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
		m_frameContext = nullptr;
		m_pendingRetirements.clear();
		m_uploadPendingFrames.clear();
	}

	uint32_t VulkanDevice::getApiVersion() const
	{
		return m_apiVersion;
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

	QueueInfo VulkanDevice::getGraphicsQueue() const
	{
		return m_graphicsQueue.toRhi();
	}

	QueueInfo VulkanDevice::getComputeQueue() const
	{
		return m_computeQueue.toRhi();
	}

	QueueInfo VulkanDevice::getTransferQueue() const
	{
		return m_transferQueue.toRhi();
	}

	bool VulkanDevice::isInstanceExtensionSupported(const char* name) const
	{
		return extensionAvailable(name, m_availableInstanceExtensions);
	}

	bool VulkanDevice::isDeviceExtensionSupported(const char* name) const
	{
		return extensionAvailable(name, m_availableDeviceExtensions);
	}

	void VulkanDevice::waitIdle()
	{
		if (m_device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_device);
			drainRetirements();
		}
	}

	uint64_t VulkanDevice::retirementTimelineValue() const
	{
		if (m_frameContext == nullptr)
		{
			return 0;
		}

		return m_frameContext->getCurrentFrameValue() + m_frameContext->getFrameCount();
	}

	void VulkanDevice::enqueueRetirement(NativeRetirement retirement)
	{
		if (!retirement.owned && !retirement.ownsSecondary)
		{
			return;
		}

		retirement.retireTimelineValue = retirementTimelineValue();
		m_pendingRetirements.push_back(retirement);
	}

	uint32_t VulkanDevice::processRetirements(uint64_t completedTimelineValue)
	{
		uint32_t destroyed = 0;
		auto it = std::remove_if(m_pendingRetirements.begin(), m_pendingRetirements.end(),
		                         [this, completedTimelineValue, &destroyed](const NativeRetirement& retirement)
		                         {
			                         if (!hasReachedRetirementPoint(completedTimelineValue,
			                                                        retirement.retireTimelineValue))
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
			if (retirement.owned && retirement.nativeObject != 0 && m_argumentPool != VK_NULL_HANDLE)
			{
				VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(retirement.
					nativeObject));
				vkFreeDescriptorSets(m_device, m_argumentPool, 1, &set);
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
			.familyIndex = familyIndex, .queueIndex = queueIndex, .queueCount = queueCount,
			.backendHandle = asNativeU64(queue)
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
		m_physicalDeviceInfo.apiVersion = m_vkProperties2.properties.apiVersion;
		m_physicalDeviceInfo.driverVersion = m_vkProperties2.properties.driverVersion;
		m_physicalDeviceInfo.vendorId = m_vkProperties2.properties.vendorID;
		m_physicalDeviceInfo.deviceId = m_vkProperties2.properties.deviceID;
		m_physicalDeviceInfo.deviceType = static_cast<uint32_t>(m_vkProperties2.properties.deviceType);

#ifdef VK_API_VERSION_1_4
		ensure(m_physicalDeviceInfo.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0),
		       "Require Vulkan 1.4 physical device");
#else
		ensure(m_physicalDeviceInfo.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0),
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
		appendFeatureNode(featureChain, &m_features12);
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

		m_featuresChainHead = featureChain;
		m_deviceFeatures.pNext = m_featuresChainHead;
		vkGetPhysicalDeviceFeatures2(m_physicalDevice, &m_deviceFeatures);

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

		m_capabilities.coreGraphics = m_graphicsQueue.familyIndex != ~0u;
		m_capabilities.coreCompute = m_computeQueue.familyIndex != ~0u;

		const bool bindlessDescriptorIndexing = m_features12.descriptorIndexing == VK_TRUE;
		const bool bindlessRuntimeArray = m_features12.runtimeDescriptorArray == VK_TRUE;
		const bool bindlessVariableCount = m_features12.descriptorBindingVariableDescriptorCount == VK_TRUE;
		const bool bindlessPartiallyBound = m_features12.descriptorBindingPartiallyBound == VK_TRUE;
		const bool bindlessNonUniformSampling = m_features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
		const bool coreFrameFeatures = m_featureInfo.timelineSemaphore && m_featureInfo.synchronization2 &&
			m_featureInfo.dynamicRendering
			&& m_featureInfo.maintenance5 && m_featureInfo.maintenance6;
		m_capabilities.coreBindless = bindlessDescriptorIndexing && bindlessRuntimeArray && bindlessVariableCount
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

	namespace
	{
		[[nodiscard]] VkImageViewType toVkImageViewType(ImageViewType type)
		{
			switch (type)
			{
			case ImageViewType::e2DArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case ImageViewType::eCube:
				return VK_IMAGE_VIEW_TYPE_CUBE;
			case ImageViewType::e3D:
				return VK_IMAGE_VIEW_TYPE_3D;
			default:
				return VK_IMAGE_VIEW_TYPE_2D;
			}
		}

		[[nodiscard]] VkImageAspectFlags toVkImageAspect(TextureAspect aspect)
		{
			switch (aspect)
			{
			case TextureAspect::depth:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			case TextureAspect::depthStencil:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}

		[[nodiscard]] VkComponentSwizzle toVkSwizzle(ComponentSwizzle s)
		{
			switch (s)
			{
			case ComponentSwizzle::zero:
				return VK_COMPONENT_SWIZZLE_ZERO;
			case ComponentSwizzle::one:
				return VK_COMPONENT_SWIZZLE_ONE;
			case ComponentSwizzle::r:
				return VK_COMPONENT_SWIZZLE_R;
			case ComponentSwizzle::g:
				return VK_COMPONENT_SWIZZLE_G;
			case ComponentSwizzle::b:
				return VK_COMPONENT_SWIZZLE_B;
			case ComponentSwizzle::a:
				return VK_COMPONENT_SWIZZLE_A;
			default:
				return VK_COMPONENT_SWIZZLE_IDENTITY;
			}
		}

		[[nodiscard]] VkFormat toVkViewFormat(TextureFormat format)
		{
			switch (format)
			{
			case TextureFormat::rgba8Unorm:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case TextureFormat::bgra8Unorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			case TextureFormat::rgba16Sfloat:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			case TextureFormat::rg16Sfloat:
				return VK_FORMAT_R16G16_SFLOAT;
			case TextureFormat::r32Sfloat:
				return VK_FORMAT_R32_SFLOAT;
			case TextureFormat::d16Unorm:
				return VK_FORMAT_D16_UNORM;
			case TextureFormat::d32Sfloat:
				return VK_FORMAT_D32_SFLOAT;
			case TextureFormat::d24UnormS8:
				return VK_FORMAT_D24_UNORM_S8_UINT;
			case TextureFormat::d32SfloatS8:
				return VK_FORMAT_D32_SFLOAT_S8_UINT;
			case TextureFormat::bc6hUfloatBlock:
				return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			case TextureFormat::bc6hSfloatBlock:
				return VK_FORMAT_BC6H_SFLOAT_BLOCK;
			case TextureFormat::bc7UnormBlock:
				return VK_FORMAT_BC7_UNORM_BLOCK;
			case TextureFormat::bc7SrgbBlock:
				return VK_FORMAT_BC7_SRGB_BLOCK;
			default:
				return VK_FORMAT_UNDEFINED;
			}
		}
	} // namespace

	TextureViewHandle VulkanDevice::createTextureView(const TextureViewCreateDesc& desc)
	{
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
		return m_resourceTable->registerTextureView(reinterpret_cast<uint64_t>(view), /*owned=*/true);
	}

	TextureViewHandle VulkanDevice::registerExternalTextureView(uint64_t externalView)
	{
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::setResourceTable must be called before registerExternalTextureView");
		return m_resourceTable->registerTextureView(externalView, /*owned=*/false);
	}

	void VulkanDevice::destroyTextureView(TextureViewHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const TextureViewRecord record = m_resourceTable->removeTextureView(handle);
		if (record.owned && record.nativeView != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::TextureView, handle.index, handle.generation},
				.nativeObject = record.nativeView,
				.owned = true,
			});
		}
	}

	namespace
	{
		[[nodiscard]] VkImageType toVkImageType(TextureDimension dim)
		{
			return dim == TextureDimension::e3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
		}

		[[nodiscard]] VkImageCreateFlags toVkImageCreateFlags(TextureDimension dim)
		{
			return dim == TextureDimension::eCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
		}

		[[nodiscard]] VkImageUsageFlags toVkImageUsage(TextureUsageFlags flags)
		{
			VkImageUsageFlags usage = 0;
			const auto has = [&](TextureUsageFlags bit) { return static_cast<uint32_t>(flags & bit) != 0; };
			if (has(TextureUsageFlags::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			if (has(TextureUsageFlags::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			if (has(TextureUsageFlags::colorAttachment)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (has(TextureUsageFlags::depthAttachment)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if (has(TextureUsageFlags::transferSrc)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (has(TextureUsageFlags::transferDst)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (has(TextureUsageFlags::inputAttachment)) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
			return usage;
		}

		[[nodiscard]] VkSampleCountFlagBits toVkSamples(SampleCount count)
		{
			switch (count)
			{
			case SampleCount::count2:
				return VK_SAMPLE_COUNT_2_BIT;
			case SampleCount::count4:
				return VK_SAMPLE_COUNT_4_BIT;
			case SampleCount::count8:
				return VK_SAMPLE_COUNT_8_BIT;
			default:
				return VK_SAMPLE_COUNT_1_BIT;
			}
		}

		[[nodiscard]] VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage);
	} // namespace

	TextureHandle VulkanDevice::createTexture(const TextureDesc& desc)
	{
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
			.samples = toVkSamples(desc.sampleCount),
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

		return m_resourceTable->registerTexture(reinterpret_cast<uint64_t>(image),
		                                        reinterpret_cast<uint64_t>(allocation),
		                                        /*owned=*/true);
	}

	void VulkanDevice::destroyTexture(TextureHandle handle)
	{
		destroyImage(handle);
	}

	TextureHandle VulkanDevice::registerExternalTexture(uint64_t externalImage)
	{
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::setResourceTable must be called before registerExternalTexture");
		return m_resourceTable->registerTexture(externalImage, /*nativeAllocation=*/0, /*owned=*/false);
	}

	void VulkanDevice::destroyImage(TextureHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const TextureRecord record = m_resourceTable->removeTexture(handle);
		if (record.owned && record.nativeImage != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Texture, handle.index, handle.generation},
				.nativeObject = record.nativeImage,
				.nativeAllocation = record.nativeAllocation,
				.owned = true,
			});
		}
	}

	namespace
	{
		[[nodiscard]] VkBufferUsageFlags toVkBufferUsage(BufferUsageFlags flags, bool allowGpuAddress,
		                                                 bool allowIndirect)
		{
			VkBufferUsageFlags usage = 0;
			const auto has = [&](BufferUsageFlags bit) { return static_cast<uint32_t>(flags & bit) != 0; };
			if (has(BufferUsageFlags::vertex)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			if (has(BufferUsageFlags::index)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if (has(BufferUsageFlags::uniform)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			if (has(BufferUsageFlags::storage)) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			if (has(BufferUsageFlags::indirect)) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			if (has(BufferUsageFlags::transferSrc)) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			if (has(BufferUsageFlags::transferDst)) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			if (has(BufferUsageFlags::shaderDeviceAddress) || allowGpuAddress)
				usage |=
					VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
			if (allowIndirect) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			return usage;
		}

		[[nodiscard]] VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage)
		{
			switch (usage)
			{
			case MemoryUsage::cpuToGpu: return VMA_MEMORY_USAGE_CPU_TO_GPU;
			case MemoryUsage::gpuToCpu: return VMA_MEMORY_USAGE_GPU_TO_CPU;
			case MemoryUsage::transientAttachment: return VMA_MEMORY_USAGE_GPU_ONLY;
			default: return VMA_MEMORY_USAGE_GPU_ONLY;
			}
		}

		[[nodiscard]] bool isCpuVisible(MemoryUsage usage)
		{
			return usage == MemoryUsage::cpuToGpu || usage == MemoryUsage::gpuToCpu;
		}

		[[nodiscard]] VkSamplerAddressMode toVkAddressMode(AddressMode mode)
		{
			switch (mode)
			{
			case AddressMode::clampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case AddressMode::clampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			case AddressMode::mirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			}
		}
	} // namespace

	BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc)
	{
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

		return m_resourceTable->registerBuffer(BufferRecord{
			.nativeBuffer = reinterpret_cast<uint64_t>(buffer),
			.nativeAllocation = reinterpret_cast<uint64_t>(allocation),
			.gpuAddress = gpuAddress,
			.mapped = allocResult.pMappedData,
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
		if (record.owned && record.nativeBuffer != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Buffer, handle.index, handle.generation},
				.nativeObject = record.nativeBuffer,
				.nativeAllocation = record.nativeAllocation,
				.owned = true,
			});
		}
	}

	BufferHandle VulkanDevice::registerExternalBuffer(uint64_t externalBuffer)
	{
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::setResourceTable must be called before registerExternalBuffer");
		BufferRecord rec{};
		rec.nativeBuffer = externalBuffer;
		rec.owned = false;
		return m_resourceTable->registerBuffer(rec);
	}

	void VulkanDevice::updateBufferBinding(BufferHandle handle, uint64_t externalBuffer)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		m_resourceTable->updateBuffer(handle, externalBuffer);
	}

	GpuPtr VulkanDevice::getBufferGpuAddress(BufferHandle handle) const
	{
		if (m_resourceTable == nullptr)
		{
			return {};
		}
		const BufferRecord* record = m_resourceTable->tryGetBuffer(handle);
		return record != nullptr ? GpuPtr{record->gpuAddress} : GpuPtr{};
	}

	void* VulkanDevice::mapBuffer(BufferHandle handle)
	{
		if (m_resourceTable == nullptr)
		{
			return nullptr;
		}
		const BufferRecord* record = m_resourceTable->tryGetBuffer(handle);
		return record != nullptr ? record->mapped : nullptr;
	}

	void VulkanDevice::unmapBuffer(BufferHandle)
	{
		// Buffers use persistent mapping (VMA_ALLOCATION_CREATE_MAPPED_BIT); nothing to unmap.
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
		return m_resourceTable->registerSampler(reinterpret_cast<uint64_t>(sampler));
	}

	void VulkanDevice::destroySampler(SamplerHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const SamplerRecord record = m_resourceTable->removeSampler(handle);
		if (record.nativeSampler != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::Sampler, handle.index, handle.generation},
				.nativeObject = record.nativeSampler,
				.owned = true,
			});
		}
	}

	// -------------------------------------------------------------------------
	// VulkanDeviceInterop typed resolve accessors (D-07)
	// -------------------------------------------------------------------------

	VkImage VulkanDevice::resolveTexture(rhi::TextureHandle handle) const
	{
		return reinterpret_cast<VkImage>(m_resourceTable != nullptr ? m_resourceTable->resolveTexture(handle) : 0);
	}

	VkImageView VulkanDevice::resolveTextureView(rhi::TextureViewHandle handle) const
	{
		return reinterpret_cast<VkImageView>(m_resourceTable != nullptr ? m_resourceTable->resolveTextureView(handle) : 0);
	}

	VkSampler VulkanDevice::resolveSampler(rhi::SamplerHandle handle) const
	{
		return reinterpret_cast<VkSampler>(m_resourceTable != nullptr ? m_resourceTable->resolveSampler(handle) : 0);
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
		return m_resourceTable->registerQueryPool(reinterpret_cast<uint64_t>(pool), queryCount);
	}

	void VulkanDevice::destroyQueryPool(QueryPoolHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const QueryPoolRecord record = m_resourceTable->removeQueryPool(handle);
		if (record.nativePool != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::QueryPool, handle.index, handle.generation},
				.nativeObject = record.nativePool,
				.owned = true,
			});
		}
	}

	uint64_t VulkanDevice::getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex)
	{
		if (m_resourceTable == nullptr)
		{
			return 0;
		}
		const uint64_t nativePool = m_resourceTable->resolveQueryPool(handle);
		if (nativePool == 0)
		{
			return 0;
		}
		uint64_t result = 0;
		vkGetQueryPoolResults(m_device, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(nativePool)), queryIndex,
		                      1,
		                      sizeof(result), &result, sizeof(result),
		                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		return result;
	}

	bool VulkanDevice::getQueryPoolResultsWithAvailability(QueryPoolHandle handle, uint32_t firstQuery,
	                                                       uint32_t queryCount, uint64_t* outPairs)
	{
		if (m_resourceTable == nullptr || outPairs == nullptr || queryCount == 0)
		{
			return false;
		}
		const uint64_t nativePool = m_resourceTable->resolveQueryPool(handle);
		if (nativePool == 0)
		{
			return false;
		}
		// Two uint64 per query: [value, availability]. Non-blocking (no WAIT_BIT) so callers can
		// skip not-yet-ready timestamps instead of stalling the CPU on the GPU.
		const VkResult result = vkGetQueryPoolResults(
			m_device, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(nativePool)), firstQuery, queryCount,
			sizeof(uint64_t) * 2u * queryCount, outPairs, sizeof(uint64_t) * 2u,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		return result == VK_SUCCESS || result == VK_NOT_READY;
	}

	namespace
	{
		[[nodiscard]] VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages)
		{
			VkShaderStageFlags flags = 0;
			const auto has = [&](ShaderStage bit)
			{
				return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(bit)) != 0;
			};
			if (has(ShaderStage::vertex)) flags |= VK_SHADER_STAGE_VERTEX_BIT;
			if (has(ShaderStage::fragment)) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
			if (has(ShaderStage::compute)) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
			if (has(ShaderStage::geometry)) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
			if (has(ShaderStage::tessControl)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			if (has(ShaderStage::tessEval)) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			if (flags == 0) flags = VK_SHADER_STAGE_ALL;
			return flags;
		}

		[[nodiscard]] VkDescriptorType toVkDescriptorType(ArgumentType type, bool dynamicOffset)
		{
			switch (type)
			{
			case ArgumentType::uniformBuffer: return dynamicOffset
				                                         ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
				                                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case ArgumentType::storageBuffer: return dynamicOffset
				                                         ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
				                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ArgumentType::sampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			case ArgumentType::storageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case ArgumentType::sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
			case ArgumentType::combinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case ArgumentType::accelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			default: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}
		}
	} // namespace

	ArgumentLayoutHandle VulkanDevice::createArgumentLayout(const ArgumentLayoutDesc& desc)
	{
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentLayout");
		std::vector<VkDescriptorSetLayoutBinding> bindings(desc.bindingCount);
		std::vector<uint32_t> dynamicBindings;
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
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
		}
		const VkDescriptorSetLayoutCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = static_cast<uint32_t>(bindings.size()),
			.pBindings = bindings.empty() ? nullptr : bindings.data(),
		};
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VK_CHECK(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout));
		return m_resourceTable->registerArgumentLayout(reinterpret_cast<uint64_t>(layout), std::move(dynamicBindings));
	}

	void VulkanDevice::destroyArgumentLayout(ArgumentLayoutHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const ArgumentLayoutRecord record = m_resourceTable->removeArgumentLayout(handle);
		if (record.nativeLayout != 0)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::ArgumentLayout, handle.index, handle.generation},
				.nativeObject = record.nativeLayout,
				.owned = true,
			});
		}
	}

	ArgumentTableHandle VulkanDevice::createArgumentTable(ArgumentLayoutHandle layout)
	{
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentTable");
		if (m_argumentPool == VK_NULL_HANDLE)
		{
			const std::array<VkDescriptorPoolSize, 8> sizes{
				{
					{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
					{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
					{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
					{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
					{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
					{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
					{VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
					{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
				}
			};
			const VkDescriptorPoolCreateInfo poolInfo{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
				.maxSets = 4096,
				.poolSizeCount = static_cast<uint32_t>(sizes.size()),
				.pPoolSizes = sizes.data(),
			};
			VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_argumentPool));
		}

		const uint64_t nativeLayout = m_resourceTable->resolveArgumentLayout(layout);
		assert(nativeLayout != 0 && "createArgumentTable requires a valid argument layout");
		VkDescriptorSetLayout setLayout = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(nativeLayout));
		const VkDescriptorSetAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = m_argumentPool,
			.descriptorSetCount = 1,
			.pSetLayouts = &setLayout,
		};
		VkDescriptorSet set = VK_NULL_HANDLE;
		VK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &set));
		return m_resourceTable->registerArgumentTable(reinterpret_cast<uint64_t>(set), layout);
	}

	void VulkanDevice::destroyArgumentTable(ArgumentTableHandle handle)
	{
		if (handle.isNull() || m_resourceTable == nullptr)
		{
			return;
		}
		const ArgumentTableRecord record = m_resourceTable->removeArgumentTable(handle);
		if (record.owned && record.nativeSet != 0 && m_argumentPool != VK_NULL_HANDLE)
		{
			enqueueRetirement(NativeRetirement{
				.resource = ResourceHandle{ResourceKind::ArgumentTable, handle.index, handle.generation},
				.nativeObject = record.nativeSet,
				.owned = true,
			});
		}
	}

	void VulkanDevice::updateArgumentTable(ArgumentTableHandle table, uint32_t writeCount, const ArgumentWrite* writes)
	{
		if (m_resourceTable == nullptr || writeCount == 0 || writes == nullptr)
		{
			return;
		}
		const ArgumentTableRecord* tableRecord = m_resourceTable->tryGetArgumentTable(table);
		if (tableRecord == nullptr || tableRecord->nativeSet == 0)
		{
			return;
		}
		VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(tableRecord->nativeSet));

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
				imageInfos[i] = VkDescriptorImageInfo{
					.imageView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_resourceTable->
						resolveTextureView(w.textureView))),
					.imageLayout = (w.type == ArgumentType::storageTexture
						               || w.accessIntent == ArgumentAccessIntent::readWrite)
						               ? VK_IMAGE_LAYOUT_GENERAL
						               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				out.pImageInfo = &imageInfos[i];
				break;
			case ArgumentType::sampler:
				imageInfos[i] = VkDescriptorImageInfo{
					.sampler = reinterpret_cast<VkSampler>(static_cast<uintptr_t>(m_resourceTable->resolveSampler(
						w.sampler)))
				};
				out.pImageInfo = &imageInfos[i];
				break;
			case ArgumentType::combinedImageSampler:
				imageInfos[i] = VkDescriptorImageInfo{
					.sampler = reinterpret_cast<VkSampler>(static_cast<uintptr_t>(m_resourceTable->resolveSampler(
						w.sampler))),
					.imageView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_resourceTable->
						resolveTextureView(w.textureView))),
					.imageLayout = (w.accessIntent == ArgumentAccessIntent::readWrite)
						               ? VK_IMAGE_LAYOUT_GENERAL
						               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				out.pImageInfo = &imageInfos[i];
				break;
			default: // buffer types
				bufferInfos[i] = VkDescriptorBufferInfo{
					.buffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(m_resourceTable->
						resolveBuffer(w.buffer))),
					.offset = w.offset,
					.range = w.size == 0 ? VK_WHOLE_SIZE : w.size,
				};
				out.pBufferInfo = &bufferInfos[i];
				break;
			}
		}
		vkUpdateDescriptorSets(m_device, writeCount, vkWrites.data(), 0, nullptr);
	}

	uint64_t VulkanDevice::resolveArgumentLayoutNative(ArgumentLayoutHandle layout) const
	{
		return m_resourceTable != nullptr ? m_resourceTable->resolveArgumentLayout(layout) : 0;
	}

	uint64_t VulkanDevice::resolveArgumentTableNative(ArgumentTableHandle table) const
	{
		return m_resourceTable != nullptr ? m_resourceTable->resolveArgumentTable(table) : 0;
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
			VkDevice device, const VulkanResourceTable& resourceTable, const PipelineBindingSchemaDesc& bindingSchema,
			const ArgumentLayoutHandle* legacyLayouts, uint32_t legacyLayoutCount,
			const PipelinePushConstantRange* legacyPushConstantRanges, uint32_t legacyPushConstantRangeCount)
		{
			const PipelineBindingSchemaValidationResult validation = validatePipelineBindingSchema(bindingSchema);
			ASSERT(validation.valid(), "Pipeline binding schema must be valid before Vulkan layout lowering");

			uint32_t maxSetSlot = 0;
			bool hasSetSlot = false;
			if (bindingSchema.argumentSlotCount > 0)
			{
				for (uint32_t i = 0; i < bindingSchema.argumentSlotCount; ++i)
				{
					maxSetSlot = (std::max)(maxSetSlot, bindingSchema.argumentSlots[i].slot);
					hasSetSlot = true;
				}
			}
			else if (legacyLayoutCount > 0)
			{
				maxSetSlot = legacyLayoutCount - 1u;
				hasSetSlot = true;
			}

			std::vector<VkDescriptorSetLayout> setLayouts(hasSetSlot ? maxSetSlot + 1u : 0u, VK_NULL_HANDLE);
			if (bindingSchema.argumentSlotCount > 0)
			{
				for (uint32_t i = 0; i < bindingSchema.argumentSlotCount; ++i)
				{
					const PipelineArgumentSlotDesc& slot = bindingSchema.argumentSlots[i];
					const uint64_t nativeLayout = resourceTable.resolveArgumentLayout(slot.layout);
					ASSERT(nativeLayout != 0,
					       "Pipeline argument slot layout must resolve to a native descriptor set layout");
					setLayouts[slot.slot] = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(
						nativeLayout));
				}
			}
			else
			{
				for (uint32_t i = 0; i < legacyLayoutCount; ++i)
				{
					const uint64_t nativeLayout = resourceTable.resolveArgumentLayout(legacyLayouts[i]);
					ASSERT(nativeLayout != 0,
					       "Pipeline argument layout must resolve to a native descriptor set layout");
					setLayouts[i] = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(nativeLayout));
				}
			}

			while (!setLayouts.empty() && setLayouts.back() == VK_NULL_HANDLE)
			{
				setLayouts.pop_back();
			}

			std::vector<VkPushConstantRange> vkPushConstants;
			std::vector<PipelineRecord::RootBindingLowering> rootLowering;
			if (bindingSchema.rootBindingCount > 0)
			{
				vkPushConstants.reserve(bindingSchema.rootBindingCount);
				rootLowering.reserve(bindingSchema.rootBindingCount);
				uint32_t nextOffset = 0;
				for (uint32_t i = 0; i < bindingSchema.rootBindingCount; ++i)
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
			}
			else
			{
				vkPushConstants.resize(legacyPushConstantRangeCount);
				for (uint32_t i = 0; i < legacyPushConstantRangeCount; ++i)
				{
					vkPushConstants[i] = VkPushConstantRange{
						.stageFlags = toVkShaderStageFlags(legacyPushConstantRanges[i].stages),
						.offset = legacyPushConstantRanges[i].offset,
						.size = legacyPushConstantRanges[i].size,
					};
					rootLowering.push_back(PipelineRecord::RootBindingLowering{
						.slot = legacyPushConstantRanges[i].offset,
						.offset = legacyPushConstantRanges[i].offset,
						.size = legacyPushConstantRanges[i].size,
						.kind = static_cast<uint32_t>(RootBindingKind::constants),
						.stages = static_cast<uint32_t>(legacyPushConstantRanges[i].stages),
					});
				}
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
			                                          desc.bindingSchema,
			                                          desc.argumentLayouts,
			                                          desc.argumentLayoutCount,
			                                          desc.pushConstantRanges,
			                                          desc.pushConstantRangeCount);
		}

		[[nodiscard]] VulkanPipelineLayoutBuildResult makePipelineLayoutFromComputeDesc(
			VkDevice device, const VulkanResourceTable& resourceTable, const ComputePipelineDesc& desc)
		{
			return makePipelineLayoutFromPipelineDesc(device,
			                                          resourceTable,
			                                          desc.bindingSchema,
			                                          desc.argumentLayouts,
			                                          desc.argumentLayoutCount,
			                                          desc.pushConstantRanges,
			                                          desc.pushConstantRangeCount);
		}
	} // namespace

	PipelineHandle VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
	{
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::setResourceTable must be called before createGraphicsPipeline");

		VulkanPipelineLayoutBuildResult ownedLayout{};
		ownedLayout = makePipelineLayoutFromGraphicsDesc(m_device, *m_resourceTable, desc);
		VkPipelineLayout nativeLayout = ownedLayout.layout;

		const GraphicsPipelineCreateInfo createInfo{.desc = &desc, .layout = nativeLayout};
		const VkPipeline pipeline = vulkan::createGraphicsPipeline(m_device, createInfo);
		const PipelineHandle handle = m_resourceTable->registerPipeline(
			static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
			reinterpret_cast<uint64_t>(pipeline),
			desc.specializationVariant,
			reinterpret_cast<uint64_t>(nativeLayout),
			std::move(ownedLayout.rootBindings),
			true,
			true);
		return handle;
	}

	PipelineHandle VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc)
	{
		assert(
			m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createComputePipeline");

		VulkanPipelineLayoutBuildResult ownedLayout{};
		ownedLayout = makePipelineLayoutFromComputeDesc(m_device, *m_resourceTable, desc);
		VkPipelineLayout nativeLayout = ownedLayout.layout;

		const ComputePipelineCreateInfo createInfo{
			.desc = &desc, .layout = nativeLayout, .pipelineFlags = desc.pipelineFlags
		};
		const VkPipeline pipeline = vulkan::createComputePipeline(m_device, createInfo);
		const PipelineHandle handle = m_resourceTable->registerPipeline(
			static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE),
			reinterpret_cast<uint64_t>(pipeline),
			desc.specializationVariant,
			reinterpret_cast<uint64_t>(nativeLayout),
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
				.nativeObject = record->nativePipeline,
				.secondaryNativeObject = record->nativeLayout,
				.owned = record->owned,
				.ownsSecondary = record->ownsLayout,
			});
		}
		m_resourceTable->destroyPipeline(handle);
	}

	// --- Immediate upload seam implementation (UPL-02/03) ---

	void VulkanDevice::executeImmediateUpload(std::function<void(rhi::CommandBuffer&)> uploadFn)
	{
		assert(m_device != VK_NULL_HANDLE && "VulkanDevice::executeImmediateUpload called before init");
		assert(
			m_uploadCmdPool != VK_NULL_HANDLE &&
			"VulkanDevice::executeImmediateUpload: upload cmd pool not initialized");
		assert(
			m_frameContext != nullptr &&
			"VulkanDevice::executeImmediateUpload: frame context not set (call setFrameContext)");
		assert(
			m_resourceTable != nullptr &&
			"VulkanDevice::executeImmediateUpload: resource table not set (call setResourceTable)");

		// UPL-03: flush existing pending uploads before submitting a new one
		flushUploadRetirements(true);

		// Lazy-resize pending frame slots to match current frame count
		const uint32_t frameCount = m_frameContext->getFrameCount();
		if (m_uploadPendingFrames.size() < frameCount)
		{
			m_uploadPendingFrames.resize(frameCount);
		}

		VkCommandBuffer cmd = utils::beginSingleTimeCommands(m_device, m_uploadCmdPool);
		VulkanCommandBuffer rhiCmd;
		rhiCmd.setTarget(cmd, m_resourceTable);
		uploadFn(rhiCmd);
		VK_CHECK(vkEndCommandBuffer(cmd));

		const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence uploadFence{VK_NULL_HANDLE};
		VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &uploadFence));

		const VkCommandBufferSubmitInfo cmdBufferInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};
		const VkSubmitInfo2 submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmdBufferInfo,
		};
		VK_CHECK(vkQueueSubmit2(m_graphicsQueue.queue, 1, &submitInfo, uploadFence)); // non-blocking

		const uint32_t frameIndex = m_frameContext->getCurrentFrameIndex();
		m_uploadPendingFrames[frameIndex].cmds.push_back(cmd);
		m_uploadPendingFrames[frameIndex].fences.push_back(uploadFence);
	}

	void VulkanDevice::flushUploadRetirements(bool waitForCompletion)
	{
		if (m_uploadPendingFrames.empty() || m_device == VK_NULL_HANDLE)
		{
			return;
		}

		for (auto& frame : m_uploadPendingFrames)
		{
			if (frame.fences.empty())
			{
				continue;
			}

			std::vector<size_t> completedIndices;
			completedIndices.reserve(frame.fences.size());
			for (size_t i = 0; i < frame.fences.size(); ++i)
			{
				if (waitForCompletion)
				{
					VK_CHECK(vkWaitForFences(m_device, 1, &frame.fences[i], VK_TRUE, UINT64_MAX));
					completedIndices.push_back(i);
				}
				else if (vkGetFenceStatus(m_device, frame.fences[i]) == VK_SUCCESS)
				{
					completedIndices.push_back(i);
				}
			}

			// Reverse-iterate to keep indices valid while erasing
			for (auto it = completedIndices.rbegin(); it != completedIndices.rend(); ++it)
			{
				vkFreeCommandBuffers(m_device, m_uploadCmdPool, 1, &frame.cmds[*it]);
				vkDestroyFence(m_device, frame.fences[*it], nullptr);
				frame.cmds.erase(frame.cmds.begin() + static_cast<ptrdiff_t>(*it));
				frame.fences.erase(frame.fences.begin() + static_cast<ptrdiff_t>(*it));
			}
		}
		// Note: staging buffer retirement (rhiStagingBuffers) stays in the render layer.
		// VulkanDevice cannot hold render-layer rhi::BufferHandle vectors (D-05 UPL-03).
	}
} // namespace demo::rhi::vulkan
