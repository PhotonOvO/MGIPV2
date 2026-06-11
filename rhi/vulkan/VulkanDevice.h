#pragma once

#include "../RHIDevice.h"
#include "../RHIResourceLifetime.h"
#include "VulkanDeviceCreateInfo.h"
#include "VulkanDeviceInterop.h"

#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

// Forward declaration of the VMA allocator handle (typedef-compatible with vk_mem_alloc.h's
// `typedef struct VmaAllocator_T* VmaAllocator;`).
using VmaAllocator = struct VmaAllocator_T*;

namespace demo::rhi::vulkan {

class VulkanResourceTable;
class VulkanFrameContext;

class VulkanDevice final : public demo::rhi::Device, public VulkanDeviceInterop
{
public:
  VulkanDevice() = default;
  ~VulkanDevice() override;

  void init(const DeviceCreateInfo& createInfo) override;
  // initVulkan accepts the full Vulkan-specific create info (D-08 transition).
  // RenderDevice calls this instead of init() to supply extension/layer requests.
  // init() delegates to initVulkan() with only the base fields (no Vulkan extensions).
  void initVulkan(const VulkanDeviceCreateInfo& createInfo);
  void deinit() override;

  uint32_t                  getApiVersion() const override;
  const char*               getDeviceName() const override;
  const PhysicalDeviceInfo& getPhysicalDeviceInfo() const override;
  const DeviceFeatureInfo&  getEnabledFeatureInfo() const override;
  CapabilityReport          queryCapabilities() const override;
  bool                      supports(CapabilityTier tier) const override;
  const MemoryProperties&   getPhysicalMemoryProperties() const override;

  QueueInfo getGraphicsQueue() const override;
  QueueInfo getComputeQueue() const override;
  QueueInfo getTransferQueue() const override;

  bool queryImGuiNativeContext(ImGuiNativeContext& out) const override;

  bool isInstanceExtensionSupported(const char* name) const override;
  bool isDeviceExtensionSupported(const char* name) const override;
  bool isFormatSupported(TextureFormat format, FormatFeatureFlag feature) const override;

  void waitIdle() override;

  // --- Immediate upload seam (UPL-02) ---
  void executeImmediateUpload(std::function<void(rhi::CommandBuffer&)> uploadFn) override;
  void flushUploadRetirements(bool waitForCompletion) override;

  TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) override;
  TextureViewHandle registerExternalTextureView(uint64_t externalView) override;
  void              destroyTextureView(TextureViewHandle handle) override;

  TextureHandle createTexture(const TextureDesc& desc) override;
  void          destroyTexture(TextureHandle handle) override;
  TextureHandle registerExternalTexture(uint64_t externalImage) override;
  void          destroyImage(TextureHandle handle) override;

  // --- Modern GPU interface (Wave 1: buffers / samplers / query pools) ---
  BufferHandle createBuffer(const BufferDesc& desc) override;
  void         destroyBuffer(BufferHandle handle) override;
  BufferHandle registerExternalBuffer(uint64_t externalBuffer) override;
  void         updateBufferBinding(BufferHandle handle, uint64_t externalBuffer) override;
  GpuPtr       getBufferGpuAddress(BufferHandle handle) const override;
  void*        mapBuffer(BufferHandle handle) override;
  void         unmapBuffer(BufferHandle handle) override;

  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void          destroySampler(SamplerHandle handle) override;

  QueryPoolHandle createQueryPool(uint32_t queryCount) override;
  void            destroyQueryPool(QueryPoolHandle handle) override;
  uint64_t        getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex) override;
  bool            getQueryPoolResultsWithAvailability(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, uint64_t* outPairs) override;

  ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc) override;
  void                 destroyArgumentLayout(ArgumentLayoutHandle handle) override;
  ArgumentTableHandle  createArgumentTable(ArgumentLayoutHandle layout) override;
  void                 destroyArgumentTable(ArgumentTableHandle handle) override;
  void                 updateArgumentTable(ArgumentTableHandle table, uint32_t writeCount, const ArgumentWrite* writes) override;
  PipelineHandle       createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  PipelineHandle       createComputePipeline(const ComputePipelineDesc& desc) override;
  void                 destroyPipeline(PipelineHandle handle) override;
  uint64_t             resolveArgumentLayoutNative(ArgumentLayoutHandle layout) const;
  uint64_t             resolveArgumentTableNative(ArgumentTableHandle table) const;

  // The render layer owns the resource table and injects it here so the device can
  // back its texture-view handles. Must be called before any createTextureView call.
  void setResourceTable(VulkanResourceTable* table) { m_resourceTable = table; }
  void setFrameContext(VulkanFrameContext* frameContext) { m_frameContext = frameContext; }

  // The render layer owns the VMA allocator and injects it here so the device can create
  // images. Must be called before any createImage call.
  void setAllocator(VmaAllocator allocator) { m_allocator = allocator; }

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

  uint32_t processRetirements(uint64_t completedTimelineValue);

  // rhi/vulkan 内部使用——供 RenderDevice::init() 传入 VulkanSwapchain::init（RDEV-01 方案 B）
  VkCommandPool transientCmdPool() const { return m_transientCmdPool; }

private:
  struct NativeRetirement
  {
    ResourceHandle resource{};
    uint64_t       retireTimelineValue{0};
    uint64_t       nativeObject{0};
    uint64_t       nativeAllocation{0};
    uint64_t       secondaryNativeObject{0};
    bool           owned{false};
    bool           ownsSecondary{false};
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

  uint64_t retirementTimelineValue() const;
  void     enqueueRetirement(NativeRetirement retirement);
  void     destroyRetiredResource(const NativeRetirement& retirement);
  void     drainRetirements();

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

  VulkanResourceTable* m_resourceTable{nullptr};
  VulkanFrameContext*  m_frameContext{nullptr};
  VmaAllocator         m_allocator{nullptr};
  VkDescriptorPool     m_argumentPool{VK_NULL_HANDLE};  // lazily created for argument tables
  std::vector<NativeRetirement> m_pendingRetirements;

  // Upload cmd pool — migrated from RenderDevice (UPL-02)
  VkCommandPool m_uploadCmdPool{VK_NULL_HANDLE};
  // Transient graphics cmd pool — migrated from RenderDevice (RDEV-01)
  VkCommandPool m_transientCmdPool{VK_NULL_HANDLE};

  // Per-frame pending upload cmd buffers + fences — migrated from FrameUserData (UPL-02/03)
  struct UploadPendingFrame
  {
    std::vector<VkCommandBuffer> cmds;
    std::vector<VkFence>         fences;
  };
  std::vector<UploadPendingFrame> m_uploadPendingFrames;
};

}  // namespace demo::rhi::vulkan
