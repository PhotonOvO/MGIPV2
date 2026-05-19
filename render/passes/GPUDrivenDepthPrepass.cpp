#include "GPUDrivenDepthPrepass.h"

#include "../ClipSpaceConvention.h"
#include "../GPUDrivenRenderer.h"
#include "../MeshPool.h"
#include "../../common/TracyProfiling.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace demo {

namespace {

[[nodiscard]] rhi::TextureAspect sceneDepthAspect(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return rhi::TextureAspect::depthStencil;
    default:
      return rhi::TextureAspect::depth;
  }
}

}  // namespace

GPUDrivenDepthPrepass::GPUDrivenDepthPrepass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenDepthPrepass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenDepthPrepass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenDepthPrepass");

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }
  const VkExtent2D vkExtent = sceneView->sceneDepthExtent;
  const rhi::Extent2D extent{vkExtent.width, vkExtent.height};

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = rhi::TextureViewHandle::fromNative(sceneView->sceneDepthView),
      .state      = rhi::ResourceState::DepthStencilAttachment,
      .loadOp     = rhi::LoadOp::clear,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  const rhi::RenderPassDesc passDesc{
      .renderArea       = {{0, 0}, extent},
      .colorTargets     = nullptr,
      .colorTargetCount = 0,
      .depthTarget      = &depthTarget,
  };
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
      .aspect      = sceneDepthAspect(sceneView->sceneDepthFormat),
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });
  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  if(context.drawStream == nullptr)
  {
    context.cmd->endRenderPass();
    context.cmd->endEvent();
    return;
  }

  const VkPipelineLayout pipelineLayout =
      reinterpret_cast<VkPipelineLayout>(m_renderer->getGraphicsScenePipelineLayout());
  const VkDescriptorSet textureSet =
      reinterpret_cast<VkDescriptorSet>(m_renderer->getGraphicsMaterialDescriptorSet());
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                          shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

  if(!context.cameraAllocValid)
  {
    context.cmd->endRenderPass();
    context.cmd->endEvent();
    return;
  }
  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  if(!cameraBindGroupHandle.isNull())
  {
    VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
    const uint32_t cameraDynamicOffset = cameraAlloc.offset;
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
  }

  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();

  uint32_t previousOpaqueCapacity = 0u;
  uint32_t previousAlphaCapacity = 0u;
  const bool previousBootstrapValid =
      m_renderer->getPreviousSortedBootstrapState(context.frameIndex, previousOpaqueCapacity, previousAlphaCapacity);
  const uint64_t previousBootstrapIndirectBufferHandle = previousBootstrapValid
                                                             ? m_renderer->getPreviousGPUDrivenPersistentIndirectStreamBuffer(context.frameIndex)
                                                             : 0;
  const uint64_t indirectBufferHandle = previousBootstrapIndirectBufferHandle != 0
                                            ? previousBootstrapIndirectBufferHandle
                                            : m_renderer->getPreviousGPUCullingIndirectBufferOpaque(context.frameIndex);
  const uint64_t countBufferHandle = m_renderer->getPreviousGPUCullingDrawCountBufferOpaque(context.frameIndex);
  const uint32_t previousIndirectObjectCount = m_renderer->getPreviousGPUCullingObjectCount(context.frameIndex);
  const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();
  if(indirectBufferHandle != 0 && countBufferHandle != 0 && previousIndirectObjectCount > 0u && !drawBindGroupHandle.isNull())
  {
    const BindGroupHandle mdiDrawBindGroupHandle = m_renderer->getDepthMDIDrawBindGroup(context.frameIndex);
    if(!mdiDrawBindGroupHandle.isNull())
    {
      const VkPipelineLayout mdiPipelineLayout =
          reinterpret_cast<VkPipelineLayout>(m_renderer->getGraphicsMDIPipelineLayout());
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                              shaderio::LSetTextures, 1, &textureSet, 0, nullptr);
      if(!cameraBindGroupHandle.isNull())
      {
        VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
            m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
        const uint32_t cameraDynamicOffset = cameraAlloc.offset;
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                                shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
      }
      const VkDescriptorSet mdiDrawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(mdiDrawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                              shaderio::LSetDraw, 1, &mdiDrawDescriptorSet, 0, nullptr);

      const auto pickRepresentativeMesh = [&]() -> const MeshRecord* {
        for(uint32_t drawIndex : m_renderer->getOpaqueDrawIndices())
        {
          MeshHandle meshHandle = kNullMeshHandle;
          if(m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
          {
            if(const MeshRecord* mesh = meshPool.tryGet(meshHandle))
            {
              return mesh;
            }
          }
        }
        for(uint32_t drawIndex : m_renderer->getAlphaTestDrawIndices())
        {
          MeshHandle meshHandle = kNullMeshHandle;
          if(m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
          {
            if(const MeshRecord* mesh = meshPool.tryGet(meshHandle))
            {
              return mesh;
            }
          }
        }
        return nullptr;
      };

      const MeshRecord* representativeMesh = pickRepresentativeMesh();
      if(representativeMesh == nullptr)
      {
        context.cmd->endRenderPass();
        context.cmd->endEvent();
        return;
      }

      const uint64_t sharedVertexHandle = representativeMesh->vertexBufferHandle;
      const uint64_t sharedIndexHandle = m_renderer->isMeshletRenderingActive()
                                             ? m_renderer->getMeshletIndexBufferHandle()
                                             : representativeMesh->indexBufferHandle;
      if(sharedIndexHandle == 0)
      {
        context.cmd->endRenderPass();
        context.cmd->endEvent();
        return;
      }
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &sharedVertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(sharedIndexHandle, 0, rhi::IndexFormat::uint32);

      TRACY_ZONE_SCOPED("GPUDrivenDepthPrepass::drawLoopMDI");
      const uint64_t opaqueCommandOffset = previousBootstrapIndirectBufferHandle != 0
                                               ? 0u
                                               : static_cast<uint64_t>(previousIndirectObjectCount) * indirectCommandStride;
      const uint64_t alphaCommandOffset = previousBootstrapIndirectBufferHandle != 0
                                              ? static_cast<uint64_t>(previousOpaqueCapacity) * indirectCommandStride
                                              : opaqueCommandOffset * 2u;
      const uint64_t opaqueCountOffset = offsetof(shaderio::GPUCullDrawCounts, opaqueCount);
      const uint64_t alphaCountOffset = offsetof(shaderio::GPUCullDrawCounts, alphaTestCount);
      const uint32_t opaqueMaxDrawCount =
          previousBootstrapIndirectBufferHandle != 0 ? previousOpaqueCapacity : previousIndirectObjectCount;
      const uint32_t alphaMaxDrawCount =
          previousBootstrapIndirectBufferHandle != 0 ? previousAlphaCapacity : previousIndirectObjectCount;

      const VkPipeline opaquePipeline = reinterpret_cast<VkPipeline>(
          m_renderer->getNativeGraphicsPipeline(m_renderer->getDepthPrepassOpaqueMDIPipelineHandle()));
      rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline);
      context.cmd->drawIndexedIndirectCount(indirectBufferHandle,
                                            opaqueCommandOffset,
                                            countBufferHandle,
                                            opaqueCountOffset,
                                            opaqueMaxDrawCount,
                                            indirectCommandStride);

      const VkPipeline alphaPipeline = reinterpret_cast<VkPipeline>(
          m_renderer->getNativeGraphicsPipeline(m_renderer->getDepthPrepassAlphaTestMDIPipelineHandle()));
      rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alphaPipeline);
      context.cmd->drawIndexedIndirectCount(indirectBufferHandle,
                                            alphaCommandOffset,
                                            countBufferHandle,
                                            alphaCountOffset,
                                            alphaMaxDrawCount,
                                            indirectCommandStride);
    }
  }

  context.cmd->endRenderPass();
  context.cmd->endEvent();
}

}  // namespace demo
