#pragma once

#include "../RHICommandBuffer.h"
#include "../RHIDebugValidation.h"
#include "../RHIEncoder.h"
#include "../RHIQueue.h"
#include <array>

#include <cstdint>
#include <vector>

namespace demo::rhi::d3d12 {

class D3D12Device;
class D3D12CommandBuffer;
class D3D12CommandAllocator;

class D3D12RenderEncoder final : public RenderEncoder
{
public:
  void prepare(void* nativeCommandList, D3D12Device* device, D3D12CommandBuffer* owner);
  void invalidate() noexcept;

  void setPipeline(PipelineHandle pipeline) override;
  void setArgumentTable(ShaderStage stages, uint32_t slot, ArgumentTableHandle table) override;
  void setDynamicBuffer(ShaderStage stages, uint32_t slot, BufferHandle buffer,
                        uint64_t offset, uint64_t size) override;
  void setRootConstants(ShaderStage stages, uint32_t slot, std::span<const std::byte> data) override;
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
  void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
  void drawMeshTasksIndirect(const DrawIndirectDesc& desc) override;

private:
  void requireActive() const;
  void* m_commandList{nullptr};
  PipelineHandle m_pipeline{};
  std::array<std::array<uint64_t, 8>, 16> m_dynamicOffsets{};
  std::array<uint32_t, 16> m_dynamicOffsetCounts{};
  D3D12Device* m_device{nullptr};
  D3D12CommandBuffer* m_owner{nullptr};
  bool m_valid{false};
};

class D3D12ComputeEncoder final : public ComputeEncoder
{
public:
  void prepare(void* nativeCommandList, D3D12Device* device, D3D12CommandBuffer* owner);
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
  void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data) override;

private:
  void requireActive() const;
  void requireCompute(const char* command) const;
  void requireTransfer(const char* command) const;
  void* m_commandList{nullptr};
  D3D12Device* m_device{nullptr};
  D3D12CommandBuffer* m_owner{nullptr};
  PipelineHandle m_pipeline{};
  bool m_valid{false};
};

class D3D12CommandBuffer final : public CommandBuffer
{
public:
  D3D12CommandBuffer() = default;
  D3D12CommandBuffer(D3D12CommandAllocator& allocator,
                     void* nativeCommandList, D3D12Device* device);
  ~D3D12CommandBuffer() override;

  void setTarget(void* nativeCommandList, D3D12Device* device);
  void retainTransientResource(void* resource);
  void* prepareFillUpload(uint64_t size, uint32_t data, uint64_t& sourceOffset);

  void begin(CommandAllocator& allocator) override;
  void end() override;
  [[nodiscard]] CommandBufferState state() const noexcept override;

  RenderEncoder* beginRenderPass(const RenderPassDesc& desc) override;
  ComputeEncoder* beginComputePass() override;
  void endEncoding() override;
  RHIResult useResidencySet(ResidencySetHandle set) override;
  void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) override;
  void resourceBarrier(std::span<const TextureBarrier> textures,
                       std::span<const BufferBarrier> buffers,
                       std::span<const AliasingBarrier> aliasing = {}) override;
  void clearColorTexture(TextureHandle texture, const TextureSubresourceRange& range,
                         const ClearColorValue& clearColor) override;
  void beginEvent(const char* name) override;
  void endEvent() override;
  void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) override;
  void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands) override;

  [[nodiscard]] void* nativeHandle() const { return m_commandList; }
  [[nodiscard]] QueueClass queueClass() const noexcept;
  void validateForSubmit() const;
  void markSubmitted(SubmissionToken token);
  void requireQueueOperation(QueueOperation operation, const char* command) const;
  void markReusable();
  void trackArgumentTable(ArgumentTableHandle table);
  void trackPipeline(PipelineHandle pipeline);
  void trackBuffer(BufferHandle buffer);
  void trackTexture(TextureHandle texture);
  void trackTextureView(TextureViewHandle view);
  void trackQueryPool(QueryPoolHandle pool);

private:
  enum class EncoderKind : uint8_t { none, render, compute };

  void releaseTransientResources() noexcept;
  void releaseFillUpload() noexcept;
  void ensureFillUploadCapacity(uint64_t requiredCapacity);
  void requireRecording(const char* operation) const;

  void* m_commandList{nullptr};
  D3D12Device* m_device{nullptr};
  D3D12CommandAllocator* m_allocator{nullptr};
  D3D12RenderEncoder m_renderEncoder;
  D3D12ComputeEncoder m_computeEncoder;
  EncoderKind m_active{EncoderKind::none};
  CommandBufferState m_state{CommandBufferState::idle};
  SubmissionToken m_submission{};
  std::array<ArgumentTableHandle, 64> m_argumentTables{};
  uint32_t m_argumentTableCount{0};
  std::array<PipelineHandle, 64> m_pipelines{};
  uint32_t m_pipelineCount{0};
  std::array<BufferHandle, 512> m_buffers{};
  uint32_t m_bufferCount{0};
  std::array<TextureHandle, 256> m_textures{};
  uint32_t m_textureCount{0};
  std::array<TextureViewHandle, 128> m_textureViews{};
  uint32_t m_textureViewCount{0};
  std::array<QueryPoolHandle, 16> m_queryPools{};
  uint32_t m_queryPoolCount{0};
  DebugResourceStateTracker m_resourceStates;
  std::vector<void*> m_transientResources;
  void* m_fillUpload{nullptr};
  void* m_fillUploadMapped{nullptr};
  uint64_t m_fillUploadCapacity{0};
  uint64_t m_fillUploadHead{0};
};

}  // namespace demo::rhi::d3d12
