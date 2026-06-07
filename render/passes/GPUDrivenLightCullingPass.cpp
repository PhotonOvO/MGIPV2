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
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
                                      rhi::ResourceState::ShaderRead),
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
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenLightCulling");

  const rhi::ArgumentTableHandle argumentTable = m_renderer->getCurrentLightCullingArgumentTable();
  if(context.params->cameraUniforms != nullptr && !argumentTable.isNull())
  {
    rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
    const auto dispatchLightKernel = [&](PipelineHandle handle, uint32_t lightCount) {
      if(handle.isNull() || lightCount == 0u)
      {
        return;
      }
      enc->setPipeline(handle);
      enc->setArgumentTable(0, argumentTable);
      enc->dispatch(rhi::DispatchDesc{(lightCount + kLightCoarseCullingThreadCount - 1u) / kLightCoarseCullingThreadCount, 1u, 1u});
    };

    dispatchLightKernel(m_renderer->getLightCullingPipelineHandle(), m_renderer->getActivePointLightCount());
    dispatchLightKernel(m_renderer->getSpotLightCullingPipelineHandle(), m_renderer->getActiveSpotLightCount());
    context.commandBuffer->endEncoding();

    // Local output barrier: point/spot light kernels write coarse bounds consumed
    // by later culling/lighting paths through resources outside this execute body.
    context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
                               rhi::HazardFlags::bufferWrites);
  }

  context.commandBuffer->endEvent();
}

}  // namespace demo
