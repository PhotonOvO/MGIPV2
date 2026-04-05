#pragma once

#include "RHIHandles.h"
#include "RHIResourceLifetime.h"
#include "RHITypes.h"

#include <cstddef>
#include <cstdint>

namespace demo::rhi {

struct BufferBarrierDesc
{
  BufferHandle   buffer{};
  uint64_t       nativeBuffer{0};
  PipelineStage  srcStage{PipelineStage::TopOfPipe};
  PipelineStage  dstStage{PipelineStage::BottomOfPipe};
  ResourceAccess srcAccess{ResourceAccess::read};
  ResourceAccess dstAccess{ResourceAccess::read};
};

struct TextureBarrierDesc
{
  TextureHandle  texture{};
  uint64_t       nativeImage{0};
  TextureAspect  aspect{TextureAspect::color};
  PipelineStage  srcStage{PipelineStage::TopOfPipe};
  PipelineStage  dstStage{PipelineStage::BottomOfPipe};
  ResourceAccess srcAccess{ResourceAccess::read};
  ResourceAccess dstAccess{ResourceAccess::read};
  ResourceState  oldState{ResourceState::Undefined};
  ResourceState  newState{ResourceState::Undefined};
  bool           isSwapchain{false};
};

struct TextureViewDesc
{
  TextureHandle  texture{};
  TextureAspect  aspect{TextureAspect::color};
  uint32_t       baseMipLevel{0};
  uint32_t       mipLevelCount{1};
  uint32_t       baseArrayLayer{0};
  uint32_t       arrayLayerCount{1};
};

struct RenderTargetDesc
{
  TextureHandle     texture{};
  TextureViewHandle view{};            // Texture view for rendering
  ResourceState     state{ResourceState::general};
  LoadOp            loadOp{LoadOp::load};
  StoreOp           storeOp{StoreOp::store};
  ClearColorValue   clearColor{};
};

struct DepthTargetDesc
{
  TextureHandle          texture{};
  TextureViewHandle      view{};            // Texture view for rendering
  ResourceState          state{ResourceState::general};
  LoadOp                 loadOp{LoadOp::load};
  StoreOp                storeOp{StoreOp::store};
  ClearDepthStencilValue clearValue{};
};

struct RenderPassDesc
{
  Rect2D                  renderArea{};
  const RenderTargetDesc* colorTargets{nullptr};
  uint32_t                colorTargetCount{0};
  const DepthTargetDesc*  depthTarget{nullptr};
};

class CommandList
{
public:
  virtual ~CommandList() = default;

  virtual void begin() = 0;
  virtual void end()   = 0;

  virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
  virtual void endRenderPass()                             = 0;

  virtual void setViewport(const Viewport& viewport) = 0;
  virtual void setScissor(const Rect2D& scissor)     = 0;

  virtual void setResourceState(ResourceHandle resource, ResourceState state) = 0;
  virtual void insertBarrier(BarrierType barrierType)                         = 0;
  virtual void transitionBuffer(const BufferBarrierDesc& desc)                = 0;
  virtual void transitionTexture(const TextureBarrierDesc& desc)              = 0;

  virtual void bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) = 0;
  virtual void bindBindTable(PipelineBindPoint bindPoint,
                             uint32_t          slot,
                             BindTableHandle   bindTable,
                             const uint32_t*   dynamicOffsets,
                             uint32_t          dynamicOffsetCount)                = 0;
  virtual void bindBindGroup(uint32_t        slot,
                             BindGroupHandle bindGroup,
                             const uint32_t* dynamicOffsets,
                             uint32_t        dynamicOffsetCount)                  = 0;
  virtual void bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers, const uint64_t* offsets, uint32_t bufferCount) = 0;
  virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) = 0;
  virtual void pushConstants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) = 0;

  virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
  virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                           uint32_t firstIndex, int32_t vertexOffset,
                           uint32_t firstInstance) = 0;
  virtual void dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)                       = 0;

  // Debug marker/event support for profiling tools (RenderDoc, PIX, etc.)
  virtual void beginEvent(const char* name) = 0;
  virtual void endEvent()                   = 0;
};

}  // namespace demo::rhi
