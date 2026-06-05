#include "GPUDrivenClusteredLightCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenClusteredLightCullingPass::GPUDrivenClusteredLightCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenClusteredLightCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 7> dependencies = {
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusteredLightUniformHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightCountsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightIndicesHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassClusterLightStatsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenClusteredLightCullingPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenClusteredLightCulling");

  const PipelineHandle pipeline = m_renderer->getClusteredLightCullingPipelineHandle();
  const BindGroupHandle bindGroup = m_renderer->getLightCoarseCullingBindGroup(context.frameIndex);
  if(context.params->debugOptions.enableClusteredLighting && !pipeline.isNull() && !bindGroup.isNull())
  {
    const rhi::BufferHandle statsBuffer = m_renderer->getClusterStatsBufferHandle(context.frameIndex);
    if(!statsBuffer.isNull())
    {
      rhi::ComputeEncoder* clear = context.cmdBuffer->beginComputePass();
      clear->fillBuffer(statsBuffer, 0, sizeof(GPUDrivenLightResources::ClusterStats), 0u);
      context.cmdBuffer->endEncoding();
      // Stats reset (transfer) must complete before the culling shader accumulates into it.
      context.cmdBuffer->barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute, rhi::HazardFlags::bufferWrites);
    }

    rhi::ComputeEncoder* enc = context.cmdBuffer->beginComputePass();
    enc->setPipeline(pipeline);
    enc->setArgumentTable(0, rhi::ArgumentTableHandle{bindGroup.index, bindGroup.generation});  // bridge (Wave 8)
    enc->dispatch(rhi::DispatchDesc{(shaderio::LClusterCount + 63u) / 64u, 1u, 1u});
    context.cmdBuffer->endEncoding();

    // Cluster light lists feed the lighting fragment stage; stats are read back on the host.
    context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::fragmentShader, rhi::HazardFlags::bufferWrites);
  }

  context.cmdBuffer->endEvent();
}

}  // namespace demo
