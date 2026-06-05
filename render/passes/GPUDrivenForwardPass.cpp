#include "GPUDrivenForwardPass.h"

#include "../GPUDrivenRenderer.h"
#include "../RenderDevice.h"
#include "../MeshPool.h"
#include "../PassExecutor.h"
#include "../ClipSpaceConvention.h"
#include "../../common/TracyProfiling.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
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

GPUDrivenForwardPass::GPUDrivenForwardPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenForwardPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::readWrite, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenForwardPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || context.cmdBuffer == nullptr || context.executor == nullptr)
  {
    return;
  }

  context.cmdBuffer->beginEvent("GPUDrivenForwardPass");

  const auto transition = [&](uint64_t nativeImage, rhi::TextureAspect aspect, rhi::ResourceState before,
                              rhi::ResourceState after) {
    const rhi::TextureBarrier barrier{
        .texture = context.executor->resolveBarrierTexture(nativeImage),
        .before  = before,
        .after   = after,
        .range   = {.aspect = aspect, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0, .layerCount = ~0u},
    };
    context.cmdBuffer->resourceBarrier(&barrier, 1, nullptr, 0);
  };

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView.isNull() || sceneView->sceneColorHdrView.isNull())
  {
    context.cmdBuffer->endEvent();
    return;
  }
  const auto restoreColorForSampling = [&]() {
    transition(reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage), rhi::TextureAspect::color,
               rhi::ResourceState::ColorAttachment, rhi::ResourceState::General);
  };
  bool depthInAttachmentLayout = false;
  const auto restoreDepthForSampling = [&]() {
    if(!depthInAttachmentLayout)
    {
      return;
    }
    transition(reinterpret_cast<uint64_t>(sceneView->sceneDepthImage), sceneDepthAspect(sceneView->sceneDepthFormat),
               rhi::ResourceState::DepthStencilAttachment, rhi::ResourceState::General);
    depthInAttachmentLayout = false;
  };

  const rhi::TextureViewHandle outputImageView = sceneView->sceneColorHdrView;
  const VkExtent2D vkExtent = sceneView->sceneDepthExtent;
  const rhi::Extent2D renderExtent = {vkExtent.width, vkExtent.height};
  if(outputImageView.isNull() || renderExtent.width == 0 || renderExtent.height == 0)
  {
    context.cmdBuffer->endEvent();
    return;
  }

  MeshPool& meshPool = m_renderer->getMeshPool();
  const uint32_t objectCount = m_renderer->getGPUCullingObjectCount(context.frameIndex);
  const uint64_t indirectBufferHandle = m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
  const uint64_t countBufferHandle = m_renderer->getGPUCullingDrawCountBufferOpaque(context.frameIndex);
  if(objectCount == 0 || indirectBufferHandle == 0 || countBufferHandle == 0)
  {
    context.cmdBuffer->endEvent();
    return;
  }

  transition(reinterpret_cast<uint64_t>(sceneView->sceneColorHdrImage), rhi::TextureAspect::color,
             rhi::ResourceState::General, rhi::ResourceState::ColorAttachment);

  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = outputImageView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
  };
  const rhi::DepthTargetDesc depthTarget{
      .texture = {},
      .view = sceneView->sceneDepthView,
      .state = rhi::ResourceState::DepthStencilAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  transition(reinterpret_cast<uint64_t>(sceneView->sceneDepthImage), sceneDepthAspect(sceneView->sceneDepthFormat),
             rhi::ResourceState::General, rhi::ResourceState::DepthStencilAttachment);
  depthInAttachmentLayout = true;

  const PipelineHandle forwardPipeline = m_renderer->getForwardMDIPipelineHandle();
  if(forwardPipeline.isNull())
  {
    restoreColorForSampling();
    restoreDepthForSampling();
    context.cmdBuffer->endEvent();
    return;
  }

  if(!context.cameraAllocValid)
  {
    restoreColorForSampling();
    restoreDepthForSampling();
    context.cmdBuffer->endEvent();
    return;
  }

  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);

  const BindGroupHandle drawBindGroupHandle = m_renderer->getMDIDrawBindGroup(context.frameIndex);
  if(drawBindGroupHandle.isNull())
  {
    restoreColorForSampling();
    restoreDepthForSampling();
    context.cmdBuffer->endEvent();
    return;
  }

  const uint32_t transparentCapacity = static_cast<uint32_t>(m_renderer->getTransparentDrawIndices().size());
  if(transparentCapacity == 0u)
  {
    restoreColorForSampling();
    restoreDepthForSampling();
    context.cmdBuffer->endEvent();
    return;
  }

  const uint32_t opaqueCapacity = static_cast<uint32_t>(m_renderer->getOpaqueDrawIndices().size());
  const uint32_t alphaCapacity = static_cast<uint32_t>(m_renderer->getAlphaTestDrawIndices().size());
  const uint32_t totalPersistentCapacity = opaqueCapacity + alphaCapacity + transparentCapacity;
  m_renderer->ensureGPUDrivenPersistentIndirectStream(context.frameIndex, totalPersistentCapacity);
  const uint64_t forwardIndirectBufferHandle = m_renderer->getGPUDrivenPersistentIndirectStreamBuffer(context.frameIndex);
  const bool transparentPatched =
      forwardIndirectBufferHandle != 0
      && m_renderer->prepareAndDispatchVisibilityPatch(*context.cmdBuffer,
                                                       context.frameIndex,
                                                       forwardIndirectBufferHandle,
                                                       0x80000000u,
                                                       opaqueCapacity + alphaCapacity);
  m_renderer->recordForwardVisibilityPatch(transparentPatched, transparentCapacity, totalPersistentCapacity);
  if(!transparentPatched)
  {
    restoreColorForSampling();
    restoreDepthForSampling();
    context.cmdBuffer->endEvent();
    return;
  }

  const rhi::RenderPassDesc passDesc{
      .renderArea = {{0, 0}, renderExtent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = &depthTarget,
  };
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(passDesc);
  enc->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, renderExtent});

  // Pipeline + descriptor binds reordered after beginRenderPass (RenderEncoder owns them).
  enc->setPipeline(forwardPipeline);
  const BindGroupHandle materialBindGroup = m_renderer->getGraphicsMaterialBindGroup();
  enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                        rhi::ArgumentTableHandle{materialBindGroup.index, materialBindGroup.generation});  // bridge (Wave 8)
  if(!cameraBindGroupHandle.isNull())
  {
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, cameraAlloc.offset, 0);
    enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, 0, 0);
    enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                          rhi::ArgumentTableHandle{cameraBindGroupHandle.index, cameraBindGroupHandle.generation});  // bridge
  }
  enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw,
                        rhi::ArgumentTableHandle{drawBindGroupHandle.index, drawBindGroupHandle.generation});  // bridge

  const auto transparentDrawIndices = m_renderer->getTransparentDrawIndices();
  const MeshRecord* representativeMesh = nullptr;
  for(uint32_t drawIndex : transparentDrawIndices)
  {
    MeshHandle meshHandle = kNullMeshHandle;
    if(m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
    {
      representativeMesh = meshPool.tryGet(meshHandle);
      if(representativeMesh != nullptr)
      {
        break;
      }
    }
  }

  if(representativeMesh != nullptr)
  {
    const rhi::BufferHandle vertexBufferRHI = meshPool.getSharedVertexBufferRHIHandle();
    const rhi::BufferHandle indexBufferRHI  = m_renderer->isMeshletRenderingActive()
                                                  ? m_renderer->getMeshletIndexBufferRHIHandle()
                                                  : meshPool.getSharedIndexBufferRHIHandle();
    if(vertexBufferRHI.isNull() || indexBufferRHI.isNull())
    {
      context.cmdBuffer->endEncoding();
      restoreColorForSampling();
      restoreDepthForSampling();
      context.cmdBuffer->endEvent();
      return;
    }
    const uint64_t vertexOffset = 0;
    const uint64_t transparentCommandOffset =
        static_cast<uint64_t>(opaqueCapacity + alphaCapacity) * m_renderer->getGPUCullingIndirectCommandStride();
    enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
    enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);

    // Transparent pass uses the current-frame persistent indirect stream + culling counts.
    enc->drawIndexedIndirectCount(rhi::DrawIndirectCountDesc{
        .argsBuffer        = m_renderer->getGPUDrivenPersistentIndirectStreamBufferRHIHandle(context.frameIndex),
        .argsOffset        = transparentCommandOffset,
        .countBuffer       = m_renderer->getGPUCullingDrawCountBufferRHIHandle(context.frameIndex),
        .countBufferOffset = offsetof(shaderio::GPUCullDrawCounts, transparentCount),
        .maxDrawCount      = transparentCapacity,
        .stride            = m_renderer->getGPUCullingIndirectCommandStride(),
    });
  }

  context.cmdBuffer->endEncoding();
  restoreColorForSampling();
  restoreDepthForSampling();
  context.cmdBuffer->endEvent();
}

}  // namespace demo
