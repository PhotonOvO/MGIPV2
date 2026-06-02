#include "GPUDrivenCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenCulling");

  const RenderParams& params = *context.params;
  const uint32_t safeObjectCount = m_renderer->getSafePersistentObjectCount();
  const bool useExternalPersistentObjects = params.gpuDrivenSceneView != nullptr
                                            && params.gpuDrivenSceneView->usePersistentCullingObjects
                                            && params.gpuDrivenSceneView->gpuCullObjectBuffer != VK_NULL_HANDLE
                                            && safeObjectCount > 0u;

  if(params.cameraUniforms != nullptr && !m_renderer->getGPUCullingPipelineHandle().isNull()
     && m_renderer->getGPUCullingPipelineLayout() != 0)
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
      context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, m_renderer->getGPUCullingPipelineHandle());
      context.cmd->bindBindGroup(0, bindGroup, nullptr, 0);
      context.cmd->dispatch((objectCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount, 1u, 1u);

      // Culling output (indirect args + draw count) is consumed by drawIndexedIndirectCount.
      const auto barrierToIndirect = [&](uint64_t buffer) {
        context.cmd->transitionBuffer(rhi::BufferBarrierDesc{
            .nativeBuffer = buffer,
            .srcStage     = rhi::PipelineStage::Compute,
            .dstStage     = rhi::PipelineStage::DrawIndirect,
            .srcAccess    = rhi::ResourceAccess::write,
            .dstAccess    = rhi::ResourceAccess::read,
        });
      };
      barrierToIndirect(indirectBuffer);
      barrierToIndirect(drawCountBuffer);
    }
  }

  context.cmd->endEvent();
}

}  // namespace demo
