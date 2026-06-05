#include "GPUDrivenVisibilitySortPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

void recordVisibilitySort(const PassContext& context, GPUDrivenRenderer& renderer)
{
  if(context.cmdBuffer == nullptr)
  {
    return;
  }

  const GPUDrivenRenderer::VisibilitySortDispatch sort = renderer.getVisibilitySortDispatch(context.frameIndex);
  if(!sort.valid)
  {
    return;
  }

  if(sort.pipelineHandle.isNull() || sort.bindGroup.isNull() || sort.uploadKeyBufferHandle.isNull()
     || sort.uploadValueBufferHandle.isNull() || sort.keyBufferHandle.isNull() || sort.valueBufferHandle.isNull())
  {
    return;
  }

  const uint64_t copySize = static_cast<uint64_t>(sort.paddedElementCount) * sizeof(uint32_t);
  rhi::ComputeEncoder* copyEnc = context.cmdBuffer->beginComputePass();
  copyEnc->copyBuffer(sort.uploadKeyBufferHandle, 0, sort.keyBufferHandle, 0, copySize);
  copyEnc->copyBuffer(sort.uploadValueBufferHandle, 0, sort.valueBufferHandle, 0, copySize);
  context.cmdBuffer->endEncoding();

  // Both staged buffers must finish uploading before the sort shader reads/writes them.
  context.cmdBuffer->barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute, rhi::HazardFlags::bufferWrites);

  const rhi::ArgumentTableHandle argTable{sort.bindGroup.index, sort.bindGroup.generation};  // bridge (Wave 8)
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
      rhi::ComputeEncoder* enc = context.cmdBuffer->beginComputePass();
      enc->setPipeline(sort.pipelineHandle);
      enc->setArgumentTable(0, argTable);
      enc->setRootConstants(0, &pushConstants, sizeof(pushConstants));
      enc->dispatch(rhi::DispatchDesc{(sort.paddedElementCount + 63u) / 64u, 1u, 1u});
      context.cmdBuffer->endEncoding();
      // Each bitonic step reads what the previous step wrote, in place.
      context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute, rhi::HazardFlags::bufferWrites);
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
  if(m_renderer == nullptr || context.cmdBuffer == nullptr)
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenVisibilitySort");
  recordVisibilitySort(context, *m_renderer);
  context.cmdBuffer->endEvent();
}

}  // namespace demo
