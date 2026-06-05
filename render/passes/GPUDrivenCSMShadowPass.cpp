#include "GPUDrivenCSMShadowPass.h"

#include "../GPUDrivenRenderer.h"
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
     || context.cmdBuffer == nullptr || context.executor == nullptr)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || !sceneView->usePersistentCullingObjects || sceneView->shadowPackedMeshes == nullptr
     || sceneView->shadowPackedMeshCount == 0 || sceneView->shadowPackedVertexBuffer == VK_NULL_HANDLE
     || sceneView->shadowPackedIndexBuffer == VK_NULL_HANDLE)
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

  context.cmdBuffer->beginEvent("GPUDrivenCSMShadow");

  CSMShadowResources& csm = m_renderer->getCSMShadowResources();
  const uint32_t cascadeCount = csm.getCascadeCount();
  const auto transitionCascade = [&](rhi::ResourceState before, rhi::ResourceState after) {
    const rhi::TextureBarrier barrier{
        .texture = context.executor->resolveBarrierTexture(reinterpret_cast<uint64_t>(csm.getCascadeImage())),
        .before  = before,
        .after   = after,
        .range   = {.aspect = rhi::TextureAspect::depth, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0, .layerCount = ~0u},
    };
    context.cmdBuffer->resourceBarrier(&barrier, 1, nullptr, 0);
  };
  transitionCascade(rhi::ResourceState::General, rhi::ResourceState::DepthStencilAttachment);

  const uint32_t frameIndex = context.frameIndex;
  const bool hasShadowIndirectBuffer = m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex) != 0;
  const bool hasDrawBindGroups = !m_renderer->getCSMShadowMDIDrawBindGroup(frameIndex, 0).isNull();
  const uint32_t shadowIndirectCapacity = m_renderer->getShadowCullingMeshCapacity(frameIndex);
  if(!hasShadowIndirectBuffer || !hasDrawBindGroups
     || shadowIndirectCapacity < sceneView->shadowPackedMeshCount)
  {
    if(hasShadowIndirectBuffer && hasDrawBindGroups
       && shadowIndirectCapacity < sceneView->shadowPackedMeshCount)
    {
      LOGW("Skipping GPUDrivenCSMShadow: indirect capacity %u smaller than shadow mesh count %u",
           shadowIndirectCapacity,
           sceneView->shadowPackedMeshCount);
    }
    context.cmdBuffer->endEvent();
    return;
  }

  const bool useShadowCulling = !m_renderer->getShadowCullingPipelineHandle().isNull()
                                && m_renderer->getShadowCullingPipelineLayout() != 0
                                && m_renderer->getShadowCullingDescriptorSetOpaque(frameIndex) != 0;
  if(!useShadowCulling)
  {
    LOGW("Skipping GPUDrivenCSMShadow: shadow indirect culling pipeline is unavailable");
    context.cmdBuffer->endEvent();
    return;
  }

  for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
  {
    if(useShadowCulling)
    {
      const PipelineHandle computePipeline = m_renderer->getShadowCullingPipelineHandle();
      const BindGroupHandle computeBindGroup = m_renderer->getShadowCullingBindGroup(frameIndex);
      const uint64_t indirectBuffer = m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex);
      if(!computePipeline.isNull() && !computeBindGroup.isNull() && indirectBuffer != 0)
      {
        const shaderio::ShadowCullPushConstants pushConstants =
            m_renderer->buildShadowCullPushConstants(cascadeIndex, sceneView->shadowPackedMeshCount);
        rhi::ComputeEncoder* cenc = context.cmdBuffer->beginComputePass();
        cenc->setPipeline(computePipeline);
        cenc->setArgumentTable(0, rhi::ArgumentTableHandle{computeBindGroup.index, computeBindGroup.generation});  // bridge (Wave 8)
        cenc->setRootConstants(0, &pushConstants, sizeof(pushConstants));
        cenc->dispatch(rhi::DispatchDesc{(sceneView->shadowPackedMeshCount + shaderio::LGPUCullingThreadCount - 1u)
                                             / shaderio::LGPUCullingThreadCount,
                                         1u, 1u});
        context.cmdBuffer->endEncoding();

        // Per-cascade indirect draw args are consumed by drawIndexedIndirect below.
        context.cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::commandInput, rhi::HazardFlags::drawArguments);
      }
    }

    const VkExtent2D cascadeExtent = csm.getCascadeExtent();
    const rhi::Extent2D extent{cascadeExtent.width, cascadeExtent.height};

    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
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
    rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(passDesc);
    enc->setViewport(
        rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    enc->setScissor(rhi::Rect2D{{0, 0}, extent});

    enc->setPipeline(csmPipeline);
    const BindGroupHandle materialBindGroup = m_renderer->getGraphicsMaterialBindGroup();
    enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                          rhi::ArgumentTableHandle{materialBindGroup.index, materialBindGroup.generation});  // bridge (Wave 8)

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

    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(frameIndex);
    if(!cameraBindGroupHandle.isNull())
    {
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, cameraAlloc.offset, 0);
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, 0, 0);
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                            rhi::ArgumentTableHandle{cameraBindGroupHandle.index, cameraBindGroupHandle.generation});  // bridge
    }

    const BindGroupHandle drawBindGroupHandle = m_renderer->getCSMShadowMDIDrawBindGroup(frameIndex, cascadeIndex);
    if(!drawBindGroupHandle.isNull())
    {
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw,
                            rhi::ArgumentTableHandle{drawBindGroupHandle.index, drawBindGroupHandle.generation});  // bridge

      const rhi::BufferHandle vertexBufferRHI = m_renderer->getShadowPackedVertexBufferRHIHandle();
      const rhi::BufferHandle indexBufferRHI  = m_renderer->getShadowPackedIndexBufferRHIHandle();
      constexpr uint64_t vertexOffset = 0;
      enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
      enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);
      enc->drawIndexedIndirect(rhi::DrawIndirectDesc{
          .argsBuffer = m_renderer->getShadowCullingIndirectBufferRHIHandle(frameIndex),
          .offset     = 0,
          .drawCount  = sceneView->shadowPackedMeshCount,
          .stride     = sizeof(VkDrawIndexedIndirectCommand),
      });
    }

    context.cmdBuffer->endEncoding();
  }

  transitionCascade(rhi::ResourceState::DepthStencilAttachment, rhi::ResourceState::General);

  context.cmdBuffer->endEvent();
}

}  // namespace demo
