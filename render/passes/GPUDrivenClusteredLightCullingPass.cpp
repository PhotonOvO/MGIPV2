#include "GPUDrivenClusteredLightCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenClusteredLightCulling");

  const PipelineHandle pipeline = m_renderer->getClusteredLightCullingPipelineHandle();
  const BindGroupHandle bindGroup = m_renderer->getLightCoarseCullingBindGroup(context.frameIndex);
  if(context.params->debugOptions.enableClusteredLighting && !pipeline.isNull() && !bindGroup.isNull())
  {
    const uint64_t statsBuffer = m_renderer->getClusterStatsBufferOpaque(context.frameIndex);
    if(statsBuffer != 0)
    {
      context.cmd->fillBuffer(statsBuffer, 0, sizeof(GPUDrivenLightResources::ClusterStats), 0u);
      // Stats reset (transfer) must complete before the culling shader accumulates into it.
      context.cmd->memoryBarrier(rhi::PipelineStage::Transfer, rhi::ResourceAccess::write,
                                 rhi::PipelineStage::Compute, rhi::ResourceAccess::write);
    }

    context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, pipeline);
    context.cmd->bindBindGroup(0, bindGroup, nullptr, 0);
    context.cmd->dispatch((shaderio::LClusterCount + 63u) / 64u, 1u, 1u);

    // Cluster light lists feed the lighting fragment stage; stats are read back on the host.
    context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                               rhi::PipelineStage::FragmentShader | rhi::PipelineStage::Host,
                               rhi::ResourceAccess::read);
  }

  context.cmd->endEvent();
}

}  // namespace demo
