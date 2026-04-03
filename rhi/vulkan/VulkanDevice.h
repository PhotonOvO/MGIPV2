#pragma once

#include "../RHIDevice.h"

#include <vector>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

class VulkanDevice final : public demo::rhi::Device
{
public:
  VulkanDevice() = default;
  ~VulkanDevice() override;

  void init(const DeviceCreateInfo& createInfo) override;
  void deinit() override;

  uint64_t getNativeInstance() const override;
  uint64_t getNativePhysicalDevice() const override;
  uint64_t getNativeDevice() const override;

  uint32_t                  getApiVersion() const override;
  const char*               getDeviceName() const override;
  const PhysicalDeviceInfo& getPhysicalDeviceInfo() const override;
  const DeviceFeatureInfo&  getEnabledFeatureInfo() const override;
  CapabilityReport          queryCapabilities() const override;
  bool                      supports(CapabilityTier tier) const override;
  const MemoryProperties&   getPhysicalMemoryProperties() const override;
  void*                     getFeaturesChainHead() const override;

  QueueInfo getGraphicsQueue() const override;
  QueueInfo getComputeQueue() const override;
  QueueInfo getTransferQueue() const override;

  bool isInstanceExtensionSupported(const char* name) const override;
  bool isDeviceExtensionSupported(const char* name) const override;

  void waitIdle() override;

  VkInstance       instance() const { return m_instance; }
  VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
  VkDevice         device() const { return m_device; }

private:
  struct NativeQueueInfo
  {
    VkQueue   queue{VK_NULL_HANDLE};
    uint32_t  familyIndex{~0u};
    uint32_t  queueIndex{0};
    uint32_t  queueCount{0};
    QueueInfo toRhi() const;
  };

  void initInstance();
  void selectPhysicalDevice();
  void initLogicalDevice();
  void initDebugMessenger();
  void destroyDebugMessenger();

  void               queryInstanceExtensions();
  void               queryInstanceLayers();
  void               queryDeviceExtensions();
  void               queryMemoryProperties();
  void               selectQueues();
  void               detectCapabilities();
  RHICapabilityError validateCapabilities() const;

  static bool extensionAvailable(const char* name, const std::vector<VkExtensionProperties>& extensions);
  static bool layerAvailable(const char* name, const std::vector<VkLayerProperties>& layers);
  static void appendFeatureNode(VkBaseOutStructure*& chainHead, void* featureStruct);

  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT             type,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                      void*                                       userData);

  DeviceCreateInfo m_createInfo{};

  VkInstance               m_instance{VK_NULL_HANDLE};
  VkPhysicalDevice         m_physicalDevice{VK_NULL_HANDLE};
  VkDevice                 m_device{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};

  NativeQueueInfo m_graphicsQueue{};
  NativeQueueInfo m_computeQueue{};
  NativeQueueInfo m_transferQueue{};

  uint32_t           m_apiVersion{0};
  PhysicalDeviceInfo m_physicalDeviceInfo{};
  DeviceFeatureInfo  m_featureInfo{};
  CapabilityReport   m_capabilities{};
  RHICapabilityError m_capabilityError{RHICapabilityError::None};
  MemoryProperties   m_memoryProperties{};

  VkPhysicalDeviceProperties2      m_vkProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  VkPhysicalDeviceMemoryProperties m_vkMemoryProperties{};
  VkPhysicalDeviceFeatures2        m_deviceFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features m_features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features m_features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features m_features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features m_features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  VkPhysicalDeviceMeshShaderFeaturesEXT m_meshShaderFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR m_accelerationStructureFeatures{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR m_rayTracingPipelineFeatures{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  VkBaseOutStructure*                m_featuresChainHead{nullptr};
  std::vector<VkExtensionProperties> m_availableInstanceExtensions;
  std::vector<VkLayerProperties>     m_availableInstanceLayers;
  std::vector<VkExtensionProperties> m_availableDeviceExtensions;
  std::vector<const char*>           m_enabledDeviceExtensions;
  bool                               m_initialized{false};
};

}  // namespace demo::rhi::vulkan
