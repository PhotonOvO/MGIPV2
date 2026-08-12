#pragma once

#include "../RHIDevice.h"
#include "MetalQueue.h"
#include "MetalResourceTable.h"

#include <array>
#include <memory>
#include <vector>

namespace NS {
class Object;
}

namespace MTL {
class Allocation;
class ComputePipelineState;
class Device;
class Texture;
class TextureViewPool;
}

namespace MTL4 {
class Compiler;
}

namespace demo::rhi::metal {

class MetalSurface;

class MetalDevice final : public Device
{
public:
  MetalDevice();
  ~MetalDevice() override;

  void init(const DeviceCreateInfo& createInfo) override;
  void deinit() override;

  BackendInfo getBackendInfo() const override;
  const char* getDeviceName() const override;
  const PhysicalDeviceInfo& getPhysicalDeviceInfo() const override;
  const DeviceFeatureInfo& getEnabledFeatureInfo() const override;
  CapabilityReport queryCapabilities() const override;
  bool supports(CapabilityTier tier) const override;
  const MemoryProperties& getPhysicalMemoryProperties() const override;

  Queue* getQueue(QueueClass queueClass) override;
  std::unique_ptr<CommandAllocator> createCommandAllocator(QueueClass queueClass) override;
  void collectGarbage() override;

  void initSurface(Surface& surface, const WindowHandle& window) override;
  std::unique_ptr<Swapchain> createSwapchain(Surface& surface, bool vSync) override;
  float getTimestampPeriodNs() const override;
  bool isFormatSupported(TextureFormat format, FormatFeatureFlag feature) const override;
  void waitIdle() override;

  TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) override;
  void destroyTextureView(TextureViewHandle handle) override;
  TextureHandle createTexture(const TextureDesc& desc) override;
  void destroyTexture(TextureHandle handle) override;

  BufferHandle createBuffer(const BufferDesc& desc) override;
  void destroyBuffer(BufferHandle handle) override;
  GpuPtr getBufferGpuAddress(BufferHandle handle) const override;
  void* mapBuffer(BufferHandle handle) override;
  void unmapBuffer(BufferHandle handle) override;
  Result<MappedBufferRange> mapBufferRange(BufferHandle handle,
                                            const BufferMapDesc& desc) override;
  RHIResult flushMappedBufferRange(BufferHandle handle, uint64_t offset,
                                   uint64_t size) override;
  RHIResult invalidateMappedBufferRange(BufferHandle handle, uint64_t offset,
                                        uint64_t size) override;

  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void destroySampler(SamplerHandle handle) override;

  ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc) override;
  void destroyArgumentLayout(ArgumentLayoutHandle handle) override;
  ArgumentTableHandle createArgumentTable(const ArgumentTableCreateDesc& desc) override;
  void destroyArgumentTable(ArgumentTableHandle handle) override;
  void updateArgumentTable(ArgumentTableHandle table, ArgumentWriteBatch writes) override;
  ArgumentLayoutHandle getArgumentTableLayout(ArgumentTableHandle table) const override;

  PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
  void destroyPipeline(PipelineHandle handle) override;

  QueryPoolHandle createQueryPool(uint32_t queryCount) override;
  void destroyQueryPool(QueryPoolHandle handle) override;
  uint64_t getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex) override;
  bool getQueryPoolResultsWithAvailability(
    QueryPoolHandle handle, uint32_t firstQuery,
    std::span<uint64_t> outValueAvailabilityPairs) override;

  ShaderLibraryHandle createShaderLibrary(const ShaderLibraryDesc& desc) override;
  void destroyShaderLibrary(ShaderLibraryHandle handle) override;

  Result<ResidencySetHandle> createResidencySet(const ResidencySetDesc& desc) override;
  RHIResult destroyResidencySet(ResidencySetHandle handle) override;
  RHIResult updateResidencySet(ResidencySetHandle handle,
                               const ResidencyUpdateBatch& batch) override;

  [[nodiscard]] MTL::Device* nativeDevice() const noexcept { return m_device; }
  [[nodiscard]] MTL4::Compiler* compiler() const noexcept { return m_compiler; }
  [[nodiscard]] MetalResourceTable& resourceTable() noexcept { return *m_resources; }
  [[nodiscard]] const MetalResourceTable& resourceTable() const noexcept { return *m_resources; }

  TextureHandle registerSwapchainTexture(MTL::Texture* texture,
                                         const TextureDesc& desc);
  void unregisterSwapchainTexture(TextureHandle handle);

private:
  enum class RetirementKind : uint8_t
  {
    generic,
    pipeline,
  };

  struct NativeRetirement
  {
    NS::Object* primary{nullptr};
    NS::Object* secondary{nullptr};
    SubmissionTokenSet dependencies{};
    RetirementKind kind{RetirementKind::generic};
    uint32_t textureViewPoolIndex{kInvalidMetalBinding};
  };

  [[nodiscard]] std::array<SubmissionToken, 3> completedTokens() const;
  [[nodiscard]] bool dependenciesComplete(const SubmissionTokenSet& dependencies) const;
  void enqueueRetirement(NS::Object* primary, SubmissionTokenSet dependencies,
                         NS::Object* secondary = nullptr,
                         RetirementKind kind = RetirementKind::generic,
                         uint32_t textureViewPoolIndex = kInvalidMetalBinding);
  void releaseActiveResources();
  [[nodiscard]] MTL::Allocation* resolveAllocation(ResidencyResource resource) const;
  [[nodiscard]] bool argumentTableCanUpdate(const MetalArgumentTableRecord& table) const;
  void validateInitialized(const char* operation) const;

  DeviceCreateInfo m_createInfo{};
  MTL::Device* m_device{nullptr};
  MTL4::Compiler* m_compiler{nullptr};
  MTL::ComputePipelineState* m_blitPipeline{nullptr};
  MTL::TextureViewPool* m_textureViewPool{nullptr};
  uint32_t m_textureViewCapacity{65536};
  uint32_t m_nextTextureViewIndex{0};
  std::vector<uint32_t> m_freeTextureViewIndices;

  std::unique_ptr<MetalResourceTable> m_resources;
  std::array<std::unique_ptr<MetalQueue>, 3> m_queues;
  uint32_t m_queueGeneration{1};
  std::vector<NativeRetirement> m_retirements;

  PhysicalDeviceInfo m_physicalDeviceInfo{};
  DeviceFeatureInfo m_featureInfo{};
  CapabilityReport m_capabilities{};
  MemoryProperties m_memoryProperties{};
  bool m_initialized{false};
};

}  // namespace demo::rhi::metal
