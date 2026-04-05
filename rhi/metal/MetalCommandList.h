#pragma once

#include "../../common/Common.h"
#include "../RHICommandList.h"

#include <vector>

namespace demo::rhi::metal {

class MetalCommandList final : public demo::rhi::CommandList
{
public:
  MetalCommandList() = default;

  void setCommandBuffer(void* commandBuffer) { m_commandBuffer = commandBuffer; }

  [[nodiscard]] void* nativeHandle() const { return m_commandBuffer; }  // id<MTLCommandBuffer>

  void begin() override;
  void end() override;

  void beginRenderPass(const RenderPassDesc& desc) override;
  void endRenderPass() override;

  void setViewport(const Viewport& viewport) override;
  void setScissor(const Rect2D& scissor) override;

  void setResourceState(ResourceHandle resource, ResourceState state) override;
  void insertBarrier(BarrierType barrierType) override;
  void transitionBuffer(const BufferBarrierDesc& desc) override;
  void transitionTexture(const TextureBarrierDesc& desc) override;

  void bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) override;
  void bindBindTable(PipelineBindPoint bindPoint, uint32_t slot, BindTableHandle bindTable, const uint32_t* dynamicOffsets, uint32_t dynamicOffsetCount) override;
  void bindBindGroup(uint32_t slot, BindGroupHandle bindGroup,
                     const uint32_t* dynamicOffsets,
                     uint32_t dynamicOffsetCount) override;
  void bindVertexBuffers(uint32_t firstBinding, const uint64_t* bufferHandles,
                         const uint64_t* offsets, uint32_t bufferCount) override;
  void bindIndexBuffer(uint64_t bufferHandle, uint64_t offset, IndexFormat format) override;
  void pushConstants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) override;

  void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
  void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                   uint32_t firstIndex, int32_t vertexOffset,
                   uint32_t firstInstance) override;
  void dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

  void beginEvent(const char* name) override;
  void endEvent() override;

private:
  struct ResourceStateEntry
  {
    ResourceHandle handle{};
    ResourceState  state{ResourceState::Undefined};
  };

  [[nodiscard]] ResourceState getTrackedState(ResourceHandle resource, ResourceState fallback) const;

  // NOTES: Metal Command Encoding Architecture
  // Metal uses command buffers with encoders, not a single command buffer like Vulkan
  //
  // Command Hierarchy:
  // 1. MTLCommandQueue (queue for command buffers)
  // 2. MTLCommandBuffer (container for encoded commands)
  // 3. MTLRenderCommandEncoder (for rendering commands)
  // 4. MTLComputeCommandEncoder (for compute commands)
  // 5. MTLBlitCommandEncoder (for copy/resolve operations)
  //
  // Encoding Flow:
  // - begin(): Prepare command buffer for encoding
  // - Commands are recorded to encoders (created from command buffer)
  // - end(): Finalize encoding, command buffer ready for submission
  //
  // Metal doesn't have explicit begin/end for command lists like Vulkan
  // Instead, encoders are created and ended for each pass/type of work
  // This class abstracts encoder management behind the RHI interface

  void* m_commandBuffer{nullptr};   // id<MTLCommandBuffer>
  void* m_renderEncoder{nullptr};   // id<MTLRenderCommandEncoder>
  void* m_computeEncoder{nullptr};  // id<MTLComputeCommandEncoder>

  std::vector<ResourceStateEntry> m_resourceStates;
};

}  // namespace demo::rhi::metal