#include "GPUDrivenGBufferPass.h"

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

GPUDrivenGBufferPass::GPUDrivenGBufferPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenGBufferPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 5> dependencies = {
      PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassGBuffer2Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenGBufferPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenGBufferPass");

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }
  const rhi::Extent2D extent = {sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};

  std::array<rhi::RenderTargetDesc, 3> colorTargets{};
  for(uint32_t i = 0; i < 3; ++i)
  {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{static_cast<uint32_t>(kPassGBuffer0Handle.index + i), kPassGBuffer0Handle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->gbufferImages[i]),
        .aspect      = rhi::TextureAspect::color,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::read,
        .dstAccess   = rhi::ResourceAccess::write,
        .oldState    = rhi::ResourceState::General,
        .newState    = rhi::ResourceState::ColorAttachment,
        .isSwapchain = false,
    });

    colorTargets[i] = {
        .texture    = {},
        .view       = rhi::TextureViewHandle::fromNative(sceneView->gbufferViews[i]),
        .state      = rhi::ResourceState::ColorAttachment,
        .loadOp     = rhi::LoadOp::clear,
        .storeOp    = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
    };
  }

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = rhi::TextureViewHandle::fromNative(sceneView->sceneDepthView),
      .state      = rhi::ResourceState::DepthStencilAttachment,
      .loadOp     = rhi::LoadOp::load,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
      .aspect      = sceneDepthAspect(sceneView->sceneDepthFormat),
      .srcStage    = rhi::PipelineStage::Compute,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::read,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

  uint64_t sortedIndirectBufferHandle = 0;
  uint64_t sortedCountBufferHandle = 0;
  uint32_t sortedOpaqueCapacity = 0;
  uint32_t sortedAlphaCapacity = 0;
  m_renderer->invalidateSortedBootstrapStateForFrame(context.frameIndex);
  if(context.drawStream != nullptr)
  {
    sortedCountBufferHandle = m_renderer->getGPUCullingDrawCountBufferOpaque(context.frameIndex);
    sortedOpaqueCapacity = static_cast<uint32_t>(m_renderer->getOpaqueDrawIndices().size());
    sortedAlphaCapacity = static_cast<uint32_t>(m_renderer->getAlphaTestDrawIndices().size());
    const uint32_t transparentCapacity = static_cast<uint32_t>(m_renderer->getTransparentDrawIndices().size());
    const uint32_t totalSortedCapacity = sortedOpaqueCapacity + sortedAlphaCapacity + transparentCapacity;
    if(sortedCountBufferHandle != 0 && totalSortedCapacity > 0u)
    {
      m_renderer->ensureGPUDrivenPersistentIndirectStream(context.frameIndex, totalSortedCapacity);
      const uint64_t persistentIndirectBufferHandle = m_renderer->getGPUDrivenPersistentIndirectStreamBuffer(context.frameIndex);
      if(persistentIndirectBufferHandle != 0)
      {
        const bool opaquePatched = sortedOpaqueCapacity == 0u
                                   || m_renderer->prepareAndDispatchVisibilityPatch(*context.cmd,
                                                                                    context.frameIndex,
                                                                                    persistentIndirectBufferHandle,
                                                                                    0x00000000u,
                                                                                    0u);
        const bool alphaPatched = sortedAlphaCapacity == 0u
                                  || m_renderer->prepareAndDispatchVisibilityPatch(*context.cmd,
                                                                                   context.frameIndex,
                                                                                   persistentIndirectBufferHandle,
                                                                                   0x40000000u,
                                                                                   sortedOpaqueCapacity);
        if(opaquePatched && alphaPatched)
        {
          sortedIndirectBufferHandle = persistentIndirectBufferHandle;
          m_renderer->publishSortedBootstrapStateForFrame(context.frameIndex, sortedOpaqueCapacity, sortedAlphaCapacity);
        }
      }
    }
  }

  const rhi::RenderPassDesc passDesc{
      .renderArea       = {{0, 0}, extent},
      .colorTargets     = colorTargets.data(),
      .colorTargetCount = static_cast<uint32_t>(colorTargets.size()),
      .depthTarget      = &depthTarget,
  };
  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  if(context.drawStream != nullptr)
  {
    MeshPool& meshPool = m_renderer->getMeshPool();
    const uint64_t indirectBufferHandle = sortedIndirectBufferHandle != 0
                                              ? sortedIndirectBufferHandle
                                              : m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
    const uint64_t countBufferHandle = sortedCountBufferHandle != 0
                                           ? sortedCountBufferHandle
                                           : m_renderer->getGPUCullingDrawCountBufferOpaque(context.frameIndex);
    const uint32_t currentIndirectObjectCount = m_renderer->getGPUCullingObjectCount(context.frameIndex);
    const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

    if(!context.cameraAllocValid)
    {
      context.cmd->endRenderPass();
      context.cmd->endEvent();
      return;
    }
    const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);

    if(indirectBufferHandle != 0 && !drawBindGroupHandle.isNull())
    {
      const bool useMdi = indirectBufferHandle != 0 && !m_renderer->getGBufferMDIDrawBindGroup(context.frameIndex).isNull();
      if(useMdi)
      {
        const VkPipelineLayout pipelineLayout =
            reinterpret_cast<VkPipelineLayout>(m_renderer->getGraphicsMDIPipelineLayout());
        const VkDescriptorSet textureSet =
            reinterpret_cast<VkDescriptorSet>(m_renderer->getGraphicsMaterialDescriptorSet());
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout,
                                shaderio::LSetTextures,
                                1,
                                &textureSet,
                                0,
                                nullptr);
        if(!cameraBindGroupHandle.isNull())
        {
          VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
              m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
          const uint32_t cameraDynamicOffset = cameraAlloc.offset;
          vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipelineLayout,
                                  shaderio::LSetScene,
                                  1,
                                  &cameraDescriptorSet,
                                  1,
                                  &cameraDynamicOffset);
        }
        const BindGroupHandle mdiDrawBindGroupHandle = m_renderer->getGBufferMDIDrawBindGroup(context.frameIndex);
        const VkDescriptorSet mdiDrawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
            m_renderer->getBindGroupDescriptorSet(mdiDrawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout,
                                shaderio::LSetDraw,
                                1,
                                &mdiDrawDescriptorSet,
                                0,
                                nullptr);

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
        const uint64_t sharedIndexHandle = representativeMesh->indexBufferHandle;
        const uint64_t vertexOffset = 0;
        context.cmd->bindVertexBuffers(0, &sharedVertexHandle, &vertexOffset, 1);
        context.cmd->bindIndexBuffer(sharedIndexHandle, 0, rhi::IndexFormat::uint32);

        TRACY_ZONE_SCOPED("GPUDrivenGBufferPass::drawLoopMDI");
        const uint64_t opaqueCommandOffset = sortedIndirectBufferHandle != 0
                                                ? 0u
                                                : static_cast<uint64_t>(currentIndirectObjectCount) * indirectCommandStride;
        const uint64_t alphaCommandOffset = sortedIndirectBufferHandle != 0
                                               ? static_cast<uint64_t>(sortedOpaqueCapacity) * indirectCommandStride
                                               : opaqueCommandOffset * 2u;
        const uint64_t opaqueCountOffset = offsetof(shaderio::GPUCullDrawCounts, opaqueCount);
        const uint64_t alphaCountOffset = offsetof(shaderio::GPUCullDrawCounts, alphaTestCount);
        const uint32_t opaqueMaxDrawCount = sortedIndirectBufferHandle != 0 ? sortedOpaqueCapacity : currentIndirectObjectCount;
        const uint32_t alphaMaxDrawCount = sortedIndirectBufferHandle != 0 ? sortedAlphaCapacity : currentIndirectObjectCount;

        const VkPipeline opaquePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getNativeGraphicsPipeline(m_renderer->getGBufferOpaqueMDIPipelineHandle()));
        rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline);
        context.cmd->drawIndexedIndirectCount(indirectBufferHandle,
                                              opaqueCommandOffset,
                                              countBufferHandle,
                                              opaqueCountOffset,
                                              opaqueMaxDrawCount,
                                              indirectCommandStride);

        const VkPipeline alphaPipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getNativeGraphicsPipeline(m_renderer->getGBufferAlphaTestMDIPipelineHandle()));
        rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alphaPipeline);
        context.cmd->drawIndexedIndirectCount(indirectBufferHandle,
                                              alphaCommandOffset,
                                              countBufferHandle,
                                              alphaCountOffset,
                                              alphaMaxDrawCount,
                                              indirectCommandStride);
      }
    }
  }

  context.cmd->endRenderPass();

  const std::array<std::pair<TextureHandle, VkImage>, 3> colorImages{{
      {kPassGBuffer0Handle, sceneView->gbufferImages[0]},
      {kPassGBuffer1Handle, sceneView->gbufferImages[1]},
      {kPassGBuffer2Handle, sceneView->gbufferImages[2]},
  }};
  for(const auto& [handle, image] : colorImages)
  {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{handle.index, handle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(image),
        .aspect      = rhi::TextureAspect::color,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::write,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::ColorAttachment,
        .newState    = rhi::ResourceState::General,
        .isSwapchain = false,
    });
  }

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
      .aspect      = sceneDepthAspect(sceneView->sceneDepthFormat),
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
