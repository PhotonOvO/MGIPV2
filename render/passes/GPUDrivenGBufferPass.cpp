#include "GPUDrivenGBufferPass.h"

#include "../ArgumentTables.h"

#include "../ClipSpaceConvention.h"
#include "../GPUDrivenRenderer.h"
#include "../DrawStreamRecorder.h"
#include "../MeshPool.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace demo {

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
                                      rhi::ResourceState::DepthStencilReadOnly),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenGBufferPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || context.commandBuffer == nullptr || context.executor == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenGBufferPass");

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView.isNull())
  {
    context.commandBuffer->endEvent();
    return;
  }
  const rhi::Extent2D extent = {sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};

  std::array<rhi::RenderTargetDesc, kPackedGBufferTargetCount> colorTargets{};
  for(uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
  {
    colorTargets[i] = {
        .texture    = {},
        .view       = sceneView->gbufferViews[i],
        .state      = rhi::ResourceState::ColorAttachment,
        .loadOp     = rhi::LoadOp::clear,
        .storeOp    = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
    };
  }

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = sceneView->sceneDepthView,
      .state      = rhi::ResourceState::DepthStencilReadOnly,
      .loadOp     = rhi::LoadOp::load,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

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
                                   || m_renderer->prepareAndDispatchVisibilityPatch(*context.commandBuffer,
                                                                                    context.frameIndex,
                                                                                    persistentIndirectBufferHandle,
                                                                                    0x00000000u,
                                                                                    0u);
        const bool alphaPatched = sortedAlphaCapacity == 0u
                                  || m_renderer->prepareAndDispatchVisibilityPatch(*context.commandBuffer,
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
    m_renderer->recordGBufferVisibilityPatch(sortedIndirectBufferHandle != 0,
                                             sortedOpaqueCapacity,
                                             sortedAlphaCapacity);
  }

  const rhi::RenderPassDesc passDesc{
      .renderArea       = {{0, 0}, extent},
      .colorTargets     = colorTargets.data(),
      .colorTargetCount = static_cast<uint32_t>(colorTargets.size()),
      .depthTarget      = &depthTarget,
  };
  rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

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
      context.commandBuffer->endEncoding();
      context.commandBuffer->endEvent();
      return;
    }
    const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

    const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
    const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);

    if(indirectBufferHandle != 0 && !drawTable.isNull())
    {
      const bool useMdi = indirectBufferHandle != 0 && !m_renderer->getGBufferMDIDrawArgumentTable(context.frameIndex).isNull();
      if(useMdi)
      {
        const rhi::ArgumentTableHandle mdiDrawTable = m_renderer->getGBufferMDIDrawArgumentTable(context.frameIndex);

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
          context.commandBuffer->endEncoding();
          context.commandBuffer->endEvent();
          return;
        }

        const rhi::BufferHandle vertexBufferRHI = meshPool.getSharedVertexBufferRHIHandle();
        const rhi::BufferHandle indexBufferRHI  = m_renderer->isMeshletRenderingActive()
                                                      ? m_renderer->getMeshletIndexBufferRHIHandle()
                                                      : meshPool.getSharedIndexBufferRHIHandle();
        if(vertexBufferRHI.isNull() || indexBufferRHI.isNull())
        {
          context.commandBuffer->endEncoding();
          context.commandBuffer->endEvent();
          return;
        }
        const uint64_t vertexOffset = 0;
        enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
        enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);

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

        // args = sorted persistent stream (when bootstrapped) else culling output; count is the culling draw-count buffer.
        const rhi::BufferHandle indirectBufferRHI =
            sortedIndirectBufferHandle != 0
                ? m_renderer->getGPUDrivenPersistentIndirectStreamBufferRHIHandle(context.frameIndex)
                : m_renderer->getGPUCullingIndirectBufferRHIHandle(context.frameIndex);
        const rhi::BufferHandle countBufferRHI = m_renderer->getGPUCullingDrawCountBufferRHIHandle(context.frameIndex);

        enc->setPipeline(m_renderer->getGBufferOpaqueMDIPipelineHandle());
        const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
        enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);
        if(!cameraTable.isNull())
        {
          enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset, 0);
          enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
          enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
        }
        enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, mdiDrawTable);
        DrawStreamRecorder::recordIndexedIndirectCount(*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
            .argsBuffer        = indirectBufferRHI,
            .argsOffset        = opaqueCommandOffset,
            .countBuffer       = countBufferRHI,
            .countBufferOffset = opaqueCountOffset,
            .maxDrawCount      = opaqueMaxDrawCount,
            .stride            = indirectCommandStride,
        });

        enc->setPipeline(m_renderer->getGBufferAlphaTestMDIPipelineHandle());
        DrawStreamRecorder::recordIndexedIndirectCount(*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
            .argsBuffer        = indirectBufferRHI,
            .argsOffset        = alphaCommandOffset,
            .countBuffer       = countBufferRHI,
            .countBufferOffset = alphaCountOffset,
            .maxDrawCount      = alphaMaxDrawCount,
            .stride            = indirectCommandStride,
        });
      }
    }
  }

  context.commandBuffer->endEncoding();

  context.commandBuffer->endEvent();
}

}  // namespace demo
