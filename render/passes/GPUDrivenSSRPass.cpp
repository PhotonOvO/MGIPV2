#include "GPUDrivenSSRPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>

namespace demo {

GPUDrivenSSRPass::GPUDrivenSSRPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenSSRPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 4> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSSRPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmdBuffer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || !context.cameraAllocValid)
  {
    return;
  }

  const uint64_t cameraBuffer = context.transientAllocator->getBufferOpaque();
  const uint32_t cameraOffset = context.cameraAlloc.offset;
  if(!context.params->debugOptions.enableSSR || m_renderer->getSSRTracePipelineOpaque() == 0
     || m_renderer->getSSRRawImageOpaque() == 0 || cameraBuffer == 0)
  {
    return;
  }

  const PipelineHandle ssrPipeline = m_renderer->getSSRTracePipelineHandle();
  // Build this frame's SSR descriptor set (gbuffer/depth/history + ssrRaw + camera) as a
  // temporary bind group; it is recycled automatically at frame end.
  const BindGroupHandle ssrBindGroup = m_renderer->acquireSSRTempBindGroup(cameraBuffer, cameraOffset);
  if(ssrBindGroup.isNull() || ssrPipeline.isNull())
  {
    return;
  }

  const VkExtent2D halfExtent = m_renderer->getPhase7HalfExtent();
  const shaderio::GPUDrivenSSRPushConstants push{
      .params0 = glm::vec4(halfExtent.width,
                           halfExtent.height,
                           static_cast<float>(std::max(1, context.params->debugOptions.ssrMaxSteps)),
                           context.params->debugOptions.ssrThickness),
      .params1 = glm::vec4(0.05f, 80.0f, 1.0f, 0.0f),
  };

  rhi::ComputeEncoder* enc = context.cmdBuffer->beginComputePass();
  enc->setPipeline(ssrPipeline);
  enc->setArgumentTable(0, rhi::ArgumentTableHandle{ssrBindGroup.index, ssrBindGroup.generation});  // bridge (Wave 8)
  enc->setRootConstants(0, &push, sizeof(push));
  enc->dispatch(rhi::DispatchDesc{(halfExtent.width + 7u) / 8u, (halfExtent.height + 7u) / 8u, 1u});
  context.cmdBuffer->endEncoding();

  // SSR raw output is sampled by the lighting/composite fragment stage.
  context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::fragmentShader, rhi::HazardFlags::textureWrites);
}

}  // namespace demo
