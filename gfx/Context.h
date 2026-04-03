#pragma once

#include "../common/Common.h"

namespace utils {

class Context
{
public:
  Context() = default;
  ~Context() { assert(m_device == VK_NULL_HANDLE && "Missing destroy()"); }

  void init(const ContextCreateInfo& createInfo)
  {
    m_createInfo = createInfo;
    initInstance();
    selectPhysicalDevice();
    initLogicalDevice();
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    vkDeviceWaitIdle(m_device);
    if(m_createInfo.enableValidationLayers && vkDestroyDebugUtilsMessengerEXT)
    {
      vkDestroyDebugUtilsMessengerEXT(m_instance, m_callback, nullptr);
    }
    vkDestroyDevice(m_device, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    *this = {};
  }

  VkDevice         getDevice() const { return m_device; }
  VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
  VkInstance       getInstance() const { return m_instance; }
  const QueueInfo& getGraphicsQueue() const { return m_queues[0]; }
  uint32_t         getApiVersion() const { return m_apiVersion; }


private:
  //--- Vulkan Debug ------------------------------------------------------------------------------------------------------------

  /*-- Callback function to catch validation errors  -*/
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                      void*)
  {
    const Logger::LogLevel level =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0   ? Logger::LogLevel::eERROR :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? Logger::LogLevel::eWARNING :
                                                                            Logger::LogLevel::eINFO;
    Logger::getInstance().log(level, "%s", callbackData->pMessage);
    if((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
#if defined(_MSVC_LANG)
      __debugbreak();
#elif defined(__linux__)
      raise(SIGTRAP);
#endif
    }
    return VK_FALSE;
  }

  void initInstance()
  {
    vkEnumerateInstanceVersion(&m_apiVersion);
    LOGI("VULKAN API: %d.%d", VK_VERSION_MAJOR(m_apiVersion), VK_VERSION_MINOR(m_apiVersion));
    ASSERT(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 loader");

    // This finds the KHR surface extensions needed to display on the right platform
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions     = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    getAvailableInstanceExtensions();

    const VkApplicationInfo applicationInfo{
        .pApplicationName   = "minimal_latest",
        .applicationVersion = 1,
        .pEngineName        = "minimal_latest",
        .engineVersion      = 1,
        .apiVersion         = m_apiVersion,
    };

    // Build instance extensions list from config
    std::vector<const char*> instanceExtensions = m_createInfo.instanceExtensions;

    // Add extensions requested by GLFW (required for windowing)
    for(uint32_t i = 0; i < glfwExtensionCount; i++)
    {
      instanceExtensions.push_back(glfwExtensions[i]);
    }

    // Add optional instance extensions if available
    if(extensionIsAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, m_instanceExtensionsAvailable))
      instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if(extensionIsAvailable(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, m_instanceExtensionsAvailable))
      instanceExtensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

    // Build instance layers list from config
    std::vector<const char*> instanceLayers = m_createInfo.instanceLayers;

    // Adding the validation layer
    if(m_createInfo.enableValidationLayers)
    {
      instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
    }

    // Setting for the validation layer
    ValidationSettings validationSettings{.validate_core = VK_TRUE};  // modify default value

    const VkInstanceCreateInfo instanceCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = validationSettings.buildPNextChain(),
        .pApplicationInfo        = &applicationInfo,
        .enabledLayerCount       = uint32_t(instanceLayers.size()),
        .ppEnabledLayerNames     = instanceLayers.data(),
        .enabledExtensionCount   = uint32_t(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    };

    // Actual Vulkan instance creation
    VK_CHECK(vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance));

    // Load all Vulkan functions
    volkLoadInstance(m_instance);

    // Add the debug callback
    if(m_createInfo.enableValidationLayers && vkCreateDebugUtilsMessengerEXT)
    {
      const VkDebugUtilsMessengerCreateInfoEXT dbg_messenger_create_info{
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
          .pfnUserCallback = Context::debugCallback,  // <-- The callback function
      };
      VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_instance, &dbg_messenger_create_info, nullptr, &m_callback));
      LOGI("Validation Layers: ON");
    }
  }

  /*--
   * The physical device is the GPU that is used to render the scene.
   * We are selecting the first discrete GPU found, if there is one.
  -*/
  void selectPhysicalDevice()
  {
    size_t chosenDevice = 0;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    ASSERT(deviceCount != 0, "failed to find GPUs with Vulkan support!");

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data());

    VkPhysicalDeviceProperties2 properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    for(size_t i = 0; i < physicalDevices.size(); i++)
    {
      vkGetPhysicalDeviceProperties2(physicalDevices[i], &properties2);
      if(properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      {
        chosenDevice = i;
        break;
      }
    }

    m_physicalDevice = physicalDevices[chosenDevice];
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties2);
    LOGI("Selected GPU: %s", properties2.properties.deviceName);  // Show the name of the GPU
    LOGI("Driver: %d.%d.%d", VK_VERSION_MAJOR(properties2.properties.driverVersion),
         VK_VERSION_MINOR(properties2.properties.driverVersion), VK_VERSION_PATCH(properties2.properties.driverVersion));
    LOGI("Vulkan API: %d.%d.%d", VK_VERSION_MAJOR(properties2.properties.apiVersion),
         VK_VERSION_MINOR(properties2.properties.apiVersion), VK_VERSION_PATCH(properties2.properties.apiVersion));
    ASSERT(properties2.properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 device, update driver!");
  }

  /*--
   * The queue is used to submit command buffers to the GPU.
   * We are selecting the first queue found (graphic), which is the most common and needed for rendering graphic elements.
   * 
   * Other types of queues are used for compute, transfer, and other types of operations.
   * In a more advanced application, the user should select the queue that fits the application needs.
   * 
   * Eventually the user should create multiple queues for different types of operations.
   * 
   * Note: The queue is created with the creation of the logical device, this is the selection which are requested when creating the logical device.
   * Note: the search of the queue could be more advanced, and search for the right queue family.
  -*/
  QueueInfo getQueue(VkQueueFlagBits flags) const
  {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties2> queueFamilies(queueFamilyCount, {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    QueueInfo queueInfo;
    for(uint32_t i = 0; i < queueFamilies.size(); i++)
    {
      if(queueFamilies[i].queueFamilyProperties.queueFlags & flags)
      {
        queueInfo.familyIndex = i;
        queueInfo.queueIndex  = 0;  // A second graphic queue could be index 1 (need logic to find the right one)
        // m_queueInfo.queue = After creating the logical device
        break;
      }
    }
    return queueInfo;
  }

  /*--
   * The logical device is the interface to the physical device.
   * It is used to create resources, allocate memory, and submit command buffers to the GPU.
   * The logical device is created with the physical device and the queue family that is used.
   * The logical device is created with the extensions and features configured in ContextCreateInfo.
  -*/
  void initLogicalDevice()
  {
    const float queuePriority = 1.0F;
    m_queues.clear();
    m_queues.emplace_back(getQueue(VK_QUEUE_GRAPHICS_BIT));

    // Request only one queue : graphic
    // User could request more specific queues: compute, transfer
    const VkDeviceQueueCreateInfo queueCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_queues[0].familyIndex,
        .queueCount       = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Chaining all features up to Vulkan 1.4
    pNextChainPushFront(&m_features11, &m_features12);
    pNextChainPushFront(&m_features11, &m_features13);
    pNextChainPushFront(&m_features11, &m_features14);

    /*-- 
     * Process device extensions from configuration:
     * - Check if each extension is available on the device
     * - Enable required extensions (assert if not available)
     * - Enable optional extensions (skip if not available)
     * - Link provided feature structs to the pNext chain
    -*/
    getAvailableDeviceExtensions();

    std::vector<const char*> deviceExtensions;
    for(const auto& extConfig : m_createInfo.deviceExtensions)
    {
      if(extensionIsAvailable(extConfig.name, m_deviceExtensionsAvailable))
      {
        deviceExtensions.push_back(extConfig.name);

        // Link feature struct if provided via ExtensionConfig::featureStruct
        if(extConfig.featureStruct != nullptr)
        {
          pNextChainPushFront(&m_features11, extConfig.featureStruct);
        }
      }
      else if(extConfig.required)
      {
        // Extension is required but not available - fail with error message
        LOGE("Required extension %s is not available!", extConfig.name);
        ASSERT(false, "Required device extension not available, update driver!");
      }
      else
      {
        // Extension is optional and not available - skip it
        LOGW("Optional extension %s is not available, skipping", extConfig.name);
      }
    }

    // Requesting all supported features, which will then be activated in the device
    m_deviceFeatures.pNext = &m_features11;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &m_deviceFeatures);

    // Validate required features - these are mandatory in Vulkan 1.4, but some drivers
    // claim 1.4 support without full conformance. Check to catch non-conformant drivers early.
    ASSERT(m_features12.timelineSemaphore, "Timeline semaphore required (Vulkan 1.2 core)");
    ASSERT(m_features13.synchronization2, "Synchronization2 required (Vulkan 1.3 core)");
    ASSERT(m_features13.dynamicRendering, "Dynamic rendering required (Vulkan 1.3 core)");
    ASSERT(m_features14.maintenance5, "Maintenance5 required (Vulkan 1.4 core)");
    ASSERT(m_features14.maintenance6, "Maintenance6 required (Vulkan 1.4 core)");

    // Get information about what the device can do
    VkPhysicalDeviceProperties2 deviceProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    deviceProperties.pNext = &m_pushDescriptorProperties;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &deviceProperties);

    // Create the logical device
    const VkDeviceCreateInfo deviceCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &m_deviceFeatures,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queueCreateInfo,
        .enabledExtensionCount   = uint32_t(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    VK_CHECK(vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device));
    DBG_VK_NAME(m_device);

    volkLoadDevice(m_device);  // Load all Vulkan device functions

    // Debug utility to name Vulkan objects, great in debugger like NSight
    debugUtilInitialize(m_device);

    // Get the requested queues
    vkGetDeviceQueue(m_device, m_queues[0].familyIndex, m_queues[0].queueIndex, &m_queues[0].queue);
    DBG_VK_NAME(m_queues[0].queue);

    // Log the enabled extensions
    LOGI("Enabled device extensions:");
    for(const auto& ext : deviceExtensions)
    {
      LOGI("  %s", ext);
    }
  }

  /*-- 
   * Get all available extensions for the device, because we cannot request an extension that isn't 
   * supported/available. If we do, the logical device creation would fail. 
  -*/
  void getAvailableDeviceExtensions()
  {
    uint32_t count{0};
    VK_CHECK(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr));
    m_deviceExtensionsAvailable.resize(count);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, m_deviceExtensionsAvailable.data()));
  }

  void getAvailableInstanceExtensions()
  {
    uint32_t count{0};
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    m_instanceExtensionsAvailable.resize(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, m_instanceExtensionsAvailable.data());
  }

  // Work in conjunction with the above
  bool extensionIsAvailable(const std::string& name, const std::vector<VkExtensionProperties>& extensions)
  {
    for(auto& ext : extensions)
    {
      if(name == ext.extensionName)
        return true;
    }
    return false;
  }


  // --- Members ------------------------------------------------------------------------------------------------------------
  ContextCreateInfo m_createInfo{};   // Configuration provided during init()
  uint32_t          m_apiVersion{0};  // The Vulkan API version

  // Properties: how much a feature can do
  VkPhysicalDevicePushDescriptorProperties m_pushDescriptorProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES};

  std::vector<VkBaseOutStructure*> m_linkedDeviceProperties{reinterpret_cast<VkBaseOutStructure*>(&m_pushDescriptorProperties)};

  VkInstance                         m_instance{};        // The Vulkan instance
  VkPhysicalDevice                   m_physicalDevice{};  // The physical device (GPU)
  VkDevice                           m_device{};          // The logical device (interface to the physical device)
  std::vector<QueueInfo>             m_queues;            // The queue used to submit command buffers to the GPU
  VkDebugUtilsMessengerEXT           m_callback{VK_NULL_HANDLE};  // The debug callback
  std::vector<VkExtensionProperties> m_instanceExtensionsAvailable;
  std::vector<VkExtensionProperties> m_deviceExtensionsAvailable;

  // Core features
  VkPhysicalDeviceFeatures2        m_deviceFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features m_features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features m_features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features m_features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features m_features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
};

}
