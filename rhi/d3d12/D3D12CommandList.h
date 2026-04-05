#pragma once

#include "../../common/Common.h"
#include "../RHICommandList.h"

#include <vector>

namespace demo::rhi::d3d12 {

class D3D12CommandList final : public demo::rhi::CommandList
{
public:
  D3D12CommandList() = default;

  void setCommandList(void* commandList) { m_commandList = commandList; }

  [[nodiscard]] void* nativeHandle() const { return m_commandList; }  // ID3D12GraphicsCommandList

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

  // NOTES: D3D12 Command List Architecture
  // D3D12 uses command lists with allocators for recording commands:
  //
  // Command Hierarchy:
  // 1. ID3D12CommandQueue (submit command lists)
  // 2. ID3D12CommandAllocator (manage command memory per frame)
  // 3. ID3D12GraphicsCommandList (record graphics commands)
  // 4. ID3D12GraphicsCommandList (also used for compute/blit)
  //
  // Recording Flow:
  // - begin(): Reset command allocator, then Reset(commandList, allocator)
  // - Commands are recorded to command list
  // - end(): Close() the command list
  // - Submit via ExecuteCommandLists on command queue
  //
  // D3D12 Resource Barriers:
  // - Use ResourceBarrier() for buffer/image transitions
  // - D3D12_RESOURCE_TRANSITION_BARRIER for state changes
  // - D3D12_RESOURCE_UAV_BARRIER for UAV synchronization
  // - D3D12_RESOURCE_ALIASING_BARRIER for resource aliasing
  //
  // D3D12 has no explicit render passes like Vulkan:
  // - Use OMSetRenderTargets() to set render targets
  // - ClearRenderTargetView() and ClearDepthStencilView() for clearing
  // - No begin/end render pass, just set targets and render

  void* m_commandList{nullptr};                   // ID3D12GraphicsCommandList
  void* m_currentResourceBarrierBuffer{nullptr};  // D3D12_RESOURCE_BARRIER array

  std::vector<ResourceStateEntry> m_resourceStates;
};

}  // namespace demo::rhi::d3d12