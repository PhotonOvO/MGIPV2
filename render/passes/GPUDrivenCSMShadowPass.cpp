#include "GPUDrivenCSMShadowPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr || context.cmd == nullptr)
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
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getCSMShadowPipelineLayout());
  if(csmPipeline.isNull() || pipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(m_renderer->getNativeGraphicsPipeline(csmPipeline));
  if(nativePipeline == VK_NULL_HANDLE)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenCSMShadow");

  CSMShadowResources& csm = m_renderer->getCSMShadowResources();
  const uint32_t cascadeCount = csm.getCascadeCount();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
      .aspect      = rhi::TextureAspect::depth,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

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
    context.cmd->endEvent();
    return;
  }

  const bool useShadowCulling = !m_renderer->getShadowCullingPipelineHandle().isNull()
                                && m_renderer->getShadowCullingPipelineLayout() != 0
                                && m_renderer->getShadowCullingDescriptorSetOpaque(frameIndex) != 0;
  if(!useShadowCulling)
  {
    LOGW("Skipping GPUDrivenCSMShadow: shadow indirect culling pipeline is unavailable");
    context.cmd->endEvent();
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
        context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, computePipeline);
        context.cmd->bindBindGroup(0, computeBindGroup, nullptr, 0);
        context.cmd->pushConstants(rhi::ShaderStage::compute, 0, sizeof(pushConstants), &pushConstants);
        context.cmd->dispatch((sceneView->shadowPackedMeshCount + shaderio::LGPUCullingThreadCount - 1u)
                                  / shaderio::LGPUCullingThreadCount,
                              1u,
                              1u);

        // Per-cascade indirect draw args are consumed by drawIndexedIndirect below.
        context.cmd->transitionBuffer(rhi::BufferBarrierDesc{
            .nativeBuffer = indirectBuffer,
            .srcStage     = rhi::PipelineStage::Compute,
            .dstStage     = rhi::PipelineStage::DrawIndirect,
            .srcAccess    = rhi::ResourceAccess::write,
            .dstAccess    = rhi::ResourceAccess::read,
        });
      }
    }

    const VkImageView layerView = csm.getCascadeLayerView(cascadeIndex);
    const VkExtent2D cascadeExtent = csm.getCascadeExtent();
    const rhi::Extent2D extent{cascadeExtent.width, cascadeExtent.height};

    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
        .view = rhi::TextureViewHandle::fromNative(layerView),
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
    context.cmd->beginRenderPass(passDesc);
    context.cmd->setViewport(
        rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

    context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, csmPipeline);
    context.cmd->bindBindGroup(shaderio::LSetTextures, m_renderer->getGraphicsMaterialBindGroup(), nullptr, 0);

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
      const uint32_t dynamicOffsets[] = {cameraAlloc.offset, 0u};
      context.cmd->bindBindGroup(shaderio::LSetScene, cameraBindGroupHandle, dynamicOffsets, 2);
    }

    const BindGroupHandle drawBindGroupHandle = m_renderer->getCSMShadowMDIDrawBindGroup(frameIndex, cascadeIndex);
    if(!drawBindGroupHandle.isNull())
    {
      context.cmd->bindBindGroup(shaderio::LSetDraw, drawBindGroupHandle, nullptr, 0);

      const uint64_t vertexBuffer = reinterpret_cast<uint64_t>(sceneView->shadowPackedVertexBuffer);
      const uint64_t indexBuffer = reinterpret_cast<uint64_t>(sceneView->shadowPackedIndexBuffer);
      constexpr uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &vertexBuffer, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(indexBuffer, 0, rhi::IndexFormat::uint32);
      context.cmd->drawIndexedIndirect(m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex),
                                       0,
                                       sceneView->shadowPackedMeshCount,
                                       sizeof(VkDrawIndexedIndirectCommand));
    }

    context.cmd->endRenderPass();
  }

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
      .aspect      = rhi::TextureAspect::depth,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::read,
      .oldState    = rhi::ResourceState::DepthStencilAttachment,
      .newState    = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  context.cmd->endEvent();
}

}  // namespace demo
