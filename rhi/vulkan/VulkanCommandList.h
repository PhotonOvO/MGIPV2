#pragma once

#include "../../common/Common.h"
#include "../RHICommandList.h"

#include <vector>

namespace demo::rhi::vulkan {

class VulkanCommandList final : public demo::rhi::CommandList
{
public:
  VulkanCommandList() = default;

  void setCommandBuffer(VkCommandBuffer commandBuffer) { m_commandBuffer = commandBuffer; }

  [[nodiscard]] VkCommandBuffer nativeHandle() const { return m_commandBuffer; }

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
  void bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers, const uint64_t* offsets, uint32_t bufferCount) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) override;
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

  VkCommandBuffer                 m_commandBuffer{VK_NULL_HANDLE};
  std::vector<ResourceStateEntry> m_resourceStates;
};

VkCommandBuffer getNativeCommandBuffer(demo::rhi::CommandList& commandList);
VkCommandBuffer getNativeCommandBuffer(const demo::rhi::CommandList& commandList);
void            cmdPipelineBarrier(const demo::rhi::CommandList& commandList, const VkDependencyInfo& dependencyInfo);
void            cmdBeginRendering(const demo::rhi::CommandList& commandList, const VkRenderingInfo& renderingInfo);
void            cmdEndRendering(const demo::rhi::CommandList& commandList);
void            cmdSetViewport(const demo::rhi::CommandList& commandList, const VkViewport& viewport);
void            cmdSetScissor(const demo::rhi::CommandList& commandList, const VkRect2D& scissor);
void            cmdPushConstants(const demo::rhi::CommandList& commandList, const VkPushConstantsInfo& pushInfo);
void cmdBindPipeline(const demo::rhi::CommandList& commandList, VkPipelineBindPoint bindPoint, VkPipeline pipeline);
void cmdBindDescriptorSets(const demo::rhi::CommandList& commandList,
                           VkPipelineBindPoint           bindPoint,
                           VkPipelineLayout              layout,
                           uint32_t                      firstSet,
                           uint32_t                      descriptorSetCount,
                           const VkDescriptorSet*        descriptorSets,
                           uint32_t                      dynamicOffsetCount,
                           const uint32_t*               dynamicOffsets);
void cmdBindDescriptorSetOpaque(const demo::rhi::CommandList& commandList,
                                uint32_t                      bindPoint,
                                uint64_t                      layoutHandle,
                                uint32_t                      firstSet,
                                uint64_t                      descriptorSetHandle,
                                uint32_t                      dynamicOffsetCount,
                                const uint32_t*               dynamicOffsets);
void cmdBindVertexBuffers(const demo::rhi::CommandList& commandList,
                          uint32_t                      firstBinding,
                          uint32_t                      bindingCount,
                          const VkBuffer*               buffers,
                          const VkDeviceSize*           offsets);
void cmdBindIndexBuffer(const demo::rhi::CommandList& commandList, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType);
void cmdDraw(const demo::rhi::CommandList& commandList, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
void cmdDrawIndexed(const demo::rhi::CommandList& commandList, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
void cmdDispatch(const demo::rhi::CommandList& commandList, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

}  // namespace demo::rhi::vulkan
