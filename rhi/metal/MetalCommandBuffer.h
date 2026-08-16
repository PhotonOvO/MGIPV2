#pragma once

#include "../RHICommandBuffer.h"
#include "../RHIDebugValidation.h"
#include "MetalResourceTable.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace NS {
class Object;
}

namespace MTL {
class Allocation;
class Buffer;
class IndirectCommandBuffer;
class ResidencySet;
}

namespace MTL4 {
class ArgumentTable;
class CommandBuffer;
class CommandEncoder;
class ComputeCommandEncoder;
class RenderCommandEncoder;
}

namespace demo::rhi::metal {

class MetalCommandAllocator;
class MetalQueue;
class MetalResourceTable;

struct MetalBoundTable
{
  uint32_t slot{0};
  ArgumentTableHandle table{};
  ShaderStage stages{ShaderStage::none};
};

struct MetalDynamicBufferValue
{
  uint32_t tableSlot{0};
  BufferHandle buffer{};
  uint64_t offset{0};
  uint64_t size{0};
};

struct MetalRootValue
{
  uint32_t slot{0};
  RootBindingKind kind{RootBindingKind::constants};
  ShaderStage stages{ShaderStage::none};
  std::vector<std::byte> constants;
  GpuPtr pointer{};
};

struct MetalVertexBufferValue
{
  uint32_t binding{0};
  BufferHandle buffer{};
  uint64_t offset{0};
};

struct MetalEncoderBindingState
{
  PipelineHandle pipeline{};
  std::vector<MetalBoundTable> tables;
  std::vector<MetalDynamicBufferValue> pendingDynamicBuffers;
  std::vector<MetalDynamicBufferValue> dynamicBuffers;
  std::vector<MetalRootValue> roots;
  std::vector<MetalVertexBufferValue> vertexBuffers;
  MTL4::ArgumentTable* snapshot{nullptr};
  bool dirty{true};

  void reset();
};

struct MetalArgumentTableSnapshotSource
{
  ArgumentTableHandle table{};
  uint64_t version{0};
};

struct MetalArgumentTableSnapshot
{
  std::vector<MetalArgumentTableSnapshotSource> sources;
};

struct MetalPreparedIndexedIndirect
{
  DrawIndirectCountDesc draw{};
  BufferHandle indexBuffer{};
  uint64_t indexBufferOffset{0};
  IndexFormat indexFormat{IndexFormat::uint32};
  PrimitiveTopology topology{PrimitiveTopology::triangleList};
  MTL::IndirectCommandBuffer* commandBuffer{nullptr};
  MTL::Buffer* executionRangeBuffer{nullptr};
  uint64_t executionRangeOffset{0};
  MTL::Buffer* clampedArgumentsBuffer{nullptr};
  uint64_t clampedArgumentsOffset{0};
  uint32_t argumentStride{0};
  bool consumed{false};
};

class MetalCommandBuffer;

class MetalRenderEncoder final : public RenderEncoder
{
public:
  void prepare(MTL4::RenderCommandEncoder* encoder, MetalCommandBuffer& owner,
               const Rect2D& defaultScissor);
  void invalidate() noexcept;

  void setPipeline(PipelineHandle pipeline) override;
  void setArgumentTable(ShaderStage stages, uint32_t slot, ArgumentTableHandle table) override;
  void setDynamicBuffer(ShaderStage stages, uint32_t slot, BufferHandle buffer,
                        uint64_t offset, uint64_t size) override;
  void setRootConstants(ShaderStage stages, uint32_t slot,
                        std::span<const std::byte> data) override;
  void setRootPointer(ShaderStage stages, uint32_t slot, GpuPtr ptr) override;
  void setViewport(const Viewport& viewport) override;
  void setScissor(const Rect2D& scissor) override;
  void bindVertexBuffers(uint32_t firstBinding,
                         std::span<const VertexBufferBinding> bindings) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) override;
  void readInputAttachment(uint32_t index) override;
  void draw(const DrawDesc& desc) override;
  void drawIndexed(const DrawIndexedDesc& desc) override;
  void drawIndexedIndirect(const DrawIndirectDesc& desc) override;
  void drawIndexedIndirectCount(const DrawIndirectCountDesc& desc) override;
  void drawIndirect(const DrawIndirectDesc& desc) override;
  void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY,
                     uint32_t groupCountZ) override;
  void drawMeshTasksIndirect(const DrawIndirectDesc& desc) override;

private:
  void requireActive() const;
  const MetalPipelineRecord& requirePipeline() const;
  void bindSnapshot();

  MTL4::RenderCommandEncoder* m_encoder{nullptr};
  MetalCommandBuffer* m_owner{nullptr};
  MetalEncoderBindingState m_bindings;
  BufferHandle m_indexBuffer{};
  uint64_t m_indexOffset{0};
  IndexFormat m_indexFormat{IndexFormat::uint32};
  RasterState m_rasterState{};
  DepthState m_depthState{};
  Rect2D m_scissor{};
  bool m_pipelineStateCacheValid{false};
  bool m_valid{false};
};

class MetalComputeEncoder final : public ComputeEncoder
{
public:
  void prepare(MTL4::ComputeCommandEncoder* encoder, MetalCommandBuffer& owner);
  void invalidate() noexcept;

  void setPipeline(PipelineHandle pipeline) override;
  void setArgumentTable(uint32_t slot, ArgumentTableHandle table) override;
  void setRootConstants(uint32_t slot, std::span<const std::byte> data) override;
  void setRootPointer(uint32_t slot, GpuPtr ptr) override;
  void dispatch(const DispatchDesc& desc) override;
  void dispatchIndirect(const DispatchIndirectDesc& desc) override;
  void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst,
                  uint64_t dstOffset, uint64_t size) override;
  void copyBufferToTexture(const BufferTextureCopyDesc& desc) override;
  void copyTextureToBuffer(const BufferTextureCopyDesc& desc) override;
  void blitTexture(const TextureBlitDesc& desc) override;
  void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size,
                  uint32_t data) override;

private:
  void requireActive() const;
  void requireCompute(const char* command) const;
  void requireTransfer(const char* command) const;
  const MetalPipelineRecord& requirePipeline() const;
  void bindSnapshot();

  MTL4::ComputeCommandEncoder* m_encoder{nullptr};
  MetalCommandBuffer* m_owner{nullptr};
  MetalEncoderBindingState m_bindings;
  bool m_valid{false};
};

class MetalCommandBuffer final : public CommandBuffer
{
public:
  MetalCommandBuffer(MetalCommandAllocator& allocator, MTL4::CommandBuffer* commandBuffer,
                     MetalResourceTable* resourceTable);
  ~MetalCommandBuffer() override;

  void begin(CommandAllocator& allocator) override;
  void end() override;
  [[nodiscard]] CommandBufferState state() const noexcept override;

  RenderEncoder* beginRenderPass(const RenderPassDesc& desc) override;
  ComputeEncoder* beginComputePass() override;
  void endEncoding() override;
  void prepareIndexedIndirect(
    const PrepareIndexedIndirectDesc& desc) override;
  void prepareIndexedIndirectCount(
    const PrepareIndexedIndirectCountDesc& desc) override;
  RHIResult useResidencySet(ResidencySetHandle set) override;

  void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) override;
  void resourceBarrier(std::span<const TextureBarrier> textures,
                       std::span<const BufferBarrier> buffers,
                       std::span<const AliasingBarrier> aliasing = {}) override;
  void clearColorTexture(TextureHandle texture, const TextureSubresourceRange& range,
                         const ClearColorValue& clearColor) override;

  void beginEvent(const char* name) override;
  void endEvent() override;
  void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery,
                      uint32_t queryCount) override;
  void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex,
                      bool afterAllCommands) override;

  [[nodiscard]] const MTL4::CommandBuffer* nativeHandle() const noexcept
  {
    return m_commandBuffer;
  }
  [[nodiscard]] QueueClass queueClass() const noexcept;
  void validateForSubmit(const MetalQueue& queue);
  void markSubmitted(SubmissionToken token);
  void markReusable();
  void requireQueueOperation(QueueOperation operation, const char* command) const;

  [[nodiscard]] MetalResourceTable& resources() const;
  [[nodiscard]] MetalCommandAllocator& allocator() const;
  [[nodiscard]] MTL4::ArgumentTable* materializeArgumentTable(
    MetalEncoderBindingState& state, MetalPipelineKind expectedKind);

  void trackArgumentTable(ArgumentTableHandle table);
  void trackPipeline(PipelineHandle pipeline);
  void trackBuffer(BufferHandle buffer);
  void trackTexture(TextureHandle texture);
  void trackTextureView(TextureViewHandle view);
  void trackSampler(SamplerHandle sampler);
  void trackQueryPool(QueryPoolHandle pool);
  void trackResidencySet(ResidencySetHandle set);
  void trackNativeAllocation(const MTL::Allocation* allocation);
  void retainNative(NS::Object* object);

private:
  friend class MetalRenderEncoder;
  friend class MetalComputeEncoder;

  enum class EncoderKind : uint8_t
  {
    none,
    render,
    compute,
  };

  void requireRecording(const char* operation) const;
  [[nodiscard]] MTL4::CommandEncoder* activeEncoder() const noexcept;
  void applyPendingBarrier(MTL4::CommandEncoder& encoder);
  void encodeBarrier(StageFlags producer, StageFlags consumer,
                     HazardFlags hazards);
  using ReplayCommand = std::function<void(MetalCommandBuffer&)>;
  void recordCommand(ReplayCommand command);
  [[nodiscard]] bool argumentTableSnapshotsChanged() const;
  void reencodeForSubmit();
  void rebuildAutomaticResidencySet();
  void releaseRetainedResources() noexcept;
  void clearTracking();
  void prepareIndexedIndirectImpl(
    const PrepareIndexedIndirectCountDesc& desc);
  [[nodiscard]] MetalPreparedIndexedIndirect* consumePreparedIndexedIndirect(
    const DrawIndirectDesc& draw, BufferHandle indexBuffer,
    uint64_t indexBufferOffset, IndexFormat indexFormat,
    PrimitiveTopology topology);
  [[nodiscard]] MetalPreparedIndexedIndirect& consumePreparedIndexedIndirectCount(
    const DrawIndirectCountDesc& draw, BufferHandle indexBuffer,
    uint64_t indexBufferOffset, IndexFormat indexFormat,
    PrimitiveTopology topology);

  MTL4::CommandBuffer* m_commandBuffer{nullptr};
  MetalResourceTable* m_resourceTable{nullptr};
  MetalCommandAllocator* m_allocator{nullptr};
  MetalRenderEncoder m_renderEncoder;
  MetalComputeEncoder m_computeEncoder;
  MTL4::RenderCommandEncoder* m_nativeRenderEncoder{nullptr};
  MTL4::ComputeCommandEncoder* m_nativeComputeEncoder{nullptr};
  MTL::ResidencySet* m_automaticResidencySet{nullptr};
  EncoderKind m_active{EncoderKind::none};
  CommandBufferState m_state{CommandBufferState::idle};
  SubmissionToken m_submission{};

  std::vector<ArgumentTableHandle> m_argumentTables;
  std::vector<PipelineHandle> m_pipelines;
  std::vector<BufferHandle> m_buffers;
  std::vector<TextureHandle> m_textures;
  std::vector<TextureViewHandle> m_textureViews;
  std::vector<SamplerHandle> m_samplers;
  std::vector<QueryPoolHandle> m_queryPools;
  std::vector<ResidencySetHandle> m_residencySets;
  std::vector<const MTL::Allocation*> m_nativeAllocations;
  std::vector<NS::Object*> m_retainedResources;
  std::vector<const MTL::Allocation*> m_automaticResidencyAllocations;
  std::vector<MetalArgumentTableSnapshot> m_argumentTableSnapshots;
  std::vector<MetalPreparedIndexedIndirect> m_preparedIndexedIndirect;
  std::vector<ReplayCommand> m_replayCommands;
  bool m_replaying{false};

  StageFlags m_pendingProducer{StageFlags::none};
  StageFlags m_pendingConsumer{StageFlags::none};
  HazardFlags m_pendingHazards{HazardFlags::none};
  bool m_pendingResourceAlias{false};
  uint32_t m_debugGroupDepth{0};
  DebugResourceStateTracker m_resourceStates;
};

}  // namespace demo::rhi::metal
