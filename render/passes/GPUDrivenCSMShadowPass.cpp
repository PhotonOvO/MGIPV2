#include "GPUDrivenCSMShadowPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../DrawStreamRecorder.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>

namespace demo {

GPUDrivenCSMShadowPass::GPUDrivenCSMShadowPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenCSMShadowPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::texture(kPassCSMShadowHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenCSMShadowPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || context.commandBuffer == nullptr || context.executor == nullptr)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || !sceneView->usePersistentCullingObjects || sceneView->shadowPackedMeshes == nullptr
     || sceneView->shadowPackedMeshCount == 0 || sceneView->shadowPackedVertexBuffer == 0
     || sceneView->shadowPackedIndexBuffer == 0)
  {
    return;
  }

  shaderio::ShadowUniforms* shadowData = m_renderer->getShadowUniformsData();
  if(shadowData == nullptr)
  {
    return;
  }

  const PipelineHandle csmPipeline = m_renderer->getCSMShadowPipelineHandle();
  if(csmPipeline.isNull())
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenCSMShadow");

  CSMShadowResources& csm = m_renderer->getCSMShadowResources();
  const uint32_t cascadeCount = csm.getCascadeCount();

  const uint32_t frameIndex = context.frameIndex;
  const bool hasShadowIndirectBuffer = m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex) != 0;
  const bool hasDrawArgumentTables = !m_renderer->getCSMShadowMDIDrawArgumentTable(frameIndex, 0).isNull();
  const uint32_t shadowIndirectCapacity = m_renderer->getShadowCullingMeshCapacity(frameIndex);
  if(!hasShadowIndirectBuffer || !hasDrawArgumentTables
     || shadowIndirectCapacity < sceneView->shadowPackedMeshCount)
  {
    if(hasShadowIndirectBuffer && hasDrawArgumentTables
       && shadowIndirectCapacity < sceneView->shadowPackedMeshCount)
    {
      LOGW("Skipping GPUDrivenCSMShadow: indirect capacity %u smaller than shadow mesh count %u",
           shadowIndirectCapacity,
           sceneView->shadowPackedMeshCount);
    }
    context.commandBuffer->endEvent();
    return;
  }

  const bool useShadowCulling = !m_renderer->getShadowCullingPipelineHandle().isNull()
                                && !m_renderer->getShadowCullingArgumentTable(frameIndex).isNull();
  if(!useShadowCulling)
  {
    LOGW("Skipping GPUDrivenCSMShadow: shadow indirect culling pipeline is unavailable");
    context.commandBuffer->endEvent();
    return;
  }

  for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
  {
    if(useShadowCulling)
    {
      const PipelineHandle computePipeline = m_renderer->getShadowCullingPipelineHandle();
      const rhi::ArgumentTableHandle computeTable = m_renderer->getShadowCullingArgumentTable(frameIndex);
      const uint64_t indirectBuffer = m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex);
      if(!computePipeline.isNull() && !computeTable.isNull() && indirectBuffer != 0)
      {
        const shaderio::ShadowCullPushConstants pushConstants =
            m_renderer->buildShadowCullPushConstants(cascadeIndex, sceneView->shadowPackedMeshCount);
        rhi::ComputeEncoder* cenc = context.commandBuffer->beginComputePass();
        cenc->setPipeline(computePipeline);
        cenc->setArgumentTable(0, computeTable);
        cenc->setRootConstants(kPrimaryRootConstantsSlot, &pushConstants, sizeof(pushConstants));
        cenc->dispatch(rhi::DispatchDesc{(sceneView->shadowPackedMeshCount + shaderio::LGPUCullingThreadCount - 1u)
                                             / shaderio::LGPUCullingThreadCount,
                                         1u, 1u});
        context.commandBuffer->endEncoding();

        // Same-pass barrier: per-cascade culling writes indirect draw arguments
        // consumed by drawIndexedIndirect below inside the same cascade pass.
        context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::commandInput, rhi::HazardFlags::drawArguments);
      }
    }

    const rhi::Extent2D extent = csm.getCascadeExtent();

    const rhi::DepthTargetDesc depthTarget{
        .texture = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
        .view = m_renderer->getCSMCascadeViewHandle(cascadeIndex),
        .state = rhi::ResourceState::DepthStencilAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {0.0f, 0},
    };

    const rhi::RenderPassDesc passDesc{
        .renderArea = {{0, 0}, extent},
        .colorTargets = nullptr,
        .colorTargetCount = 0,
        .depthTarget = &depthTarget,
    };
    rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
    enc->setViewport(
        rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    enc->setScissor(rhi::Rect2D{{0, 0}, extent});

    enc->setPipeline(csmPipeline);
    const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);

    shaderio::CameraUniforms cascadeCamera{};
    cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
    cascadeCamera.projection = cascadeCamera.viewProjection;
    cascadeCamera.view = glm::mat4(1.0f);
    cascadeCamera.inverseViewProjection = glm::inverse(cascadeCamera.viewProjection);
    cascadeCamera.prevView = cascadeCamera.view;
    cascadeCamera.prevProjection = cascadeCamera.projection;
    cascadeCamera.prevViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.unjitteredInverseViewProjection = cascadeCamera.inverseViewProjection;
    cascadeCamera.prevUnjitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.prevJitteredViewProjection = cascadeCamera.viewProjection;
    cascadeCamera.cameraPosition = glm::vec3(0.0f);
    const float baseConstantBias = context.params->lightSettings.depthBias;
    const float baseSlopeBias = context.params->lightSettings.normalBias;
    const float biasScale = shadowData->cascadeBiasScale.z;
    const float cascadeBiasScale = 1.0f + static_cast<float>(cascadeIndex) * biasScale;
    const glm::vec3 lightTravelDir = glm::normalize(context.params->lightSettings.direction);
    const glm::vec3 dirToLight = -lightTravelDir;
    cascadeCamera.shadowConstantBias = baseConstantBias * cascadeBiasScale;
    cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, baseSlopeBias * cascadeBiasScale);
    const TransientAllocator::Allocation cameraAlloc = context.transientAllocator->allocateAndWrite(cascadeCamera, 256);

    const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(frameIndex);
    if(!cameraTable.isNull())
    {
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset, 0);
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
    }

    const rhi::ArgumentTableHandle drawTable = m_renderer->getCSMShadowMDIDrawArgumentTable(frameIndex, cascadeIndex);
    if(!drawTable.isNull())
    {
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, drawTable);

      const rhi::BufferHandle vertexBufferRHI = m_renderer->getShadowPackedVertexBufferRHIHandle();
      const rhi::BufferHandle indexBufferRHI  = m_renderer->getShadowPackedIndexBufferRHIHandle();
      constexpr uint64_t vertexOffset = 0;
      enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
      enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);
      DrawStreamRecorder::recordIndexedIndirect(*enc, DrawStreamRecorder::IndexedIndirectRecordDesc{
          .argsBuffer = m_renderer->getShadowCullingIndirectBufferRHIHandle(frameIndex),
          .offset     = 0,
          .drawCount  = sceneView->shadowPackedMeshCount,
          .stride     = 0,
      });
    }

    context.commandBuffer->endEncoding();
  }

  context.commandBuffer->endEvent();
}

}  // namespace demo
