#pragma once

#include "../RHIDevice.h"
#include "../RHIResourceLifetime.h"
#include "VulkanDeviceCreateInfo.h"
#include "VulkanDeviceInterop.h"
#include "VulkanQueue.h"

#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

// Forward declaration of the VMA allocator handle (typedef-compatible with vk_mem_alloc.h's
// `typedef struct VmaAllocator_T* VmaAllocator;`).
using VmaAllocator = struct VmaAllocator_T*;

namespace demo::rhi::vulkan {

class VulkanResourceTable;

class VulkanDevice final : public demo::rhi::Device, public VulkanDeviceInterop
{
public:
  VulkanDevice();
  ~VulkanDevice() override;

  void init(const DeviceCreateInfo& createInfo) override;
  // initVulkan accepts the full Vulkan-specific create info (D-08 transition).
  // RenderDevice calls this instead of init() to supply extension/layer requests.
  // init() delegates to initVulkan() with only the base fields (no Vulkan extensions).
  void initVulkan(const VulkanDeviceCreateInfo& createInfo);
  void deinit() override;

  BackendInfo               getBackendInfo() const override;
  const char*               getDeviceName() const override;
  const PhysicalDeviceInfo& getPhysicalDeviceInfo() const override;
  const DeviceFeatureInfo&  getEnabledFeatureInfo() const override;
  CapabilityReport          queryCapabilities() const override;
  bool                      supports(CapabilityTier tier) const override;
  const MemoryProperties&   getPhysicalMemoryProperties() const override;

  Queue* getQueue(QueueClass queueClass) override;
  std::unique_ptr<CommandAllocator> createCommandAllocator(QueueClass queueClass) override;
  void collectGarbage() override;

  void initSurface(Surface& surface, const WindowHandle& window) override;
  std::unique_ptr<Swapchain> createSwapchain(Surface& surface, bool vSync) override;
  float getTimestampPeriodNs() const override;



  bool isFormatSupported(TextureFormat format, FormatFeatureFlag feature) const override;

  void waitIdle() override;

  // --- Immediate upload seam (UPL-02) ---

  TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) override;
  void              destroyTextureView(TextureViewHandle handle) override;

  TextureHandle createTexture(const TextureDesc& desc) override;
  void          destroyTexture(TextureHandle handle) override;


  // --- Modern GPU interface (Wave 1: buffers / samplers / query pools) ---
  BufferHandle createBuffer(const BufferDesc& desc) override;
  void         destroyBuffer(BufferHandle handle) override;
  GpuPtr       getBufferGpuAddress(BufferHandle handle) const override;
  void*        mapBuffer(BufferHandle handle) override;
  void         unmapBuffer(BufferHandle handle) override;
  Result<MappedBufferRange> mapBufferRange(BufferHandle handle, const BufferMapDesc& desc) override;
  RHIResult flushMappedBufferRange(BufferHandle handle, uint64_t offset, uint64_t size) override;
  RHIResult invalidateMappedBufferRange(BufferHandle handle, uint64_t offset, uint64_t size) override;

  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void          destroySampler(SamplerHandle handle) override;

  QueryPoolHandle createQueryPool(uint32_t queryCount) override;
  void            destroyQueryPool(QueryPoolHandle handle) override;
  uint64_t        getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex) override;
  bool getQueryPoolResultsWithAvailability(QueryPoolHandle handle, uint32_t firstQuery, std::span<uint64_t> outValueAvailabilityPairs) override;

  ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc) override;
  void                 destroyArgumentLayout(ArgumentLayoutHandle handle) override;
  ArgumentTableHandle  createArgumentTable(const ArgumentTableCreateDesc& desc) override;
  void                 destroyArgumentTable(ArgumentTableHandle handle) override;
  void                 updateArgumentTable(ArgumentTableHandle table, ArgumentWriteBatch writes) override;
  ArgumentLayoutHandle getArgumentTableLayout(ArgumentTableHandle table) const override;
  PipelineHandle       createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  PipelineHandle       createComputePipeline(const ComputePipelineDesc& desc) override;
  void                 destroyPipeline(PipelineHandle handle) override;
  ShaderLibraryHandle  createShaderLibrary(const ShaderLibraryDesc& desc) override;
  void                 destroyShaderLibrary(ShaderLibraryHandle handle) override;
  Result<ResidencySetHandle> createResidencySet(const ResidencySetDesc& desc) override;
  RHIResult destroyResidencySet(ResidencySetHandle handle) override;
  RHIResult updateResidencySet(ResidencySetHandle handle, const ResidencyUpdateBatch& batch) override;
  // The VMA allocator is created and owned backend-internally (RDEV-06).

  VkInstance       instance() const { return m_instance; }
  VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
  VkDevice         device() const { return m_device; }

  // -------------------------------------------------------------------------
  // VulkanDeviceInterop overrides (D-07 backend-internal native accessor)
  // -------------------------------------------------------------------------
  VkInstance          nativeInstance() const override { return m_instance; }
  VkPhysicalDevice    nativePhysicalDevice() const override { return m_physicalDevice; }
  VkDevice            nativeDevice() const override { return m_device; }
  VkBaseOutStructure* nativeFeaturesChainHead() const override { return m_featuresChainHead; }

  VkImage     resolveTexture(rhi::TextureHandle handle) const override;
  VkImageView resolveTextureView(rhi::TextureViewHandle handle) const override;
  VkSampler   resolveSampler(rhi::SamplerHandle handle) const override;

  uint32_t processRetirements();

private:
  struct NativeRetirement
  {
    ResourceHandle resource{};
    SubmissionTokenSet retirementDependencies{};
    uint64_t       nativeObject{0};
    uint64_t       nativeAllocation{0};
    uint64_t       secondaryNativeObject{0};
    bool           owned{false};
    bool           ownsSecondary{false};
    bool           preciseDependencies{false};
  };

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

  [[nodiscard]] SubmissionTokenSet retirementDependencies() const;
  [[nodiscard]] bool isRetirementComplete(const SubmissionTokenSet& dependencies) const;
  void enqueueRetirement(NativeRetirement retirement);
  void     destroyRetiredResource(const NativeRetirement& retirement);
  void     drainRetirements();
  void     releaseResourceTableObjects();

  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT             type,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                      void*                                       userData);

  VulkanDeviceCreateInfo m_createInfo{};

  VkInstance               m_instance{VK_NULL_HANDLE};
  VkPhysicalDevice         m_physicalDevice{VK_NULL_HANDLE};
  VkDevice                 m_device{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};

  NativeQueueInfo m_graphicsQueue{};
  NativeQueueInfo m_computeQueue{};
  NativeQueueInfo m_transferQueue{};

  std::unique_ptr<VulkanQueueSyncRegistry> m_queueSyncRegistry;
  std::unique_ptr<VulkanQueue> m_graphicsQueueApi;
  std::unique_ptr<VulkanQueue> m_computeQueueApi;
  std::unique_ptr<VulkanQueue> m_transferQueueApi;

  uint32_t           m_queueGeneration{0};
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
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
  VkPhysicalDeviceVulkan14Features m_features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#else
  VkPhysicalDeviceMaintenance5FeaturesKHR m_maintenance5Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
  VkPhysicalDeviceMaintenance6FeaturesKHR m_maintenance6Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
#endif
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

  std::unique_ptr<VulkanResourceTable> m_resourceTable;
  VmaAllocator         m_allocator{nullptr};
  VkDescriptorPool     m_argumentPool{VK_NULL_HANDLE};  // active pool for new argument tables
  uint32_t             m_combinedImageSamplerPoolCapacity{16384};
  std::vector<VkDescriptorPool> m_argumentPools;
  std::unordered_map<uint64_t, VkDescriptorPool> m_argumentSetPools;
  std::vector<NativeRetirement> m_pendingRetirements;


};

}  // namespace demo::rhi::vulkan
