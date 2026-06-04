#include "GPUDrivenCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenCullingPass::GPUDrivenCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 6> dependencies = {
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullIndirectBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullStatsBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullUniformBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::write, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenCullingPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  rhi::CommandBuffer* cmdBuffer = context.cmdBuffer;
  cmdBuffer->beginEvent("GPUDrivenCulling");

  const RenderParams& params = *context.params;
  const uint32_t safeObjectCount = m_renderer->getSafePersistentObjectCount();
  const bool useExternalPersistentObjects = params.gpuDrivenSceneView != nullptr
                                            && params.gpuDrivenSceneView->usePersistentCullingObjects
                                            && params.gpuDrivenSceneView->gpuCullObjectBuffer != VK_NULL_HANDLE
                                            && safeObjectCount > 0u;

  if(params.cameraUniforms != nullptr && !m_renderer->getGPUCullingPipelineHandle().isNull())
  {
    const uint32_t currentFrameIndex = context.frameIndex;
    const uint32_t objectCount =
        useExternalPersistentObjects
            ? safeObjectCount
            : (params.gltfModel != nullptr ? static_cast<uint32_t>(params.gltfModel->meshes.size()) : 0u);
    const BindGroupHandle bindGroup = m_renderer->getGPUCullingBindGroup(currentFrameIndex);
    const uint64_t indirectBuffer = m_renderer->getGPUCullingIndirectBufferOpaque(currentFrameIndex);
    const uint64_t drawCountBuffer = m_renderer->getGPUCullingDrawCountBufferOpaque(currentFrameIndex);
    if(objectCount != 0u && !bindGroup.isNull() && indirectBuffer != 0 && drawCountBuffer != 0)
    {
      rhi::ComputeEncoder* enc = cmdBuffer->beginComputePass();
      enc->setPipeline(m_renderer->getGPUCullingPipelineHandle());
      // Transitional: pass the legacy bind group's bits as an ArgumentTableHandle (resolved
      // via the bind-group bridge in VulkanResourceTable; replaced by real ArgumentTable in Wave 8).
      enc->setArgumentTable(0, rhi::ArgumentTableHandle{bindGroup.index, bindGroup.generation});
      enc->dispatch(rhi::DispatchDesc{
          .groupCountX = (objectCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount,
          .groupCountY = 1u,
          .groupCountZ = 1u});
      cmdBuffer->endEncoding();

      // Culling output (indirect args + draw count) is consumed by drawIndexedIndirectCount.
      cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::commandInput,
                         rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites);
    }
  }

  cmdBuffer->endEvent();
}

}  // namespace demo
