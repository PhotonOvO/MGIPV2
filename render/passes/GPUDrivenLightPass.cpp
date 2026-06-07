#include "GPUDrivenLightPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenLightPass::GPUDrivenLightPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenLightPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 9> dependencies = {
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBuffer2Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ShaderRead),
      PassResourceDependency::texture(kPassCSMShadowHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ShaderRead),
      PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenLightPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr
     || context.transientAllocator == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenLightPass");

  const GPUDrivenSceneView* sceneView = context.params != nullptr ? context.params->gpuDrivenSceneView : nullptr;
  if(sceneView == nullptr || sceneView->sceneColorHdrView.isNull())
  {
    context.commandBuffer->endEvent();
    return;
  }

  rhi::TextureViewHandle outputViewHandle = sceneView->sceneColorHdrView;
  const rhi::Extent2D extent = sceneView->sceneDepthExtent;
  if(outputViewHandle.isNull())
  {
    context.commandBuffer->endEvent();
    return;
  }

  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = outputViewHandle,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  const rhi::RenderPassDesc passDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  };
  rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle lightPipeline = m_renderer->getGPUDrivenLightHdrPipelineHandle();
  if(lightPipeline.isNull())
  {
    context.commandBuffer->endEncoding();
    context.commandBuffer->endEvent();
    return;
  }

  enc->setPipeline(lightPipeline);
  const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
  enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

  if(!context.cameraAllocValid)
  {
    context.commandBuffer->endEncoding();
    context.commandBuffer->endEvent();
    return;
  }

  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
  const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
  if(!sceneTable.isNull())
  {
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset, 0);
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
    enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
  }

  enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
  context.commandBuffer->endEncoding();

  context.commandBuffer->endEvent();
}

}  // namespace demo
