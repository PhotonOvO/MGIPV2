#pragma once

#include "RHIArgumentTable.h"
#include "RHIHandles.h"
#include "RHIStageBarrier.h"
#include "RHITypes.h"

#include <cstdint>

namespace demo::rhi {

struct DrawDesc
{
  uint32_t vertexCount{0};
  uint32_t instanceCount{1};
  uint32_t firstVertex{0};
  uint32_t firstInstance{0};
};

struct DrawIndexedDesc
{
  BufferHandle indexBuffer{};
  uint64_t     indexBufferOffset{0};
  IndexFormat  indexFormat{IndexFormat::uint32};
  uint32_t     indexCount{0};
  uint32_t     instanceCount{1};
  uint32_t     firstIndex{0};
  int32_t      vertexOffset{0};
  uint32_t     firstInstance{0};
};

struct DrawIndirectDesc
{
  BufferHandle argsBuffer{};
  uint64_t     offset{0};
  uint32_t     drawCount{1};
  uint32_t     stride{0};  // 0 = backend default
};

struct DrawIndirectCountDesc
{
  BufferHandle argsBuffer{};
  uint64_t     argsOffset{0};
  BufferHandle countBuffer{};
  uint64_t     countBufferOffset{0};
  uint32_t     maxDrawCount{0};
  uint32_t     stride{0};
};

struct DispatchDesc
{
  uint32_t groupCountX{1};
  uint32_t groupCountY{1};
  uint32_t groupCountZ{1};
};

struct DispatchIndirectDesc
{
  BufferHandle argsBuffer{};
  uint64_t     offset{0};
};

struct BufferTextureCopyDesc
{
  BufferHandle  buffer{};
  uint64_t      bufferOffset{0};
  TextureHandle texture{};
  TextureAspect aspect{TextureAspect::color};
  uint32_t      mipLevel{0};
  uint32_t      baseArrayLayer{0};
  uint32_t      layerCount{1};
  uint32_t      width{0};
  uint32_t      height{0};
  uint32_t      depth{1};
};

struct TextureBlitDesc
{
  TextureHandle srcTexture{};
  TextureHandle dstTexture{};
  TextureAspect aspect{TextureAspect::color};
  Offset3D      srcOffsets[2]{};
  Offset3D      dstOffsets[2]{};
};

// setRoot*/setDynamicBuffer carry ShaderStage so slot visibility is explicit
// (aligns with the existing pushConstants(ShaderStage,...) model and future
// RootBindingSchema). Encoders hold no global mutable state so they remain
// safe to extend toward multi-threaded recording later.
class RenderEncoder
{
public:
  virtual ~RenderEncoder() = default;

  virtual void setPipeline(PipelineHandle pipeline)                                              = 0;
  virtual void setArgumentTable(ShaderStage stages, uint32_t slot, ArgumentTableHandle table)    = 0;  // per-stage (Metal4); Vulkan binds pipeline-wide
  virtual void setDynamicBuffer(ShaderStage stages, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) = 0;
  virtual void setRootConstants(ShaderStage stages, uint32_t slot, const void* data, uint32_t size)                     = 0;
  virtual void setRootPointer(ShaderStage stages, uint32_t slot, GpuPtr ptr)                                            = 0;

  virtual void setViewport(const Viewport& viewport) = 0;
  virtual void setScissor(const Rect2D& scissor)     = 0;

  virtual void bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers, const uint64_t* offsets, uint32_t count) = 0;
  virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)                                     = 0;

  // Reserved for tile-resident deferred (input attachment / local read).
  virtual void readInputAttachment(uint32_t index) = 0;

  virtual void draw(const DrawDesc& desc)                                     = 0;
  virtual void drawIndexed(const DrawIndexedDesc& desc)                       = 0;
  virtual void drawIndexedIndirect(const DrawIndirectDesc& desc)              = 0;
  virtual void drawIndexedIndirect(GpuPtr args, uint32_t count, uint32_t stride) = 0;
  virtual void drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)    = 0;
  virtual void drawIndirect(const DrawIndirectDesc& desc)                     = 0;

  // Mesh shader (capability-gated).
  virtual void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
  virtual void drawMeshTasksIndirect(const DrawIndirectDesc& desc)                             = 0;
};

// No standalone CopyEncoder: aligning with Metal 4, copy/blit/fill are a command
// subset of ComputeEncoder (Metal 4 folds these into MTL4ComputeCommandEncoder).
class ComputeEncoder
{
public:
  virtual ~ComputeEncoder() = default;

  // --- compute (stage is implicitly compute; no ShaderStage param) ---
  virtual void setPipeline(PipelineHandle pipeline)                       = 0;
  virtual void setArgumentTable(uint32_t slot, ArgumentTableHandle table) = 0;
  virtual void setRootConstants(uint32_t slot, const void* data, uint32_t size) = 0;
  virtual void setRootPointer(uint32_t slot, GpuPtr ptr)                         = 0;

  virtual void dispatch(const DispatchDesc& desc)                 = 0;
  virtual void dispatchIndirect(const DispatchIndirectDesc& desc) = 0;
  virtual void dispatchIndirect(GpuPtr args)                      = 0;

  // --- copy / blit (command subset) ---
  virtual void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset, uint64_t size) = 0;
  virtual void copyBufferToTexture(const BufferTextureCopyDesc& desc)                                                = 0;
  virtual void copyTextureToBuffer(const BufferTextureCopyDesc& desc)                                                = 0;
  virtual void blitTexture(const TextureBlitDesc& desc)                                                              = 0;
  virtual void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)                        = 0;
};

}  // namespace demo::rhi
