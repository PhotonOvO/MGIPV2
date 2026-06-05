#pragma once

#include "RHIEncoder.h"
#include "RHIHandles.h"
#include "RHIStageBarrier.h"
#include "RHITypes.h"

namespace demo::rhi {

// One-shot command buffer: a per-frame recording facade over the backend's
// native command buffer (owned by FrameContext, reset each frame). Obtained
// via FrameContext::getCommandBuffer(); not created/destroyed on Device.
class CommandBuffer
{
public:
  virtual ~CommandBuffer() = default;

  // Encoder factory. endEncoding() closes the current encoder.
  // Copy/blit live on ComputeEncoder (Metal 4-aligned); no separate copy pass.
  virtual RenderEncoder*  beginRenderPass(const RenderPassDesc& desc) = 0;
  virtual ComputeEncoder* beginComputePass()                          = 0;
  virtual void            endEncoding()                               = 0;

  // Main synchronization path: stage dependency + hazard class.
  virtual void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) = 0;

  // Special synchronization path (first-class, not deprecated): explicit
  // resource layout / queue ownership / present / aliasing transitions.
  virtual void resourceBarrier(const TextureBarrier* textures, uint32_t textureCount,
                               const BufferBarrier* buffers, uint32_t bufferCount) = 0;

  // Debug markers (RenderDoc / PIX / Tracy).
  virtual void beginEvent(const char* name) = 0;
  virtual void endEvent()                   = 0;

  // Timestamp queries (GPU profiling). The pool is an RHI handle resolved by the
  // backend; afterAllCommands selects bottom-of-pipe (true) vs top-of-pipe (false).
  virtual void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) = 0;
  virtual void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands) = 0;
};

}  // namespace demo::rhi
