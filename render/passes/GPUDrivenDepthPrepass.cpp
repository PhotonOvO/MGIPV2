#include "GPUDrivenDepthPrepass.h"

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
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || context.commandBuffer == nullptr || context.executor == nullptr)
  {
    return;
  }

  context.commandBuffer->beginEvent("GPUDrivenDepthPrepass");

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView.isNull())
  {
    context.commandBuffer->endEvent();
    return;
  }
  const rhi::Extent2D extent = sceneView->sceneDepthExtent;

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = sceneView->sceneDepthView,
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
  rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  if(context.drawStream == nullptr)
  {
    context.commandBuffer->endEncoding();
    context.commandBuffer->endEvent();
    return;
  }

  if(!context.cameraAllocValid)
  {
    context.commandBuffer->endEncoding();
    context.commandBuffer->endEvent();
    return;
  }
  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

  const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
  const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);
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
  if(indirectBufferHandle != 0 && countBufferHandle != 0 && previousIndirectObjectCount > 0u && !drawTable.isNull())
  {
    const rhi::ArgumentTableHandle mdiDrawTable = m_renderer->getDepthMDIDrawArgumentTable(context.frameIndex);
    if(!mdiDrawTable.isNull())
    {
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

      // All meshes share the MeshPool vertex/index arenas; bind their stable RHI
      // handles (meshlet path uses the GPUMeshletBuffer index handle).
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
      m_renderer->recordDepthPrepassVisibilitySource(true,
                                                     previousBootstrapIndirectBufferHandle != 0,
                                                     previousIndirectObjectCount,
                                                     opaqueMaxDrawCount,
                                                     alphaMaxDrawCount);

      // Indirect args + count buffers as stable RHI handles (previous-frame ring),
      // selecting the bootstrap persistent stream or the culling output to match the
      // uint64 path above.
      const rhi::BufferHandle indirectBufferRHI =
          previousBootstrapIndirectBufferHandle != 0
              ? m_renderer->getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(context.frameIndex)
              : m_renderer->getPreviousGPUCullingIndirectBufferRHIHandle(context.frameIndex);
      const rhi::BufferHandle countBufferRHI =
          m_renderer->getPreviousGPUCullingDrawCountBufferRHIHandle(context.frameIndex);

      enc->setPipeline(m_renderer->getDepthPrepassOpaqueMDIPipelineHandle());
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

      enc->setPipeline(m_renderer->getDepthPrepassAlphaTestMDIPipelineHandle());
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

  context.commandBuffer->endEncoding();
  context.commandBuffer->endEvent();
}

}  // namespace demo
