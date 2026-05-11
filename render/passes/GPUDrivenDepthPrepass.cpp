#include "GPUDrivenDepthPrepass.h"

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
  size_t                  meshIndex;
  uint32_t                drawIndex;
  const MeshRecord*       mesh;
  shaderio::DrawUniforms  uniforms;
  PipelineHandle          pipeline;
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

  const uint64_t indirectBufferHandle = m_renderer->getPreviousGPUCullingIndirectBufferOpaque(context.frameIndex);
  const uint32_t previousIndirectObjectCount = m_renderer->getPreviousGPUCullingObjectCount(context.frameIndex);
  const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

  std::vector<PendingDraw> pendingDraws;
    {
      TRACY_ZONE_SCOPED("GPUDrivenDepthPrepass::collectMeshes");
      const PipelineHandle opaquePipeline = m_renderer->getDepthPrepassOpaquePipelineHandle();
      const PipelineHandle alphaTestPipeline = m_renderer->getDepthPrepassAlphaTestPipelineHandle();
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
          drawData.baseColorFactor = mesh->baseColorFactor;
          drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
          drawData.normalTextureIndex = mesh->normalTextureIndex;
          drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
          drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
          drawData.metallicFactor = mesh->metallicFactor;
          drawData.roughnessFactor = mesh->roughnessFactor;
          drawData.normalScale = mesh->normalScale;
          drawData.alphaMode = alphaMode;
          drawData.alphaCutoff = mesh->alphaCutoff;

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
    const BindGroupHandle mdiDrawBindGroupHandle = m_renderer->getDepthMDIDrawBindGroup(context.frameIndex);
    const bool useMdi = !mdiDrawBindGroupHandle.isNull();
    const uint32_t batchSize = static_cast<uint32_t>(pendingDraws.size()) * kDrawUniformsStride;
    const TransientAllocator::Allocation batchAlloc =
        useMdi ? TransientAllocator::Allocation{} : context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

    {
      TRACY_ZONE_SCOPED("GPUDrivenDepthPrepass::batchUpload");
      if(useMdi)
      {
        std::vector<shaderio::DrawUniforms> mdiDrawData(m_renderer->getPersistentObjectCount());
        std::vector<shaderio::GPUCullIndirectCommand> bootstrapCommands(m_renderer->getPersistentObjectCount());
        for(const PendingDraw& draw : pendingDraws)
        {
          if(draw.drawIndex < mdiDrawData.size())
          {
            mdiDrawData[draw.drawIndex] = draw.uniforms;
            bootstrapCommands[draw.drawIndex] = shaderio::GPUCullIndirectCommand{
                .indexCount = draw.mesh->indexCount,
                .instanceCount = 1u,
                .firstIndex = draw.mesh->firstIndex,
                .vertexOffset = draw.mesh->vertexOffset,
                .firstInstance = draw.drawIndex,
            };
          }
        }
        m_renderer->uploadDepthMDIDrawData(context.frameIndex, mdiDrawData);
        m_renderer->uploadGPUDrivenBootstrapCommands(context.frameIndex, bootstrapCommands);
      }
      else
      {
        for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
        {
          std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + static_cast<uint32_t>(slot) * kDrawUniformsStride;
          std::memcpy(dst, &pendingDraws[slot].uniforms, sizeof(shaderio::DrawUniforms));
        }
        context.transientAllocator->flushAllocation(batchAlloc, batchSize);
      }
    }

    if(useMdi)
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

      const uint64_t sharedVertexHandle = pendingDraws.front().mesh->vertexBufferHandle;
      const uint64_t sharedIndexHandle = pendingDraws.front().mesh->indexBufferHandle;
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &sharedVertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(sharedIndexHandle, 0, rhi::IndexFormat::uint32);

      TRACY_ZONE_SCOPED("GPUDrivenDepthPrepass::drawLoopMDI");
      const uint64_t bootstrapIndirectBufferHandle = m_renderer->getGPUDrivenBootstrapIndirectBuffer(context.frameIndex);
      size_t runBegin = 0;
      while(runBegin < pendingDraws.size())
      {
        const PipelineHandle mdiPipeline =
            pendingDraws[runBegin].pipeline == m_renderer->getDepthPrepassAlphaTestPipelineHandle()
                ? m_renderer->getDepthPrepassAlphaTestMDIPipelineHandle()
                : m_renderer->getDepthPrepassOpaqueMDIPipelineHandle();
        const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getNativeGraphicsPipeline(mdiPipeline));
        rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

        size_t runEnd = runBegin + 1;
        const bool usePreviousIndirect = pendingDraws[runBegin].drawIndex < previousIndirectObjectCount;
        while(runEnd < pendingDraws.size()
              && pendingDraws[runEnd].pipeline == pendingDraws[runBegin].pipeline
              && pendingDraws[runEnd].drawIndex == pendingDraws[runEnd - 1].drawIndex + 1
              && ((pendingDraws[runEnd].drawIndex < previousIndirectObjectCount) == usePreviousIndirect))
        {
          ++runEnd;
        }

        const uint32_t runCount = static_cast<uint32_t>(runEnd - runBegin);
        const uint64_t sourceIndirectBuffer = usePreviousIndirect ? indirectBufferHandle : bootstrapIndirectBufferHandle;
        if(sourceIndirectBuffer == 0)
        {
          runBegin = runEnd;
          continue;
        }

        context.cmd->drawIndexedIndirect(sourceIndirectBuffer,
                                         static_cast<uint64_t>(pendingDraws[runBegin].drawIndex) * indirectCommandStride,
                                         runCount,
                                         indirectCommandStride);
        runBegin = runEnd;
      }
    }
  }

  context.cmd->endRenderPass();
  context.cmd->endEvent();
}

}  // namespace demo
