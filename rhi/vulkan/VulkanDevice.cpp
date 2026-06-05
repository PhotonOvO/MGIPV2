#include "VulkanDevice.h"
#include "../../common/Common.h"
#include "VulkanResourceTable.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <csignal>
#endif

namespace demo::rhi::vulkan {
namespace {

constexpr CapabilityReport       kDeterminismProbeReport{};
constexpr CapabilityRequirements kDeterminismProbeRequirements{};
static_assert(evaluateCapabilityRequirements(kDeterminismProbeReport, kDeterminismProbeRequirements) == RHICapabilityError::MissingCoreGraphics,
              "Mandatory capability failures must be deterministic");

uint64_t asNativeU64(const void* handle)
{
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

bool cstrEqual(const char* lhs, const char* rhs)
{
  if(lhs == nullptr || rhs == nullptr)
  {
    return false;
  }
  return std::strcmp(lhs, rhs) == 0;
}

void pushUnique(std::vector<const char*>& values, const char* value)
{
  if(value == nullptr)
  {
    return;
  }
  const bool found =
      std::any_of(values.begin(), values.end(), [value](const char* current) { return cstrEqual(current, value); });
  if(!found)
  {
    values.push_back(value);
  }
}

void ensure(bool condition, const char* message)
{
  if(!condition)
  {
    throw std::runtime_error(message);
  }
}

void checkVk(VkResult result, const char* message)
{
  if(result != VK_SUCCESS)
  {
    const char* resultName = string_VkResult(result);
    LOGE("%s (VkResult=%s)", message, resultName != nullptr ? resultName : "UNKNOWN");
    throw std::runtime_error(message);
  }
}

}  // namespace

VulkanDevice::~VulkanDevice()
{
  deinit();
}

void VulkanDevice::init(const DeviceCreateInfo& createInfo)
{
  ensure(!m_initialized, "VulkanDevice::init called twice");
  m_createInfo = createInfo;
  LOGI("VulkanDevice::init: begin");

  initInstance();
  ensure(m_instance != VK_NULL_HANDLE, "VulkanDevice::init failed: Vulkan instance is null");
  selectPhysicalDevice();
  initLogicalDevice();

  m_initialized = true;
  LOGI("VulkanDevice::init: completed");
}

void VulkanDevice::deinit()
{
  if(m_device != VK_NULL_HANDLE)
  {
    vkDeviceWaitIdle(m_device);
    if(m_argumentPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(m_device, m_argumentPool, nullptr);
      m_argumentPool = VK_NULL_HANDLE;
    }
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
  }

  destroyDebugMessenger();

  if(m_instance != VK_NULL_HANDLE)
  {
    vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
  }

  m_physicalDevice     = VK_NULL_HANDLE;
  m_graphicsQueue      = {};
  m_computeQueue       = {};
  m_transferQueue      = {};
  m_apiVersion         = 0;
  m_physicalDeviceInfo = {};
  m_featureInfo        = {};
  m_capabilities       = {};
  m_capabilityError    = RHICapabilityError::None;
  m_memoryProperties   = {};
  m_vkMemoryProperties = {};
  m_availableDeviceExtensions.clear();
  m_availableInstanceExtensions.clear();
  m_availableInstanceLayers.clear();
  m_enabledDeviceExtensions.clear();
  m_featuresChainHead             = nullptr;
  m_deviceFeatures                = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  m_features11                    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  m_features12                    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  m_features13                    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
  m_features14                    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#else
  m_maintenance5Features          = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
  m_maintenance6Features          = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
#endif
  m_meshShaderFeatures            = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  m_accelerationStructureFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  m_rayTracingPipelineFeatures    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  m_initialized                   = false;
}

uint64_t VulkanDevice::getNativeInstance() const
{
  return asNativeU64(m_instance);
}

uint64_t VulkanDevice::getNativePhysicalDevice() const
{
  return asNativeU64(m_physicalDevice);
}

uint64_t VulkanDevice::getNativeDevice() const
{
  return asNativeU64(m_device);
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

void* VulkanDevice::getFeaturesChainHead() const
{
  return m_featuresChainHead;
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
  if(m_device != VK_NULL_HANDLE)
  {
    vkDeviceWaitIdle(m_device);
  }
}

QueueInfo VulkanDevice::NativeQueueInfo::toRhi() const
{
  return QueueInfo{.familyIndex = familyIndex, .queueIndex = queueIndex, .queueCount = queueCount, .nativeHandle = asNativeU64(queue)};
}

void VulkanDevice::initInstance()
{
  ensure(vkCreateInstance != nullptr, "Vulkan loader is not initialized. Call volkInitialize() before VulkanDevice::init");
  checkVk(vkEnumerateInstanceVersion(&m_apiVersion), "Failed to enumerate Vulkan instance version");
#ifdef VK_API_VERSION_1_4
  ensure(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 loader");
#else
  ensure(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0), "Require Vulkan 1.3 loader");
#endif

  queryInstanceExtensions();
  queryInstanceLayers();

  std::vector<const char*> enabledInstanceExtensions = m_createInfo.instanceExtensions;

  if(m_createInfo.enableValidationLayers && extensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, m_availableInstanceExtensions))
  {
    pushUnique(enabledInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
  if(extensionAvailable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, m_availableInstanceExtensions))
  {
    pushUnique(enabledInstanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  }
  if(extensionAvailable(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, m_availableInstanceExtensions))
  {
    pushUnique(enabledInstanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
  }

  for(const char* ext : enabledInstanceExtensions)
  {
    ensure(extensionAvailable(ext, m_availableInstanceExtensions), "Requested instance extension is not supported");
  }

  std::vector<const char*> enabledLayers = m_createInfo.instanceLayers;
  if(m_createInfo.enableValidationLayers)
  {
    pushUnique(enabledLayers, "VK_LAYER_KHRONOS_validation");
  }
  for(const char* layer : enabledLayers)
  {
    ensure(layerAvailable(layer, m_availableInstanceLayers), "Requested instance layer is not supported");
  }

  const VkApplicationInfo appInfo{
      .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName   = "minimal_latest",
      .applicationVersion = 1,
      .pEngineName        = "minimal_latest",
      .engineVersion      = 1,
      .apiVersion         = m_apiVersion,
  };

  // Validation settings with GPU-assisted validation enabled
  utils::ValidationSettings validationSettings{};

  const VkInstanceCreateInfo instanceCreateInfo{
      .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext                   = m_createInfo.enableValidationLayers ? validationSettings.buildPNextChain() : nullptr,
      .pApplicationInfo        = &appInfo,
      .enabledLayerCount       = static_cast<uint32_t>(enabledLayers.size()),
      .ppEnabledLayerNames     = enabledLayers.data(),
      .enabledExtensionCount   = static_cast<uint32_t>(enabledInstanceExtensions.size()),
      .ppEnabledExtensionNames = enabledInstanceExtensions.data(),
  };

  LOGI("VulkanDevice::initInstance: creating Vulkan instance");
  const VkResult createResult = vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance);
  if(createResult != VK_SUCCESS)
  {
    const char* resultName = string_VkResult(createResult);
    LOGE("Failed to create Vulkan instance (VkResult=%s)", resultName != nullptr ? resultName : "UNKNOWN");
    throw std::runtime_error("Failed to create Vulkan instance");
  }

  ensure(m_instance != VK_NULL_HANDLE, "vkCreateInstance returned success but instance handle is null");
  volkLoadInstance(m_instance);
  ensure(vkEnumeratePhysicalDevices != nullptr, "Failed to load instance-level Vulkan entry points after vkCreateInstance");

  LOGI("VulkanDevice::initInstance: Vulkan instance created and function pointers loaded");
  initDebugMessenger();
}

void VulkanDevice::selectPhysicalDevice()
{
  ensure(m_instance != VK_NULL_HANDLE, "VulkanDevice::selectPhysicalDevice called with null instance");
  ensure(vkEnumeratePhysicalDevices != nullptr, "vkEnumeratePhysicalDevices is null (instance functions not loaded)");

  uint32_t deviceCount = 0;
  checkVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr), "Failed to enumerate physical devices");
  ensure(deviceCount > 0, "Failed to find Vulkan physical device");

  std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
  checkVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data()), "Failed to enumerate physical devices list");

  size_t selectedIndex = 0;
  for(size_t i = 0; i < physicalDevices.size(); ++i)
  {
    VkPhysicalDeviceProperties2 candidateProps{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(physicalDevices[i], &candidateProps);
    if(candidateProps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      selectedIndex = i;
      break;
    }
  }

  m_physicalDevice = physicalDevices[selectedIndex];
  vkGetPhysicalDeviceProperties2(m_physicalDevice, &m_vkProperties2);

  m_physicalDeviceInfo.deviceName    = m_vkProperties2.properties.deviceName;
  m_physicalDeviceInfo.apiVersion    = m_vkProperties2.properties.apiVersion;
  m_physicalDeviceInfo.driverVersion = m_vkProperties2.properties.driverVersion;
  m_physicalDeviceInfo.vendorId      = m_vkProperties2.properties.vendorID;
  m_physicalDeviceInfo.deviceId      = m_vkProperties2.properties.deviceID;
  m_physicalDeviceInfo.deviceType    = static_cast<uint32_t>(m_vkProperties2.properties.deviceType);

#ifdef VK_API_VERSION_1_4
  ensure(m_physicalDeviceInfo.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 physical device");
#else
  ensure(m_physicalDeviceInfo.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0), "Require Vulkan 1.3 physical device");
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
  if(m_computeQueue.familyIndex != m_graphicsQueue.familyIndex)
  {
    queueFamilyIndices.push_back(m_computeQueue.familyIndex);
  }
  if(m_transferQueue.familyIndex != m_graphicsQueue.familyIndex && m_transferQueue.familyIndex != m_computeQueue.familyIndex)
  {
    queueFamilyIndices.push_back(m_transferQueue.familyIndex);
  }

  const float                          queuePriority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  queueCreateInfos.reserve(queueFamilyIndices.size());
  for(uint32_t familyIndex : queueFamilyIndices)
  {
    queueCreateInfos.push_back(VkDeviceQueueCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = familyIndex,
        .queueCount       = 1,
        .pQueuePriorities = &queuePriority,
    });
  }

  m_deviceFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  m_features11     = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  m_features12     = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  m_features13     = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
  m_features14     = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#else
  m_maintenance5Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
  m_maintenance6Features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
#endif

  m_enabledDeviceExtensions.clear();

  VkBaseOutStructure* featureChain = nullptr;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
  appendFeatureNode(featureChain, &m_features14);
#else
  if(extensionAvailable(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, m_availableDeviceExtensions))
  {
    pushUnique(m_enabledDeviceExtensions, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    appendFeatureNode(featureChain, &m_maintenance5Features);
  }
  if(extensionAvailable(VK_KHR_MAINTENANCE_6_EXTENSION_NAME, m_availableDeviceExtensions))
  {
    pushUnique(m_enabledDeviceExtensions, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
    appendFeatureNode(featureChain, &m_maintenance6Features);
  }
#endif
  appendFeatureNode(featureChain, &m_features13);
  appendFeatureNode(featureChain, &m_features12);
  appendFeatureNode(featureChain, &m_features11);

  for(const ExtensionRequest& request : m_createInfo.deviceExtensions)
  {
    const bool supported = extensionAvailable(request.name, m_availableDeviceExtensions);
    if(supported)
    {
      pushUnique(m_enabledDeviceExtensions, request.name);
      appendFeatureNode(featureChain, request.featuresStruct);
    }
    else if(request.required)
    {
      ensure(false, "Required device extension is not supported");
    }
  }

  m_featuresChainHead    = featureChain;
  m_deviceFeatures.pNext = m_featuresChainHead;
  vkGetPhysicalDeviceFeatures2(m_physicalDevice, &m_deviceFeatures);

  m_featureInfo.timelineSemaphore = m_features12.timelineSemaphore == VK_TRUE;
  m_featureInfo.synchronization2  = m_features13.synchronization2 == VK_TRUE;
  m_featureInfo.dynamicRendering  = m_features13.dynamicRendering == VK_TRUE;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
  m_featureInfo.maintenance5      = m_features14.maintenance5 == VK_TRUE;
  m_featureInfo.maintenance6      = m_features14.maintenance6 == VK_TRUE;
#else
  m_featureInfo.maintenance5      = m_maintenance5Features.maintenance5 == VK_TRUE;
  m_featureInfo.maintenance6      = m_maintenance6Features.maintenance6 == VK_TRUE;
#endif

  detectCapabilities();
  m_capabilityError = validateCapabilities();
  ensure(m_capabilityError == RHICapabilityError::None, toString(m_capabilityError));

  const VkDeviceCreateInfo deviceCreateInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext                   = &m_deviceFeatures,
      .queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size()),
      .pQueueCreateInfos       = queueCreateInfos.data(),
      .enabledExtensionCount   = static_cast<uint32_t>(m_enabledDeviceExtensions.size()),
      .ppEnabledExtensionNames = m_enabledDeviceExtensions.data(),
  };

  checkVk(vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device), "Failed to create logical device");

  if(m_graphicsQueue.familyIndex != ~0u)
  {
    vkGetDeviceQueue(m_device, m_graphicsQueue.familyIndex, m_graphicsQueue.queueIndex, &m_graphicsQueue.queue);
  }
  if(m_computeQueue.familyIndex != ~0u)
  {
    vkGetDeviceQueue(m_device, m_computeQueue.familyIndex, m_computeQueue.queueIndex, &m_computeQueue.queue);
  }
  if(m_transferQueue.familyIndex != ~0u)
  {
    vkGetDeviceQueue(m_device, m_transferQueue.familyIndex, m_transferQueue.queueIndex, &m_transferQueue.queue);
  }
}

void VulkanDevice::initDebugMessenger()
{
  if(!m_createInfo.enableValidationLayers || vkCreateDebugUtilsMessengerEXT == nullptr)
  {
    return;
  }

  const VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
      .pfnUserCallback = VulkanDevice::debugCallback,
      .pUserData       = nullptr,
  };

  checkVk(vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_debugMessenger), "Failed to create debug messenger");
}

void VulkanDevice::destroyDebugMessenger()
{
  if(m_debugMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
  {
    vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    m_debugMessenger = VK_NULL_HANDLE;
  }
}

void VulkanDevice::queryInstanceExtensions()
{
  uint32_t count = 0;
  checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "Failed querying instance extension count");
  m_availableInstanceExtensions.resize(count);
  checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, m_availableInstanceExtensions.data()),
          "Failed querying instance extensions");
}

void VulkanDevice::queryInstanceLayers()
{
  uint32_t count = 0;
  checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr), "Failed querying instance layer count");
  m_availableInstanceLayers.resize(count);
  checkVk(vkEnumerateInstanceLayerProperties(&count, m_availableInstanceLayers.data()), "Failed querying instance layers");
}

void VulkanDevice::queryDeviceExtensions()
{
  uint32_t count = 0;
  checkVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr), "Failed querying device extension count");
  m_availableDeviceExtensions.resize(count);
  checkVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, m_availableDeviceExtensions.data()),
          "Failed querying device extensions");
}

void VulkanDevice::queryMemoryProperties()
{
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_vkMemoryProperties);

  m_memoryProperties.memoryTypes.clear();
  m_memoryProperties.memoryHeaps.clear();

  m_memoryProperties.memoryTypes.reserve(m_vkMemoryProperties.memoryTypeCount);
  for(uint32_t i = 0; i < m_vkMemoryProperties.memoryTypeCount; ++i)
  {
    m_memoryProperties.memoryTypes.push_back(MemoryTypeInfo{
        .propertyFlags = static_cast<uint32_t>(m_vkMemoryProperties.memoryTypes[i].propertyFlags),
        .heapIndex     = m_vkMemoryProperties.memoryTypes[i].heapIndex,
    });
  }

  m_memoryProperties.memoryHeaps.reserve(m_vkMemoryProperties.memoryHeapCount);
  for(uint32_t i = 0; i < m_vkMemoryProperties.memoryHeapCount; ++i)
  {
    m_memoryProperties.memoryHeaps.push_back(MemoryHeapInfo{
        .size  = static_cast<uint64_t>(m_vkMemoryProperties.memoryHeaps[i].size),
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

  auto findQueueFamily = [&](VkQueueFlags required, VkQueueFlags preferredAbsent) -> uint32_t {
    uint32_t firstMatch = ~0u;
    for(uint32_t i = 0; i < queueFamilyCount; ++i)
    {
      if((queueFamilies[i].queueFlags & required) == 0 || queueFamilies[i].queueCount == 0)
      {
        continue;
      }
      if((queueFamilies[i].queueFlags & preferredAbsent) == 0)
      {
        return i;
      }
      if(firstMatch == ~0u)
      {
        firstMatch = i;
      }
    }
    return firstMatch;
  };

  const uint32_t graphicsFamily = findQueueFamily(VK_QUEUE_GRAPHICS_BIT, 0);
  ASSERT(graphicsFamily != ~0u, "No graphics queue family available");

  const uint32_t computeFamily  = findQueueFamily(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
  const uint32_t transferFamily = findQueueFamily(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

  m_graphicsQueue.familyIndex = graphicsFamily;
  m_graphicsQueue.queueCount  = queueFamilies[graphicsFamily].queueCount;
  m_graphicsQueue.queueIndex  = 0;

  const uint32_t computeFallback = computeFamily == ~0u ? graphicsFamily : computeFamily;
  m_computeQueue.familyIndex     = computeFallback;
  m_computeQueue.queueCount      = queueFamilies[computeFallback].queueCount;
  m_computeQueue.queueIndex      = 0;

  const uint32_t transferFallback = transferFamily == ~0u ? computeFallback : transferFamily;
  m_transferQueue.familyIndex     = transferFallback;
  m_transferQueue.queueCount      = queueFamilies[transferFallback].queueCount;
  m_transferQueue.queueIndex      = 0;
}

void VulkanDevice::detectCapabilities()
{
  m_capabilities = {};

  m_capabilities.coreGraphics = m_graphicsQueue.familyIndex != ~0u;
  m_capabilities.coreCompute  = m_computeQueue.familyIndex != ~0u;

  const bool bindlessDescriptorIndexing = m_features12.descriptorIndexing == VK_TRUE;
  const bool bindlessRuntimeArray       = m_features12.runtimeDescriptorArray == VK_TRUE;
  const bool bindlessVariableCount      = m_features12.descriptorBindingVariableDescriptorCount == VK_TRUE;
  const bool bindlessPartiallyBound     = m_features12.descriptorBindingPartiallyBound == VK_TRUE;
  const bool bindlessNonUniformSampling = m_features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
  const bool coreFrameFeatures = m_featureInfo.timelineSemaphore && m_featureInfo.synchronization2 && m_featureInfo.dynamicRendering
                                 && m_featureInfo.maintenance5 && m_featureInfo.maintenance6;
  m_capabilities.coreBindless = bindlessDescriptorIndexing && bindlessRuntimeArray && bindlessVariableCount
                                && bindlessPartiallyBound && bindlessNonUniformSampling && coreFrameFeatures;

  m_capabilities.extensionAsyncCompute =
      m_computeQueue.familyIndex != ~0u && m_computeQueue.familyIndex != m_graphicsQueue.familyIndex;

  VkPhysicalDeviceFeatures2 probeFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  m_meshShaderFeatures            = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  m_accelerationStructureFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  m_rayTracingPipelineFeatures    = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

  VkBaseOutStructure* probeChain = nullptr;
  if(extensionAvailable(VK_EXT_MESH_SHADER_EXTENSION_NAME, m_availableDeviceExtensions))
  {
    appendFeatureNode(probeChain, &m_meshShaderFeatures);
  }
  if(extensionAvailable(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, m_availableDeviceExtensions)
     && extensionAvailable(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, m_availableDeviceExtensions))
  {
    appendFeatureNode(probeChain, &m_accelerationStructureFeatures);
    appendFeatureNode(probeChain, &m_rayTracingPipelineFeatures);
  }

  probeFeatures.pNext = probeChain;
  vkGetPhysicalDeviceFeatures2(m_physicalDevice, &probeFeatures);

  m_capabilities.extensionMeshShader = extensionAvailable(VK_EXT_MESH_SHADER_EXTENSION_NAME, m_availableDeviceExtensions)
                                       && m_meshShaderFeatures.meshShader == VK_TRUE;
  m_capabilities.extensionRayTracing =
      extensionAvailable(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, m_availableDeviceExtensions)
      && extensionAvailable(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, m_availableDeviceExtensions)
      && m_accelerationStructureFeatures.accelerationStructure == VK_TRUE
      && m_rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;
}

RHICapabilityError VulkanDevice::validateCapabilities() const
{
  return evaluateCapabilityRequirements(m_capabilities, m_createInfo.capabilityRequirements);
}

bool VulkanDevice::extensionAvailable(const char* name, const std::vector<VkExtensionProperties>& extensions)
{
  if(name == nullptr)
  {
    return false;
  }

  for(const VkExtensionProperties& ext : extensions)
  {
    if(std::strcmp(name, ext.extensionName) == 0)
    {
      return true;
    }
  }
  return false;
}

bool VulkanDevice::layerAvailable(const char* name, const std::vector<VkLayerProperties>& layers)
{
  if(name == nullptr)
  {
    return false;
  }

  for(const VkLayerProperties& layer : layers)
  {
    if(std::strcmp(name, layer.layerName) == 0)
    {
      return true;
    }
  }
  return false;
}

void VulkanDevice::appendFeatureNode(VkBaseOutStructure*& chainHead, void* featureStruct)
{
  if(featureStruct == nullptr)
  {
    return;
  }

  VkBaseOutStructure* node = reinterpret_cast<VkBaseOutStructure*>(featureStruct);
  node->pNext              = chainHead;
  chainHead                = node;
}

VkBool32 VulkanDevice::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                     const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                     void*)
{
  const char* severityName = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0
                                 ? "ERROR"
                                 : ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? "WARNING" : "INFO");
  const char* typeName = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0
                             ? "VALIDATION"
                             : ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0 ? "PERFORMANCE" : "GENERAL");
  LOGE("Vulkan validation %s/%s: %s",
       severityName,
       typeName,
       callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "<null>");
  if((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
  {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__linux__) || defined(__ANDROID__)
    raise(SIGTRAP);
#endif
  }
  return VK_FALSE;
}

namespace {
[[nodiscard]] VkImageViewType toVkImageViewType(ImageViewType type)
{
  switch(type)
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
  switch(aspect)
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
  switch(s)
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
}  // namespace

TextureViewHandle VulkanDevice::createTextureView(const TextureViewCreateDesc& desc)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createTextureView");
  // Prefer the RHI image handle (business layer holds no VkImage); fall back to the legacy
  // nativeImage seam for call sites that still pass a raw VkImage.
  const uint64_t nativeImage = !desc.image.isNull() ? resolveImageNative(desc.image) : desc.nativeImage;
  const VkImageViewCreateInfo info{
      .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image      = reinterpret_cast<VkImage>(static_cast<uintptr_t>(nativeImage)),
      .viewType   = toVkImageViewType(desc.viewType),
      .format     = static_cast<VkFormat>(desc.nativeFormat),
      .components = {toVkSwizzle(desc.components.r), toVkSwizzle(desc.components.g), toVkSwizzle(desc.components.b),
                     toVkSwizzle(desc.components.a)},
      .subresourceRange = {.aspectMask     = toVkImageAspect(desc.aspect),
                           .baseMipLevel   = desc.baseMipLevel,
                           .levelCount     = desc.levelCount,
                           .baseArrayLayer = desc.baseArrayLayer,
                           .layerCount     = desc.layerCount},
  };
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(m_device, &info, nullptr, &view));
  return m_resourceTable->registerTextureView(reinterpret_cast<uint64_t>(view), /*owned=*/true);
}

TextureViewHandle VulkanDevice::registerExternalTextureView(uint64_t nativeView)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before registerExternalTextureView");
  return m_resourceTable->registerTextureView(nativeView, /*owned=*/false);
}

void VulkanDevice::destroyTextureView(TextureViewHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const TextureViewRecord record = m_resourceTable->removeTextureView(handle);
  if(record.owned && record.nativeView != 0)
  {
    vkDestroyImageView(m_device, reinterpret_cast<VkImageView>(static_cast<uintptr_t>(record.nativeView)), nullptr);
  }
}

uint64_t VulkanDevice::resolveTextureViewNative(TextureViewHandle handle) const
{
  return m_resourceTable != nullptr ? m_resourceTable->resolveTextureView(handle) : 0;
}

namespace {
[[nodiscard]] VkImageType toVkImageType(TextureDimension dim)
{
  return dim == TextureDimension::e3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}

[[nodiscard]] VkSampleCountFlagBits toVkSamples(SampleCount count)
{
  switch(count)
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
}  // namespace

TextureHandle VulkanDevice::createImage(const TextureCreateDesc& desc)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createImage");
  assert(m_allocator != nullptr && "VulkanDevice::setAllocator must be called before createImage");
  const VkImageCreateInfo info{
      .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags       = desc.cubeCompatible ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : 0u,
      .imageType   = toVkImageType(desc.dimension),
      .format      = static_cast<VkFormat>(desc.nativeFormat),
      .extent      = {desc.width, desc.height, desc.depth},
      .mipLevels   = desc.mipLevels,
      .arrayLayers = desc.arrayLayers,
      .samples     = toVkSamples(desc.sampleCount),
      .tiling      = VK_IMAGE_TILING_OPTIMAL,
      .usage       = static_cast<VkImageUsageFlags>(desc.nativeUsage),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VkImage       image      = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VK_CHECK(vmaCreateImage(m_allocator, &info, &allocInfo, &image, &allocation, nullptr));
  if(desc.debugName != nullptr)
  {
    utils::DebugUtil::getInstance().setObjectName(image, desc.debugName);
  }
  return m_resourceTable->registerTexture(reinterpret_cast<uint64_t>(image),
                                          reinterpret_cast<uint64_t>(allocation), /*owned=*/true);
}

TextureHandle VulkanDevice::registerExternalTexture(uint64_t nativeImage)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before registerExternalTexture");
  return m_resourceTable->registerTexture(nativeImage, /*nativeAllocation=*/0, /*owned=*/false);
}

void VulkanDevice::destroyImage(TextureHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const TextureRecord record = m_resourceTable->removeTexture(handle);
  if(record.owned && record.nativeImage != 0)
  {
    vmaDestroyImage(m_allocator, reinterpret_cast<VkImage>(static_cast<uintptr_t>(record.nativeImage)),
                    reinterpret_cast<VmaAllocation>(static_cast<uintptr_t>(record.nativeAllocation)));
  }
}

uint64_t VulkanDevice::resolveImageNative(TextureHandle handle) const
{
  return m_resourceTable != nullptr ? m_resourceTable->resolveTexture(handle) : 0;
}

namespace {
[[nodiscard]] VkBufferUsageFlags toVkBufferUsage(BufferUsageFlags flags, bool allowGpuAddress, bool allowIndirect)
{
  VkBufferUsageFlags usage = 0;
  const auto         has   = [&](BufferUsageFlags bit) { return static_cast<uint32_t>(flags & bit) != 0; };
  if(has(BufferUsageFlags::vertex))      usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if(has(BufferUsageFlags::index))       usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if(has(BufferUsageFlags::uniform))     usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if(has(BufferUsageFlags::storage))     usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if(has(BufferUsageFlags::indirect))    usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if(has(BufferUsageFlags::transferSrc)) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if(has(BufferUsageFlags::transferDst)) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if(has(BufferUsageFlags::shaderDeviceAddress) || allowGpuAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if(allowIndirect) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  return usage;
}

[[nodiscard]] VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage)
{
  switch(usage)
  {
    case MemoryUsage::cpuToGpu:            return VMA_MEMORY_USAGE_CPU_TO_GPU;
    case MemoryUsage::gpuToCpu:            return VMA_MEMORY_USAGE_GPU_TO_CPU;
    case MemoryUsage::transientAttachment: return VMA_MEMORY_USAGE_GPU_ONLY;
    default:                               return VMA_MEMORY_USAGE_GPU_ONLY;
  }
}

[[nodiscard]] bool isCpuVisible(MemoryUsage usage)
{
  return usage == MemoryUsage::cpuToGpu || usage == MemoryUsage::gpuToCpu;
}

[[nodiscard]] VkSamplerAddressMode toVkAddressMode(AddressMode mode)
{
  switch(mode)
  {
    case AddressMode::clampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::clampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case AddressMode::mirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:                          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}
}  // namespace

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createBuffer");
  assert(m_allocator != nullptr && "VulkanDevice::setAllocator must be called before createBuffer");

  const VkBufferCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size  = desc.size,
      .usage = toVkBufferUsage(desc.usage, desc.allowGpuAddress, desc.allowIndirectArgument),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocInfo{.usage = toVmaMemoryUsage(desc.memoryUsage)};
  if(isCpuVisible(desc.memoryUsage))
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  VkBuffer          buffer     = VK_NULL_HANDLE;
  VmaAllocation     allocation = nullptr;
  VmaAllocationInfo allocResult{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &info, &allocInfo, &buffer, &allocation, &allocResult));
  if(desc.debugName != nullptr)
  {
    utils::DebugUtil::getInstance().setObjectName(buffer, desc.debugName);
  }

  uint64_t gpuAddress = 0;
  if(desc.allowGpuAddress || static_cast<uint32_t>(desc.usage & BufferUsageFlags::shaderDeviceAddress) != 0)
  {
    const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    gpuAddress = vkGetBufferDeviceAddress(m_device, &addressInfo);
  }

  return m_resourceTable->registerBuffer(BufferRecord{
      .nativeBuffer     = reinterpret_cast<uint64_t>(buffer),
      .nativeAllocation = reinterpret_cast<uint64_t>(allocation),
      .gpuAddress       = gpuAddress,
      .mapped           = allocResult.pMappedData,
      .owned            = true,
  });
}

void VulkanDevice::destroyBuffer(BufferHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const BufferRecord record = m_resourceTable->removeBuffer(handle);
  if(record.owned && record.nativeBuffer != 0)
  {
    vmaDestroyBuffer(m_allocator, reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(record.nativeBuffer)),
                     reinterpret_cast<VmaAllocation>(static_cast<uintptr_t>(record.nativeAllocation)));
  }
}

GpuPtr VulkanDevice::getBufferGpuAddress(BufferHandle handle) const
{
  if(m_resourceTable == nullptr)
  {
    return {};
  }
  const BufferRecord* record = m_resourceTable->tryGetBuffer(handle);
  return record != nullptr ? GpuPtr{record->gpuAddress} : GpuPtr{};
}

void* VulkanDevice::mapBuffer(BufferHandle handle)
{
  if(m_resourceTable == nullptr)
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
      .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter        = static_cast<VkFilter>(desc.magFilter),
      .minFilter        = static_cast<VkFilter>(desc.minFilter),
      .mipmapMode       = static_cast<VkSamplerMipmapMode>(desc.mipmapMode),
      .addressModeU     = toVkAddressMode(desc.addressModeU),
      .addressModeV     = toVkAddressMode(desc.addressModeV),
      .addressModeW     = toVkAddressMode(desc.addressModeW),
      .mipLodBias       = desc.mipLodBias,
      .anisotropyEnable = desc.anisotropyEnable ? VK_TRUE : VK_FALSE,
      .maxAnisotropy    = desc.maxAnisotropy,
      .compareEnable    = desc.compareEnable ? VK_TRUE : VK_FALSE,
      .compareOp        = static_cast<VkCompareOp>(desc.compareOp),
      .minLod           = desc.minLod,
      .maxLod           = desc.maxLod,
  };
  VkSampler sampler = VK_NULL_HANDLE;
  VK_CHECK(vkCreateSampler(m_device, &info, nullptr, &sampler));
  if(desc.debugName != nullptr)
  {
    utils::DebugUtil::getInstance().setObjectName(sampler, desc.debugName);
  }
  return m_resourceTable->registerSampler(reinterpret_cast<uint64_t>(sampler));
}

void VulkanDevice::destroySampler(SamplerHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const SamplerRecord record = m_resourceTable->removeSampler(handle);
  if(record.nativeSampler != 0)
  {
    vkDestroySampler(m_device, reinterpret_cast<VkSampler>(static_cast<uintptr_t>(record.nativeSampler)), nullptr);
  }
}

QueryPoolHandle VulkanDevice::createQueryPool(uint32_t queryCount)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createQueryPool");
  const VkQueryPoolCreateInfo info{
      .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType  = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = queryCount,
  };
  VkQueryPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateQueryPool(m_device, &info, nullptr, &pool));
  return m_resourceTable->registerQueryPool(reinterpret_cast<uint64_t>(pool), queryCount);
}

void VulkanDevice::destroyQueryPool(QueryPoolHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const QueryPoolRecord record = m_resourceTable->removeQueryPool(handle);
  if(record.nativePool != 0)
  {
    vkDestroyQueryPool(m_device, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(record.nativePool)), nullptr);
  }
}

uint64_t VulkanDevice::getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex)
{
  if(m_resourceTable == nullptr)
  {
    return 0;
  }
  const uint64_t nativePool = m_resourceTable->resolveQueryPool(handle);
  if(nativePool == 0)
  {
    return 0;
  }
  uint64_t result = 0;
  vkGetQueryPoolResults(m_device, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(nativePool)), queryIndex, 1,
                        sizeof(result), &result, sizeof(result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  return result;
}

bool VulkanDevice::getQueryPoolResultsWithAvailability(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, uint64_t* outPairs)
{
  if(m_resourceTable == nullptr || outPairs == nullptr || queryCount == 0)
  {
    return false;
  }
  const uint64_t nativePool = m_resourceTable->resolveQueryPool(handle);
  if(nativePool == 0)
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

namespace {
[[nodiscard]] VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages)
{
  VkShaderStageFlags flags = 0;
  const auto         has   = [&](ShaderStage bit) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(bit)) != 0; };
  if(has(ShaderStage::vertex))      flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if(has(ShaderStage::fragment))    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if(has(ShaderStage::compute))     flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  if(has(ShaderStage::geometry))    flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  if(has(ShaderStage::tessControl)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if(has(ShaderStage::tessEval))    flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  if(flags == 0) flags = VK_SHADER_STAGE_ALL;
  return flags;
}

[[nodiscard]] VkDescriptorType toVkDescriptorType(ArgumentType type, bool dynamicOffset)
{
  switch(type)
  {
    case ArgumentType::uniformBuffer:         return dynamicOffset ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case ArgumentType::storageBuffer:         return dynamicOffset ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case ArgumentType::sampledTexture:        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case ArgumentType::storageTexture:        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case ArgumentType::sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
    case ArgumentType::combinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case ArgumentType::accelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    default:                                  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
}
}  // namespace

ArgumentLayoutHandle VulkanDevice::createArgumentLayout(const ArgumentLayoutDesc& desc)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentLayout");
  std::vector<VkDescriptorSetLayoutBinding> bindings(desc.bindingCount);
  std::vector<uint32_t>                     dynamicBindings;
  for(uint32_t i = 0; i < desc.bindingCount; ++i)
  {
    const ArgumentBinding& b = desc.bindings[i];
    bindings[i]              = VkDescriptorSetLayoutBinding{
                     .binding         = b.binding,
                     .descriptorType  = toVkDescriptorType(b.type, b.dynamicOffset),
                     .descriptorCount = b.arrayCount,
                     .stageFlags      = toVkShaderStageFlags(b.visibility),
    };
    if(b.dynamicOffset)
    {
      dynamicBindings.push_back(b.binding);
    }
  }
  const VkDescriptorSetLayoutCreateInfo info{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.empty() ? nullptr : bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout));
  return m_resourceTable->registerArgumentLayout(reinterpret_cast<uint64_t>(layout), std::move(dynamicBindings));
}

void VulkanDevice::destroyArgumentLayout(ArgumentLayoutHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const ArgumentLayoutRecord record = m_resourceTable->removeArgumentLayout(handle);
  if(record.nativeLayout != 0)
  {
    vkDestroyDescriptorSetLayout(m_device, reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(record.nativeLayout)), nullptr);
  }
}

ArgumentTableHandle VulkanDevice::createArgumentTable(ArgumentLayoutHandle layout)
{
  assert(m_resourceTable != nullptr && "VulkanDevice::setResourceTable must be called before createArgumentTable");
  if(m_argumentPool == VK_NULL_HANDLE)
  {
    const std::array<VkDescriptorPoolSize, 8> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
    }};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 4096,
        .poolSizeCount = static_cast<uint32_t>(sizes.size()),
        .pPoolSizes    = sizes.data(),
    };
    VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_argumentPool));
  }

  const uint64_t nativeLayout = m_resourceTable->resolveArgumentLayout(layout);
  assert(nativeLayout != 0 && "createArgumentTable requires a valid argument layout");
  VkDescriptorSetLayout       setLayout = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(nativeLayout));
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_argumentPool,
      .descriptorSetCount = 1,
      .pSetLayouts        = &setLayout,
  };
  VkDescriptorSet set = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &set));
  return m_resourceTable->registerArgumentTable(reinterpret_cast<uint64_t>(set), layout);
}

void VulkanDevice::destroyArgumentTable(ArgumentTableHandle handle)
{
  if(handle.isNull() || m_resourceTable == nullptr)
  {
    return;
  }
  const ArgumentTableRecord record = m_resourceTable->removeArgumentTable(handle);
  if(record.owned && record.nativeSet != 0 && m_argumentPool != VK_NULL_HANDLE)
  {
    VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(record.nativeSet));
    vkFreeDescriptorSets(m_device, m_argumentPool, 1, &set);
  }
}

void VulkanDevice::updateArgumentTable(ArgumentTableHandle table, uint32_t writeCount, const ArgumentWrite* writes)
{
  if(m_resourceTable == nullptr || writeCount == 0 || writes == nullptr)
  {
    return;
  }
  const ArgumentTableRecord* tableRecord = m_resourceTable->tryGetArgumentTable(table);
  if(tableRecord == nullptr || tableRecord->nativeSet == 0)
  {
    return;
  }
  VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(tableRecord->nativeSet));

  // Per-binding dynamic-ness comes from the layout: a write's descriptorType must
  // match the layout, so dynamic UBO/SSBO bindings need the *_DYNAMIC variant.
  const ArgumentLayoutRecord* layoutRecord = m_resourceTable->tryGetArgumentLayout(tableRecord->layout);
  const auto isDynamicBinding = [layoutRecord](uint32_t binding) -> bool {
    if(layoutRecord == nullptr)
    {
      return false;
    }
    for(uint32_t dyn : layoutRecord->dynamicBindings)
    {
      if(dyn == binding)
      {
        return true;
      }
    }
    return false;
  };

  std::vector<VkDescriptorBufferInfo> bufferInfos(writeCount);
  std::vector<VkDescriptorImageInfo>  imageInfos(writeCount);
  std::vector<VkWriteDescriptorSet>   vkWrites(writeCount);
  for(uint32_t i = 0; i < writeCount; ++i)
  {
    const ArgumentWrite& w    = writes[i];
    const VkDescriptorType type = toVkDescriptorType(w.type, isDynamicBinding(w.binding));
    VkWriteDescriptorSet&  out  = vkWrites[i];
    out = VkWriteDescriptorSet{
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = set,
        .dstBinding      = w.binding,
        .dstArrayElement = w.arrayElement,
        .descriptorCount = 1,
        .descriptorType  = type,
    };
    switch(w.type)
    {
      case ArgumentType::sampledTexture:
      case ArgumentType::storageTexture:
        imageInfos[i] = VkDescriptorImageInfo{
            .imageView   = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_resourceTable->resolveTextureView(w.textureView))),
            // storageTexture is always GENERAL; sampledTexture defaults to SHADER_READ_ONLY_OPTIMAL
            // but honors w.imageLayout == General for images sampled while kept in GENERAL.
            .imageLayout = (w.type == ArgumentType::storageTexture || w.imageLayout == ResourceState::General)
                               ? VK_IMAGE_LAYOUT_GENERAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        out.pImageInfo = &imageInfos[i];
        break;
      case ArgumentType::sampler:
        imageInfos[i]  = VkDescriptorImageInfo{.sampler = reinterpret_cast<VkSampler>(static_cast<uintptr_t>(m_resourceTable->resolveSampler(w.sampler)))};
        out.pImageInfo = &imageInfos[i];
        break;
      case ArgumentType::combinedImageSampler:
        imageInfos[i] = VkDescriptorImageInfo{
            .sampler     = reinterpret_cast<VkSampler>(static_cast<uintptr_t>(m_resourceTable->resolveSampler(w.sampler))),
            .imageView   = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_resourceTable->resolveTextureView(w.textureView))),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        out.pImageInfo = &imageInfos[i];
        break;
      default:  // buffer types
        bufferInfos[i] = VkDescriptorBufferInfo{
            .buffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(m_resourceTable->resolveBuffer(w.buffer))),
            .offset = w.offset,
            .range  = w.size == 0 ? VK_WHOLE_SIZE : w.size,
        };
        out.pBufferInfo = &bufferInfos[i];
        break;
    }
  }
  vkUpdateDescriptorSets(m_device, writeCount, vkWrites.data(), 0, nullptr);
}

}  // namespace demo::rhi::vulkan
