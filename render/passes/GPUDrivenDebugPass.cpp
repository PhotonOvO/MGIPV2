#include "GPUDrivenDebugPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>
#include <vector>

namespace demo {

namespace {
constexpr uint32_t kDebugCullSegmentCount = 24u;
}  // namespace

GPUDrivenDebugPass::GPUDrivenDebugPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenDebugPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenDebugPass::execute(const PassContext& context) const
{
  if(m_renderer != nullptr && context.cmd != nullptr && context.params != nullptr && context.transientAllocator != nullptr)
  {
    // renderDebugOverlay(context);
  }
}

void GPUDrivenDebugPass::renderDebugOverlay(const PassContext& context) const
{
  if(m_renderer == nullptr)
  {
    return;
  }

  const uint32_t safeObjectCount = m_renderer->getSafePersistentObjectCount();
  if(context.params == nullptr || context.transientAllocator == nullptr || !context.params->debugOptions.enabled)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr)
  {
    return;
  }

  const std::vector<shaderio::DebugLineVertex>& debugVertices = m_renderer->getDebugLineVertices();
  const bool hasLineDebug = !debugVertices.empty();
  const bool hasGPUCullingDebug =
      context.params->debugOptions.showGPUCullingOverlay && safeObjectCount > 0u
      && !m_renderer->getGPUCullingDebugPipelineHandle().isNull()
      && m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex) != 0
      && m_renderer->getGPUCullingResultBufferAddress(context.frameIndex) != 0;
  if(!hasLineDebug && !hasGPUCullingDebug)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenDebug");

  const rhi::Extent2D extent{sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};
  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(sceneView->outputView),
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::write,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  context.cmd->setViewport(
      rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle debugPipeline = m_renderer->getDebugPipelineHandle();
  const PipelineHandle gpuCullingDebugPipeline = m_renderer->getGPUCullingDebugPipelineHandle();
  if((hasLineDebug && debugPipeline.isNull()) || (hasGPUCullingDebug && gpuCullingDebugPipeline.isNull()))
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::write,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::ColorAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  if(!context.cameraAllocValid)
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::write,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::ColorAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  const uint32_t sceneDynamicOffsets[] = {cameraAlloc.offset, 0u};

  if(hasLineDebug)
  {
    context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, debugPipeline);
    if(!cameraBindGroupHandle.isNull())
    {
      context.cmd->bindBindGroup(shaderio::LSetScene, cameraBindGroupHandle, sceneDynamicOffsets, 2);
    }

    const uint32_t vertexDataSize = static_cast<uint32_t>(debugVertices.size() * sizeof(shaderio::DebugLineVertex));
    const TransientAllocator::Allocation vertexAlloc =
        context.transientAllocator->allocate(vertexDataSize, alignof(shaderio::DebugLineVertex));
    std::memcpy(vertexAlloc.cpuPtr, debugVertices.data(), vertexDataSize);
    context.transientAllocator->flushAllocation(vertexAlloc, vertexDataSize);

    const uint64_t vertexBuffer = context.transientAllocator->getBufferOpaque();
    const uint64_t vertexOffset = vertexAlloc.offset;
    context.cmd->bindVertexBuffers(0, &vertexBuffer, &vertexOffset, 1);
    context.cmd->draw(static_cast<uint32_t>(debugVertices.size()), 1, 0, 0);
  }

  if(hasGPUCullingDebug)
  {
    context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, gpuCullingDebugPipeline);
    if(!cameraBindGroupHandle.isNull())
    {
      context.cmd->bindBindGroup(shaderio::LSetScene, cameraBindGroupHandle, sceneDynamicOffsets, 2);
    }

    const shaderio::PushConstantGPUCullDebug pushValues{
        .objectBufferAddress = m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex),
        .resultBufferAddress = m_renderer->getGPUCullingResultBufferAddress(context.frameIndex),
        .objectCount = safeObjectCount,
        .segmentCount = kDebugCullSegmentCount,
        ._padding0 = 0u,
        ._padding1 = 0u,
    };
    context.cmd->pushConstants(rhi::ShaderStage::vertex, 0, sizeof(pushValues), &pushValues);
    context.cmd->draw(pushValues.segmentCount * 2u * 3u, pushValues.objectCount, 0, 0);
  }

  context.cmd->endRenderPass();
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
      .aspect = rhi::TextureAspect::color,
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::write,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::ColorAttachment,
      .newState = rhi::ResourceState::General,
      .isSwapchain = false,
  });
  context.cmd->endEvent();
}

}  // namespace demo
