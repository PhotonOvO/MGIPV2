#include "GPUDrivenLightCullingPass.h"

#include "../GPUDrivenRenderer.h"

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
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenLightCulling");

  const BindGroupHandle bindGroup = m_renderer->getCurrentLightCullingBindGroup();
  if(context.params->cameraUniforms != nullptr && !bindGroup.isNull())
  {
    rhi::ComputeEncoder* enc = context.cmdBuffer->beginComputePass();
    const rhi::ArgumentTableHandle argTable{bindGroup.index, bindGroup.generation};  // bridge (Wave 8)
    const auto dispatchLightKernel = [&](PipelineHandle handle, uint32_t lightCount) {
      if(handle.isNull() || lightCount == 0u)
      {
        return;
      }
      enc->setPipeline(handle);
      enc->setArgumentTable(0, argTable);
      enc->dispatch(rhi::DispatchDesc{(lightCount + kLightCoarseCullingThreadCount - 1u) / kLightCoarseCullingThreadCount, 1u, 1u});
    };

    dispatchLightKernel(m_renderer->getLightCullingPipelineHandle(), m_renderer->getActivePointLightCount());
    dispatchLightKernel(m_renderer->getSpotLightCullingPipelineHandle(), m_renderer->getActiveSpotLightCount());
    context.cmdBuffer->endEncoding();

    // Coarse bounds are consumed by the clustered-culling compute pass and the lighting fragment stage.
    context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                               rhi::HazardFlags::bufferWrites);
  }

  context.cmdBuffer->endEvent();
}

}  // namespace demo
