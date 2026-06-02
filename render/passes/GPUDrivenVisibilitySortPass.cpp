#include "GPUDrivenVisibilitySortPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

void recordVisibilitySort(const PassContext& context, GPUDrivenRenderer& renderer)
{
  if(context.cmd == nullptr)
  {
    return;
  }

  const GPUDrivenRenderer::VisibilitySortDispatch sort = renderer.getVisibilitySortDispatch(context.frameIndex);
  if(!sort.valid)
  {
    return;
  }

  if(sort.pipelineHandle.isNull() || sort.bindGroup.isNull())
  {
    return;
  }

  const uint64_t copySize = static_cast<uint64_t>(sort.paddedElementCount) * sizeof(uint32_t);
  context.cmd->copyBuffer(sort.uploadKeyBuffer, sort.keyBuffer, 0, 0, copySize);
  context.cmd->copyBuffer(sort.uploadValueBuffer, sort.valueBuffer, 0, 0, copySize);

  // Both staged buffers must finish uploading before the sort shader reads/writes them.
  context.cmd->transitionBuffer(rhi::BufferBarrierDesc{
      .nativeBuffer = sort.keyBuffer,
      .srcStage     = rhi::PipelineStage::Transfer,
      .dstStage     = rhi::PipelineStage::Compute,
      .srcAccess    = rhi::ResourceAccess::write,
      .dstAccess    = rhi::ResourceAccess::readWrite,
  });
  context.cmd->transitionBuffer(rhi::BufferBarrierDesc{
      .nativeBuffer = sort.valueBuffer,
      .srcStage     = rhi::PipelineStage::Transfer,
      .dstStage     = rhi::PipelineStage::Compute,
      .srcAccess    = rhi::ResourceAccess::write,
      .dstAccess    = rhi::ResourceAccess::readWrite,
  });

  context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, sort.pipelineHandle);
  context.cmd->bindBindGroup(0, sort.bindGroup, nullptr, 0);

  // Each bitonic step reads what the previous step wrote, in place.
  const auto issueBarrier = [&]() {
    context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                               rhi::PipelineStage::Compute, rhi::ResourceAccess::readWrite);
  };

  for(uint32_t level = 2u; level <= sort.paddedElementCount; level <<= 1u)
  {
    for(uint32_t levelMask = level >> 1u; levelMask > 0u; levelMask >>= 1u)
    {
      const shaderio::BitonicSortPushConstants pushConstants{
          .elementCount = sort.paddedElementCount,
          .level = level,
          .levelMask = levelMask,
          .descending = 1u,
      };
      context.cmd->pushConstants(rhi::ShaderStage::compute, 0, sizeof(pushConstants), &pushConstants);
      context.cmd->dispatch((sort.paddedElementCount + 63u) / 64u, 1u, 1u);
      issueBarrier();
    }
  }
}

GPUDrivenVisibilitySortPass::GPUDrivenVisibilitySortPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenVisibilitySortPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::buffer(kPassGPUDrivenSortKeyBufferHandle, ResourceAccess::readWrite, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUDrivenSortValueBufferHandle, ResourceAccess::readWrite, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenVisibilitySortPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenVisibilitySort");
  recordVisibilitySort(context, *m_renderer);
  context.cmd->endEvent();
}

}  // namespace demo
