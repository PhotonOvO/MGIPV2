#include "DebugPass.h"
#include "../Renderer.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <cstring>

namespace demo {

DebugPass::DebugPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> DebugPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassOutputHandle,
                                      ResourceAccess::write,
                                      rhi::ShaderStage::fragment,
                                      rhi::ResourceState::ColorAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void DebugPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || !context.params->debugOptions.enabled)
  {
    return;
  }

  const std::vector<shaderio::DebugLineVertex>& debugVertices = m_renderer->getDebugLineVertices();
  const bool hasLineDebug = !debugVertices.empty();
  const uint32_t gpuCullObjectCount =
      context.params->gltfModel != nullptr ? static_cast<uint32_t>(context.params->gltfModel->meshes.size()) : 0u;
  const bool hasGPUCullingDebug =
      context.params->debugOptions.showGPUCullingOverlay && gpuCullObjectCount > 0
      && !m_renderer->getGPUCullingDebugPipelineHandle().isNull()
      && m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex) != 0
      && m_renderer->getGPUCullingResultBufferAddress(context.frameIndex) != 0;
  if(!hasLineDebug && !hasGPUCullingDebug)
  {
    return;
  }

  context.cmd->beginEvent("DebugPass");

  const VkExtent2D outputExtent = m_renderer->getSceneResources().getSize();
  const rhi::Extent2D extent{outputExtent.width, outputExtent.height};

  rhi::RenderTargetDesc colorTarget{
      .texture   = {},
      .view      = rhi::TextureViewHandle::fromNative(m_renderer->getOutputTextureView()),
      .state     = rhi::ResourceState::ColorAttachment,
      .loadOp    = rhi::LoadOp::load,
      .storeOp   = rhi::StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };

  context.cmd->beginRenderPass(rhi::RenderPassDesc{
      .renderArea       = {{0, 0}, extent},
      .colorTargets     = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget      = nullptr,
  });
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle debugPipeline = m_renderer->getDebugPipelineHandle();
  const PipelineHandle gpuCullingDebugPipeline = m_renderer->getGPUCullingDebugPipelineHandle();
  if((hasLineDebug && debugPipeline.isNull()) || (hasGPUCullingDebug && gpuCullingDebugPipeline.isNull()))
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(m_renderer->getSceneResources().getOutputTextureImage()),
        .aspect      = rhi::TextureAspect::color,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::write,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::ColorAttachment,
        .newState    = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  const TransientAllocator::Allocation cameraAlloc =
      context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
  shaderio::CameraUniforms cameraData{};
  if(context.params->cameraUniforms != nullptr)
  {
    cameraData = *context.params->cameraUniforms;
  }
  std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
  context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  VkDescriptorSet cameraDescriptorSet = VK_NULL_HANDLE;
  if(!cameraBindGroupHandle.isNull())
  {
    cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  }
  const uint32_t cameraDynamicOffset = cameraAlloc.offset;
  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

  if(hasLineDebug)
  {
    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(debugPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    if(cameraDescriptorSet != VK_NULL_HANDLE)
    {
      vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetScene, 1,
                              &cameraDescriptorSet, 1, &cameraDynamicOffset);
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
    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(gpuCullingDebugPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getDebugPipelineLayout());
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    if(cameraDescriptorSet != VK_NULL_HANDLE)
    {
      vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetScene, 1,
                              &cameraDescriptorSet, 1, &cameraDynamicOffset);
    }

    const shaderio::PushConstantGPUCullDebug pushValues{
        .objectBufferAddress = m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex),
        .resultBufferAddress = m_renderer->getGPUCullingResultBufferAddress(context.frameIndex),
        .objectCount = gpuCullObjectCount,
        .segmentCount = 24u,
        ._padding0 = 0u,
        ._padding1 = 0u,
    };
    const VkPushConstantsInfo pushInfo{
        .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
        .layout     = pipelineLayout,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(pushValues),
        .pValues    = &pushValues,
    };
    rhi::vulkan::cmdPushConstants(*context.cmd, pushInfo);

    context.cmd->draw(pushValues.segmentCount * 2u * 3u, pushValues.objectCount, 0, 0);
  }

  context.cmd->endRenderPass();

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(m_renderer->getSceneResources().getOutputTextureImage()),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::write,
      .dstAccess   = rhi::ResourceAccess::read,
      .oldState    = rhi::ResourceState::ColorAttachment,
      .newState    = rhi::ResourceState::General,
      .isSwapchain = false,
  });

  context.cmd->endEvent();
}

}  // namespace demo
