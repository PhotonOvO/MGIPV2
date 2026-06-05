#pragma once

#include "../RHICommandBuffer.h"
#include "../RHIEncoder.h"

struct VkCommandBuffer_T;
struct VkPipelineLayout_T;
using VkCommandBuffer  = VkCommandBuffer_T*;
using VkPipelineLayout = VkPipelineLayout_T*;

namespace demo::rhi::vulkan {

class VulkanResourceTable;

inline constexpr uint32_t kMaxArgumentSlots   = 8;
inline constexpr uint32_t kMaxDynOffsetPerSlot = 4;

// Records into a VkCommandBuffer owned by FrameContext. setRoot*/setRootConstants
// interpret `slot` as the push-constant byte offset (Vulkan has no slot concept
// for push constants); this is finalized per-shader when passes migrate.
class VulkanRenderEncoder final : public RenderEncoder
{
public:
  void prepare(VkCommandBuffer cmd, VulkanResourceTable* table);

  void setPipeline(PipelineHandle pipeline) override;
  void setArgumentTable(ShaderStage stages, uint32_t slot, ArgumentTableHandle table) override;
  void setDynamicBuffer(ShaderStage stages, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void setRootConstants(ShaderStage stages, uint32_t slot, const void* data, uint32_t size) override;
  void setRootPointer(ShaderStage stages, uint32_t slot, GpuPtr ptr) override;
  void setViewport(const Viewport& viewport) override;
  void setScissor(const Rect2D& scissor) override;
  void bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers, const uint64_t* offsets, uint32_t count) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) override;
  void readInputAttachment(uint32_t index) override;
  void draw(const DrawDesc& desc) override;
  void drawIndexed(const DrawIndexedDesc& desc) override;
  void drawIndexedIndirect(const DrawIndirectDesc& desc) override;
  void drawIndexedIndirect(GpuPtr args, uint32_t count, uint32_t stride) override;
  void drawIndexedIndirectCount(const DrawIndirectCountDesc& desc) override;
  void drawIndirect(const DrawIndirectDesc& desc) override;
  void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
  void drawMeshTasksIndirect(const DrawIndirectDesc& desc) override;

private:
  VkCommandBuffer      m_cmd{nullptr};
  VulkanResourceTable* m_table{nullptr};
  VkPipelineLayout     m_layout{nullptr};
  uint32_t             m_pendingDynOffsets[kMaxArgumentSlots][kMaxDynOffsetPerSlot]{};
  uint32_t             m_pendingDynCount[kMaxArgumentSlots]{};
};

class VulkanComputeEncoder final : public ComputeEncoder
{
public:
  void prepare(VkCommandBuffer cmd, VulkanResourceTable* table);

  void setPipeline(PipelineHandle pipeline) override;
  void setArgumentTable(uint32_t slot, ArgumentTableHandle table) override;
  void setRootConstants(uint32_t slot, const void* data, uint32_t size) override;
  void setRootPointer(uint32_t slot, GpuPtr ptr) override;
  void dispatch(const DispatchDesc& desc) override;
  void dispatchIndirect(const DispatchIndirectDesc& desc) override;
  void dispatchIndirect(GpuPtr args) override;

  // copy / blit (command subset)
  void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset, uint64_t size) override;
  void copyBufferToTexture(const BufferTextureCopyDesc& desc) override;
  void copyTextureToBuffer(const BufferTextureCopyDesc& desc) override;
  void blitTexture(const TextureBlitDesc& desc) override;
  void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data) override;

private:
  VkCommandBuffer      m_cmd{nullptr};
  VulkanResourceTable* m_table{nullptr};
  VkPipelineLayout     m_layout{nullptr};
};

class VulkanCommandBuffer final : public CommandBuffer
{
public:
  // Bound to the current frame's native command buffer + resource table each frame.
  void setTarget(VkCommandBuffer cmd, VulkanResourceTable* table);

  RenderEncoder*  beginRenderPass(const RenderPassDesc& desc) override;
  ComputeEncoder* beginComputePass() override;
  void            endEncoding() override;

  void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) override;
  void resourceBarrier(const TextureBarrier* textures, uint32_t textureCount,
                       const BufferBarrier* buffers, uint32_t bufferCount) override;

  void beginEvent(const char* name) override;
  void endEvent() override;

  void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) override;
  void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands) override;

  void* getNativeHandle() const override { return m_cmd; }

private:
  enum class EncoderKind : uint8_t
  {
    none,
    render,
    compute,
  };

  VkCommandBuffer      m_cmd{nullptr};
  VulkanResourceTable* m_table{nullptr};
  VulkanRenderEncoder  m_renderEncoder;
  VulkanComputeEncoder m_computeEncoder;
  EncoderKind          m_active{EncoderKind::none};
};

}  // namespace demo::rhi::vulkan
