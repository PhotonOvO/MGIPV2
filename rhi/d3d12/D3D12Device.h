#pragma once

#include "../RHIDevice.h"
#include "../RHIResourceLifetime.h"
#include "../RHIStageBarrier.h"
#include "D3D12Queue.h"

#include <unordered_map>
#include <vector>

namespace demo::rhi::d3d12 {


class D3D12Device final : public demo::rhi::Device
{
public:
  D3D12Device() = default;
  ~D3D12Device() override;

  void init(const DeviceCreateInfo& createInfo) override;
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
  void waitIdle() override;
  bool isFormatSupported(TextureFormat format, FormatFeatureFlag feature) const override;

  TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) override;
  void              destroyTextureView(TextureViewHandle handle) override;
  TextureHandle     createTexture(const TextureDesc& desc) override;
  void              destroyTexture(TextureHandle handle) override;
  BufferHandle createBuffer(const BufferDesc& desc) override;
  void         destroyBuffer(BufferHandle handle) override;
  GpuPtr       getBufferGpuAddress(BufferHandle handle) const override;
  void*        mapBuffer(BufferHandle handle) override;
  void         unmapBuffer(BufferHandle handle) override;
  Result<MappedBufferRange> mapBufferRange(BufferHandle handle, const BufferMapDesc& desc) override;
  RHIResult flushMappedBufferRange(BufferHandle handle, uint64_t offset, uint64_t size) override;
  RHIResult invalidateMappedBufferRange(BufferHandle handle, uint64_t offset, uint64_t size) override;

  QueryPoolHandle createQueryPool(uint32_t queryCount) override;
  void destroyQueryPool(QueryPoolHandle handle) override;
  uint64_t getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex) override;
  bool getQueryPoolResultsWithAvailability(QueryPoolHandle handle, uint32_t firstQuery, std::span<uint64_t> outValueAvailabilityPairs) override;
  float getTimestampPeriodNs() const override;

  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void          destroySampler(SamplerHandle handle) override;

  ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc) override;
  void                 destroyArgumentLayout(ArgumentLayoutHandle handle) override;
  ArgumentTableHandle  createArgumentTable(const ArgumentTableCreateDesc& desc) override;
  void                 destroyArgumentTable(ArgumentTableHandle handle) override;
  void                 updateArgumentTable(ArgumentTableHandle table, ArgumentWriteBatch writes) override;
  ArgumentLayoutHandle getArgumentTableLayout(ArgumentTableHandle table) const override;

  PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
  void           destroyPipeline(PipelineHandle handle) override;
  ShaderLibraryHandle createShaderLibrary(const ShaderLibraryDesc& desc) override;
  void                destroyShaderLibrary(ShaderLibraryHandle handle) override;
  Result<ResidencySetHandle> createResidencySet(const ResidencySetDesc& desc) override;
  RHIResult destroyResidencySet(ResidencySetHandle handle) override;
  RHIResult updateResidencySet(ResidencySetHandle handle, const ResidencyUpdateBatch& batch) override;

  [[nodiscard]] bool validateArgumentTableForSubmit(ArgumentTableHandle table) const noexcept;
  [[nodiscard]] bool isPipelineValid(PipelineHandle pipeline) const noexcept;
  [[nodiscard]] bool isBufferValid(BufferHandle buffer) const noexcept;
  [[nodiscard]] bool isTextureValid(TextureHandle texture) const noexcept;
  [[nodiscard]] bool isTextureViewValid(TextureViewHandle view) const noexcept;
  [[nodiscard]] bool isQueryPoolValid(QueryPoolHandle pool) const noexcept;
  void markArgumentTableSubmitted(ArgumentTableHandle table, SubmissionToken token);
  void markPipelineSubmitted(PipelineHandle pipeline, SubmissionToken token);
  void markBufferSubmitted(BufferHandle buffer, SubmissionToken token);
  void markTextureSubmitted(TextureHandle texture, SubmissionToken token);
  void markTextureViewSubmitted(TextureViewHandle view, SubmissionToken token);
  void markQueryPoolSubmitted(QueryPoolHandle pool, SubmissionToken token);
  void bindPipeline(void* commandList, PipelineHandle pipeline, bool compute) const;
  void bindArgumentTable(void* commandList, PipelineHandle pipeline, bool compute,
                         uint32_t slot, ArgumentTableHandle table,
                         const uint64_t* dynamicOffsets, uint32_t dynamicOffsetCount) const;
  void bindRootConstants(void* commandList, PipelineHandle pipeline, bool compute,
                         uint32_t slot, const void* data, uint32_t size) const;
  [[nodiscard]] uint32_t vertexStride(PipelineHandle pipeline, uint32_t binding) const;
  [[nodiscard]] void* drawIndirectSignature(
    bool indexed, PipelineHandle pipeline, uint32_t stride) const;
  [[nodiscard]] void* dispatchIndirectSignature() const;

  [[nodiscard]] uint64_t resolveArgumentTableResourceGpu(ArgumentTableHandle handle) const;
  [[nodiscard]] uint64_t resolveArgumentTableSamplerGpu(ArgumentTableHandle handle) const;

  [[nodiscard]] void* getD3D12Device() const { return m_d3d12Device; }
  [[nodiscard]] bool  isInitialized() const { return m_initialized; }
  [[nodiscard]] bool  isValidationEnabled() const { return m_validationEnabled; }
  [[nodiscard]] void* resolveTexture(TextureHandle handle) const;
  [[nodiscard]] uint64_t resolveTextureViewDescriptor(TextureViewHandle handle) const;
  [[nodiscard]] uint64_t resolveTextureViewAttachmentDescriptor(TextureViewHandle handle) const;
  [[nodiscard]] uint64_t resolveTextureAttachmentDescriptor(
    TextureHandle texture, const TextureSubresourceRange& range) const;
  [[nodiscard]] bool textureViewHasStencil(TextureViewHandle handle) const;
  [[nodiscard]] void* resolveBuffer(BufferHandle handle) const;
  [[nodiscard]] void* resolveBufferMappedData(BufferHandle handle) const;
  [[nodiscard]] uint64_t resolveBufferSize(BufferHandle handle) const;
  void transitionBuffer(void* commandList, BufferHandle handle, uint32_t targetState);
  void prepareBufferForQueueHandoff(void* commandList, BufferHandle handle);
  void transitionTexture(void* commandList, TextureHandle handle,
                         const TextureSubresourceRange& range,
                         uint32_t targetState);
  void resetQueryPool(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount);
  void writeTimestamp(void* commandList, QueryPoolHandle handle, uint32_t queryIndex);

private:
  friend class D3D12Swapchain;

  TextureHandle adoptSwapchainTexture(void* resource);
  TextureViewHandle adoptSwapchainTextureView(uint64_t descriptor);

  struct ShaderLibraryRecord
  {
    ShaderIRFormat format{ShaderIRFormat::unknown};
    std::vector<uint8_t> bytes;
  };

  struct BufferRecord
  {
    void*       resource{nullptr};
    void*       mapped{nullptr};
    uint64_t    size{0};
    GpuPtr      gpuAddress{};
    MemoryUsage memoryUsage{MemoryUsage::gpuOnly};
    uint32_t nativeState{0};
    SubmissionTokenSet pendingUses{};
  };

  struct QueryPoolRecord
  {
    void* queryHeap{nullptr};
    void* readbackBuffer{nullptr};
    uint32_t count{0};
    std::vector<uint8_t> available;
    SubmissionTokenSet pendingUses{};
  };

  struct TextureRecord
  {
    void*             resource{nullptr};
    TextureDesc       desc{};
    std::vector<uint32_t> nativeStates;
    bool              owned{true};
    SubmissionTokenSet pendingUses{};
  };

  struct TextureViewRecord
  {
    void*    resource{nullptr};
    uint32_t descriptorIndex{~0u};
    uint64_t nativeDescriptor{0};
    uint32_t attachmentDescriptorIndex{~0u};
    uint64_t attachmentDescriptor{0};
    TextureViewCreateDesc desc{};
    bool     ownsResourceReference{true};
    bool     ownsDescriptor{true};
    bool     ownsAttachmentDescriptor{false};
    SubmissionTokenSet pendingUses{};
  };

  struct RetiredBuffer
  {
    BufferRecord record{};
    SubmissionTokenSet retirementDependencies{};
  };

  struct RetiredTexture
  {
    TextureRecord record{};
    SubmissionTokenSet retirementDependencies{};
  };

  struct RetiredTextureView
  {
    TextureViewRecord record{};
    SubmissionTokenSet retirementDependencies{};
  };

  struct RetiredQueryPool
  {
    QueryPoolRecord record{};
    SubmissionTokenSet retirementDependencies{};
  };

  struct SamplerRecord
  {
    uint32_t descriptorIndex{0};
  };

  struct ArgumentBindingPlacement
  {
    ArgumentBinding binding{};
    uint32_t resourceOffset{~0u};
    uint32_t samplerOffset{~0u};
  };

  struct ArgumentLayoutRecord
  {
    std::vector<ArgumentBindingPlacement> bindings;
    uint32_t resourceDescriptorCount{0};
    uint32_t samplerDescriptorCount{0};
  };

  struct ArgumentResourceReference
  {
    uint32_t binding{0};
    uint32_t arrayElement{0};
    ResidencyResource resource{};
  };

  struct ArgumentTableRecord
  {
    struct BufferValue
    {
      BufferHandle buffer{};
      uint64_t offset{0};
      uint64_t size{0};
    };

    ArgumentLayoutHandle layout{};
    ArgumentTableLifetime lifetime{ArgumentTableLifetime::persistent};
    uint32_t resourceBase{~0u};
    uint32_t samplerBase{~0u};
    SubmissionTokenSet pendingUses{};
    std::unordered_map<uint32_t, BufferValue> buffers;
    std::vector<ArgumentResourceReference> referencedResources;
  };
public:
  struct PipelineTableBinding
  {
    struct DynamicBinding
    {
      uint32_t binding{0};
      uint32_t rootParameter{~0u};
      uint32_t kind{0};
    };

    std::vector<uint32_t> resourceParameters;
    std::vector<uint32_t> samplerParameters;
    std::vector<DynamicBinding> dynamicBindings;
  };

  struct PipelineRecord
  {
    struct RootParameterBinding
    {
      uint32_t rootParameter{~0u};
      uint32_t destinationOffset{0};
      uint32_t size{0};
    };

    void* pipelineState{nullptr};
    void* rootSignature{nullptr};
    void* indexedIndirectSignature{nullptr};
    bool compute{false};
    uint32_t primitiveTopology{0};
    std::vector<uint32_t> vertexStrides;
    std::unordered_map<uint32_t, PipelineTableBinding> tables;
    std::unordered_map<uint32_t, std::vector<RootParameterBinding>> rootParameters;
    SubmissionTokenSet pendingUses{};
  };

private:
  struct RetiredPipeline
  {
    PipelineRecord record{};
    SubmissionTokenSet retirementDependencies{};
  };

  [[nodiscard]] const ArgumentBindingPlacement* findArgumentBinding(
    const ArgumentLayoutRecord& layout, uint32_t binding) const;
  struct NativeQueueInfo
  {
    void*      commandQueue{nullptr};
    uint32_t   nodeMask{0};
    uint32_t   queueIndex{0};
    QueueClass queueClass{QueueClass::graphics};
    bool       dedicated{false};

    QueueInfo toRhi() const;
  };

  void selectD3D12Adapter();
  void initD3D12Device();
  void initD3D12Queues();
  void initDescriptorHeaps();
  void detectD3D12Capabilities();
  void validateD3D12Capabilities() const;
  void releaseNativeObjects() noexcept;
  [[nodiscard]] SubmissionTokenSet retirementDependencies() const;
  [[nodiscard]] bool isRetirementComplete(const SubmissionTokenSet& dependencies) const;
  void releaseRetiredBuffer(BufferRecord& record) noexcept;
  void releaseRetiredTexture(TextureRecord& record) noexcept;
  void releaseRetiredTextureView(TextureViewRecord& record) noexcept;
  void releaseRetiredQueryPool(QueryPoolRecord& record) noexcept;
  void releaseRetiredPipeline(PipelineRecord& record) noexcept;
  [[nodiscard]] bool isResidencyResourceAlive(ResidencyResource resource) const noexcept;
  void drainRetirements() noexcept;

  DeviceCreateInfo m_createInfo{};

  void* m_dxgiFactory{nullptr};
  void* m_dxgiAdapter{nullptr};
  void* m_d3d12Device{nullptr};
  void* m_infoQueue{nullptr};
  uint32_t m_infoQueueCallbackCookie{0};

  NativeQueueInfo m_graphicsQueue{};
  NativeQueueInfo m_computeQueue{};
  NativeQueueInfo m_transferQueue{};
  std::unique_ptr<D3D12Queue> m_graphicsQueueApi;
  std::unique_ptr<D3D12Queue> m_computeQueueApi;
  std::unique_ptr<D3D12Queue> m_transferQueueApi;

  uint32_t           m_apiVersion{0};
  PhysicalDeviceInfo m_physicalDeviceInfo{};
  DeviceFeatureInfo  m_featureInfo{};
  CapabilityReport   m_capabilities{};
  MemoryProperties   m_memoryProperties{};

  uint32_t m_d3dFeatureLevel{0};
  uint32_t m_shaderModel{0};
  uint32_t m_resourceBindingTier{0};
  bool     m_supportsMeshShaders{false};
  bool     m_supportsRayTracing{false};
  bool     m_supportsEnhancedBarriers{false};

  void*    m_cbvSrvUavHeap{nullptr};
  void*    m_samplerHeap{nullptr};
  void*    m_resourceStagingHeap{nullptr};
  void*    m_samplerStagingHeap{nullptr};
  void*    m_rtvHeap{nullptr};
  void*    m_dsvHeap{nullptr};
  void*    m_drawSignature{nullptr};
  void*    m_drawIndexedSignature{nullptr};
  void*    m_dispatchSignature{nullptr};
  uint32_t m_descriptorSize{0};

  uint32_t m_samplerDescriptorSize{0};
  uint32_t m_rtvDescriptorSize{0};
  uint32_t m_dsvDescriptorSize{0};
  std::unordered_map<uint32_t, BufferRecord>      m_buffers;
  std::vector<RetiredBuffer>                      m_retiredBuffers;
  std::vector<RetiredTexture>                     m_retiredTextures;
  std::vector<RetiredTextureView>                 m_retiredTextureViews;
  std::vector<RetiredQueryPool>                   m_retiredQueryPools;
  std::vector<RetiredPipeline>                    m_retiredPipelines;
  std::unordered_map<uint32_t, TextureRecord>     m_textures;
  std::unordered_map<uint32_t, QueryPoolRecord>   m_queryPools;
  std::unordered_map<uint32_t, TextureViewRecord> m_textureViews;
  std::vector<uint32_t>                           m_freeTextureViewDescriptors;
  std::vector<uint32_t>                           m_freeRtvDescriptors;
  std::vector<uint32_t>                           m_freeDsvDescriptors;
  std::unordered_map<uint32_t, SamplerRecord>        m_samplers;
  std::unordered_map<uint32_t, ArgumentLayoutRecord> m_argumentLayouts;
  std::unordered_map<uint32_t, ArgumentTableRecord>  m_argumentTables;
  uint32_t                                        m_nextBufferIndex{1};
  uint32_t                                        m_nextQueryPoolIndex{1};
  std::unordered_map<uint32_t, PipelineRecord>       m_pipelines;
  std::unordered_map<uint32_t, ShaderLibraryRecord>  m_shaderLibraries;
  uint32_t                                        m_nextTextureIndex{1};
  uint32_t                                        m_nextTextureViewIndex{1};
  uint32_t                                        m_nextTextureViewDescriptor{0};
  uint32_t                                        m_nextRtvDescriptor{0};
  uint32_t                                        m_nextDsvDescriptor{0};

  uint32_t m_handleGeneration{0};
  uint32_t m_nextSamplerIndex{1};
  uint32_t m_nextArgumentLayoutIndex{1};
  uint32_t m_nextArgumentTableIndex{1};
  uint32_t m_nextSamplerDescriptor{0};
  uint32_t m_nextPipelineIndex{1};
  uint32_t m_nextShaderLibraryIndex{1};
  bool m_initialized{false};
  bool m_validationEnabled{false};
};

}  // namespace demo::rhi::d3d12
