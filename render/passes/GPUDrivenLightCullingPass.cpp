#include "GPUDrivenLightCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>

namespace demo {

namespace {
constexpr uint32_t kLightCoarseCullingThreadCount = 64u;
}  // namespace

GPUDrivenLightCullingPass::GPUDrivenLightCullingPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenLightCullingPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 7> dependencies = {
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassSpotLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
      PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenLightCullingPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenLightCulling");

  const BindGroupHandle bindGroup = m_renderer->getCurrentLightCullingBindGroup();
  if(context.params->cameraUniforms != nullptr && !bindGroup.isNull())
  {
    const auto dispatchLightKernel = [&](PipelineHandle handle, uint32_t lightCount) {
      if(handle.isNull() || lightCount == 0u)
      {
        return;
      }
      context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, handle);
      context.cmd->bindBindGroup(0, bindGroup, nullptr, 0);
      context.cmd->dispatch((lightCount + kLightCoarseCullingThreadCount - 1u) / kLightCoarseCullingThreadCount, 1u, 1u);
    };

    dispatchLightKernel(m_renderer->getLightCullingPipelineHandle(), m_renderer->getActivePointLightCount());
    dispatchLightKernel(m_renderer->getSpotLightCullingPipelineHandle(), m_renderer->getActiveSpotLightCount());

    // Coarse bounds are consumed by the clustered-culling compute pass and the lighting fragment stage.
    context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                               rhi::PipelineStage::Compute | rhi::PipelineStage::FragmentShader,
                               rhi::ResourceAccess::read);
  }

  context.cmd->endEvent();
}

}  // namespace demo
