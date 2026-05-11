#include "GPUDrivenGBufferPass.h"

#include "../ClipSpaceConvention.h"
#include "../GPUDrivenRenderer.h"
#include "../MeshPool.h"
#include "../../common/TracyProfiling.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>
#include <vector>

namespace demo {

namespace {

constexpr uint32_t kDrawUniformsStride = 256;

struct PendingDraw
{
  size_t                 meshIndex;
  uint32_t               drawIndex;
  const MeshRecord*      mesh;
  shaderio::DrawUniforms uniforms;
  PipelineHandle         pipeline;
};

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
    const uint64_t indirectBufferHandle = m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
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

    if(!context.globalBindlessGroup.isNull())
    {
      const VkPipelineLayout pipelineLayout =
          reinterpret_cast<VkPipelineLayout>(m_renderer->getGraphicsScenePipelineLayout());
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
    }

    if(!cameraBindGroupHandle.isNull())
    {
      const VkPipelineLayout pipelineLayout =
          reinterpret_cast<VkPipelineLayout>(m_renderer->getGraphicsScenePipelineLayout());
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

    std::vector<PendingDraw> pendingDraws;
    {
      TRACY_ZONE_SCOPED("GPUDrivenGBufferPass::collectMeshes");
      const PipelineHandle opaquePipeline = m_renderer->getGBufferOpaquePipelineHandle();
      const PipelineHandle alphaTestPipeline = m_renderer->getGBufferAlphaTestPipelineHandle();
      const auto opaqueDrawIndices = m_renderer->getOpaqueDrawIndices();
      const auto alphaTestDrawIndices = m_renderer->getAlphaTestDrawIndices();
      pendingDraws.reserve(opaqueDrawIndices.size() + alphaTestDrawIndices.size());
      const auto appendDraws = [&](std::span<const uint32_t> drawIndices, PipelineHandle pipeline, int32_t alphaMode) {
        for(const uint32_t drawIndex : drawIndices)
        {
          MeshHandle meshHandle = kNullMeshHandle;
          if(!m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
          {
            continue;
          }

          const MeshRecord* mesh = meshPool.tryGet(meshHandle);
          if(mesh == nullptr || mesh->alphaMode == shaderio::LAlphaBlend)
          {
            continue;
          }

          shaderio::DrawUniforms drawData{};
          drawData.modelMatrix = mesh->transform;
          drawData.alphaMode = alphaMode;
          drawData.alphaCutoff = mesh->alphaCutoff;
          drawData.baseColorFactor = mesh->baseColorFactor;
          drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
          drawData.normalTextureIndex = mesh->normalTextureIndex;
          drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
          drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
          drawData.metallicFactor = mesh->metallicFactor;
          drawData.roughnessFactor = mesh->roughnessFactor;
          drawData.normalScale = mesh->normalScale;

          pendingDraws.push_back({0u, drawIndex, mesh, drawData, pipeline});
        }
      };
      if(!opaqueDrawIndices.empty() || !alphaTestDrawIndices.empty())
      {
        appendDraws(opaqueDrawIndices, opaquePipeline, shaderio::LAlphaOpaque);
        appendDraws(alphaTestDrawIndices, alphaTestPipeline, shaderio::LAlphaMask);
      }

    }

    if(!pendingDraws.empty() && !drawBindGroupHandle.isNull())
    {
      const bool useMdi = indirectBufferHandle != 0 && !m_renderer->getGBufferMDIDrawBindGroup(context.frameIndex).isNull();
      if(useMdi)
      {
        {
          TRACY_ZONE_SCOPED("GPUDrivenGBufferPass::batchUpload");
          std::vector<shaderio::DrawUniforms> mdiDrawData(m_renderer->getPersistentObjectCount());
          for(const PendingDraw& draw : pendingDraws)
          {
            if(draw.drawIndex < mdiDrawData.size())
            {
              mdiDrawData[draw.drawIndex] = draw.uniforms;
            }
          }
          m_renderer->uploadGBufferMDIDrawData(context.frameIndex, mdiDrawData);
        }

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

        const uint64_t sharedVertexHandle = pendingDraws.front().mesh->vertexBufferHandle;
        const uint64_t sharedIndexHandle = pendingDraws.front().mesh->indexBufferHandle;
        const uint64_t vertexOffset = 0;
        context.cmd->bindVertexBuffers(0, &sharedVertexHandle, &vertexOffset, 1);
        context.cmd->bindIndexBuffer(sharedIndexHandle, 0, rhi::IndexFormat::uint32);

        TRACY_ZONE_SCOPED("GPUDrivenGBufferPass::drawLoopMDI");
        size_t runBegin = 0;
        while(runBegin < pendingDraws.size())
        {
          const PipelineHandle mdiPipeline =
              pendingDraws[runBegin].pipeline == m_renderer->getGBufferAlphaTestPipelineHandle()
                  ? m_renderer->getGBufferAlphaTestMDIPipelineHandle()
                  : m_renderer->getGBufferOpaqueMDIPipelineHandle();
          const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
              m_renderer->getNativeGraphicsPipeline(mdiPipeline));
          rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

          size_t runEnd = runBegin + 1;
          while(runEnd < pendingDraws.size()
                && pendingDraws[runEnd].pipeline == pendingDraws[runBegin].pipeline
                && pendingDraws[runEnd].drawIndex == pendingDraws[runEnd - 1].drawIndex + 1
                && pendingDraws[runEnd].drawIndex < currentIndirectObjectCount)
          {
            ++runEnd;
          }

          if(pendingDraws[runBegin].drawIndex < currentIndirectObjectCount)
          {
            context.cmd->drawIndexedIndirect(indirectBufferHandle,
                                             static_cast<uint64_t>(pendingDraws[runBegin].drawIndex) * indirectCommandStride,
                                             static_cast<uint32_t>(runEnd - runBegin),
                                             indirectCommandStride);
          }
          runBegin = runEnd;
        }
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
