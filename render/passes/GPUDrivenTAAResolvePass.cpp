#include "GPUDrivenTAAResolvePass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

GPUDrivenTAAResolvePass::GPUDrivenTAAResolvePass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenTAAResolvePass::getDependencies() const
{
  static const std::array<PassResourceDependency, 4> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassVelocityHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHistoryWriteHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenTAAResolvePass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params == nullptr)
  {
    return;
  }

  const bool taaEnabled = context.params->debugOptions.enablePostProcessing && context.params->debugOptions.enableTAA;
  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || !taaEnabled || sceneView->sceneColorHistoryWriteImage.isNull()
     || sceneView->sceneColorHistoryWriteView.isNull())
  {
    return;
  }

  const rhi::Extent2D extent = m_renderer->getSceneExtent();
  if(extent.width == 0u || extent.height == 0u)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenTAAResolve");
  const rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = sceneView->sceneColorHistoryWriteView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };
  rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle pipelineHandle = m_renderer->getTAAResolvePipelineHandle();
  const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
  if(!pipelineHandle.isNull() && !inputTable.isNull() && context.cameraAllocValid)
  {
    enc->setPipeline(pipelineHandle);
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

    const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
    const shaderio::PostProcessUniforms postProcessUniforms{
        .params0 = glm::vec4(context.params->debugOptions.postExposure,
                             context.params->debugOptions.bloomIntensity,
                             context.params->debugOptions.bloomThreshold,
                             context.params->debugOptions.enableBloom ? 1.0f : 0.0f),
        .params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, extent.width)),
                             1.0f / static_cast<float>(std::max(1u, extent.height)),
                             1.0f / static_cast<float>(std::max(1u, extent.width)),
                             1.0f / static_cast<float>(std::max(1u, extent.height))),
        .params2 = glm::vec4(0.0f),
        .params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .params4 = glm::vec4(0.0f),
        .params5 = glm::vec4(1.0f,
                             m_renderer->isTAAHistoryValid() ? 1.0f : 0.0f,
                             std::clamp(context.params->debugOptions.taaBlendWeight, 0.0f, 0.98f),
                             0.0f),
    };
    const TransientAllocator::Allocation postProcessAlloc =
        context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
    std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
    context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
    const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
    if(!sceneTable.isNull())
    {
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset, 0);
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, postProcessAlloc.offset, 0);
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
      enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
    }
  }

  context.commandBuffer->endEncoding();
  context.commandBuffer->endEvent();
}

}  // namespace demo
