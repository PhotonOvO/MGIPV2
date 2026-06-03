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

// Image-to-image blit (e.g. scene-output → swapchain present). Images are opaque
// native handles; the regions carry explicit source/destination offsets so callers
// can letterbox. Sampling uses a linear filter.
struct ImageBlitDesc
{
  uint64_t      srcImage{0};
  uint64_t      dstImage{0};
  ResourceState srcState{ResourceState::TransferSrc};
  ResourceState dstState{ResourceState::TransferDst};
  TextureAspect aspect{TextureAspect::color};
  Offset3D      srcOffsets[2]{};
  Offset3D      dstOffsets[2]{};
};

// Clear a color image (RHI handle). `state` is the image's current layout at clear time.
struct ClearColorImageDesc
{
  TextureHandle   image{};
  ClearColorValue color{};
  ResourceState   state{ResourceState::general};
  TextureAspect   aspect{TextureAspect::color};
  uint32_t        baseMipLevel{0};
  uint32_t        levelCount{1};
  uint32_t        baseArrayLayer{0};
  uint32_t        layerCount{1};
};

// Upload a buffer region into an image (RHI handle). The caller transitions the image to
// `dstState` (transfer-dst) beforehand; this verb only records the copy.
struct BufferImageCopyDesc
{
  uint64_t      srcBuffer{0};
  TextureHandle dstImage{};
  ResourceState dstState{ResourceState::TransferDst};
  uint64_t      bufferOffset{0};
  TextureAspect aspect{TextureAspect::color};
  uint32_t      mipLevel{0};
  uint32_t      baseArrayLayer{0};
  uint32_t      layerCount{1};
  uint32_t      width{0};
  uint32_t      height{0};
  uint32_t      depth{1};
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
  // Global memory barrier with explicit src/dst scopes (replaces inline VkMemoryBarrier2 usage in passes).
  virtual void memoryBarrier(PipelineStage  srcStage,
                             ResourceAccess srcAccess,
                             PipelineStage  dstStage,
                             ResourceAccess dstAccess)                         = 0;
  virtual void transitionBuffer(const BufferBarrierDesc& desc)                = 0;
  virtual void transitionTexture(const TextureBarrierDesc& desc)              = 0;

  // Transfer / blit verbs. Buffers and images are opaque native handles.
  virtual void copyBuffer(uint64_t srcBuffer, uint64_t dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
  virtual void fillBuffer(uint64_t dstBuffer, uint64_t offset, uint64_t size, uint32_t data)                             = 0;
  virtual void blitImage(const ImageBlitDesc& desc)                                                                      = 0;
  virtual void clearColorImage(const ClearColorImageDesc& desc)                                                          = 0;
  virtual void copyBufferToImage(const BufferImageCopyDesc& desc)                                                        = 0;

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
  // Bind vertex buffers with opaque handles (can store native pointers)
  virtual void bindVertexBuffers(uint32_t firstBinding, const uint64_t* bufferHandles,
                                 const uint64_t* offsets, uint32_t bufferCount) = 0;
  // Bind index buffer with opaque handle
  virtual void bindIndexBuffer(uint64_t bufferHandle, uint64_t offset, IndexFormat format) = 0;
  virtual void pushConstants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) = 0;

  virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
  virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                           uint32_t firstIndex, int32_t vertexOffset,
                           uint32_t firstInstance) = 0;
  virtual void drawIndexedIndirect(uint64_t bufferHandle, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
  virtual void drawIndexedIndirectCount(uint64_t bufferHandle,
                                        uint64_t offset,
                                        uint64_t countBufferHandle,
                                        uint64_t countBufferOffset,
                                        uint32_t maxDrawCount,
                                        uint32_t stride) = 0;
  virtual void dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)                       = 0;

  // Debug marker/event support for profiling tools (RenderDoc, PIX, etc.)
  virtual void beginEvent(const char* name) = 0;
  virtual void endEvent()                   = 0;
};

}  // namespace demo::rhi
