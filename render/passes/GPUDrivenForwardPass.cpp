#include "GPUDrivenForwardPass.h"

#include "../GPUDrivenRenderer.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../ClipSpaceConvention.h"
#include "../../common/TracyProfiling.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

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
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::readWrite, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenForwardPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenForwardPass");

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->sceneDepthView == VK_NULL_HANDLE || sceneView->outputView == VK_NULL_HANDLE)
  {
    context.cmd->endEvent();
    return;
  }
  const auto restoreDepthForSampling = [&]() {
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
        .aspect = sceneDepthAspect(sceneView->sceneDepthFormat),
        .srcStage = rhi::PipelineStage::FragmentShader,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .srcAccess = rhi::ResourceAccess::read,
        .dstAccess = rhi::ResourceAccess::read,
        .oldState = rhi::ResourceState::DepthStencilAttachment,
        .newState = rhi::ResourceState::General,
        .isSwapchain = false,
    });
  };

  const VkImageView outputImageView = sceneView->outputView;
  const VkExtent2D vkExtent = sceneView->sceneDepthExtent;
  const rhi::Extent2D renderExtent = {vkExtent.width, vkExtent.height};
  if(outputImageView == VK_NULL_HANDLE || renderExtent.width == 0 || renderExtent.height == 0)
  {
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

  MeshPool& meshPool = m_renderer->getMeshPool();
  const uint32_t objectCount = m_renderer->getGPUCullingObjectCount(context.frameIndex);
  const uint64_t indirectBufferHandle = m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
  const uint64_t countBufferHandle = m_renderer->getGPUCullingDrawCountBufferOpaque(context.frameIndex);
  if(objectCount == 0 || indirectBufferHandle == 0 || countBufferHandle == 0)
  {
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

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

  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(outputImageView),
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
  };
  const rhi::DepthTargetDesc depthTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(sceneView->sceneDepthView),
      .state = rhi::ResourceState::DepthStencilAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},
  };

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->sceneDepthImage),
      .aspect = sceneDepthAspect(sceneView->sceneDepthFormat),
      .srcStage = rhi::PipelineStage::FragmentShader,
      .dstStage = rhi::PipelineStage::FragmentShader,
      .srcAccess = rhi::ResourceAccess::read,
      .dstAccess = rhi::ResourceAccess::read,
      .oldState = rhi::ResourceState::General,
      .newState = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

  const PipelineHandle forwardPipeline = m_renderer->getForwardMDIPipelineHandle();
  if(forwardPipeline.isNull())
  {
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
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

  const VkPipeline nativePipeline =
      reinterpret_cast<VkPipeline>(m_renderer->getNativeGraphicsPipeline(forwardPipeline));
  rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

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

  if(!context.cameraAllocValid)
  {
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
    restoreDepthForSampling();
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
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout,
                            shaderio::LSetScene,
                            1,
                            &cameraDescriptorSet,
                            1,
                            &cameraDynamicOffset);
  }

  const BindGroupHandle drawBindGroupHandle = m_renderer->getMDIDrawBindGroup(context.frameIndex);
  if(drawBindGroupHandle.isNull())
  {
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
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

  const uint32_t transparentCapacity = static_cast<uint32_t>(m_renderer->getTransparentDrawIndices().size());
  if(transparentCapacity == 0u)
  {
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
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

  std::vector<shaderio::GPUCullIndirectCommand> transparentBootstrap(transparentCapacity);
  m_renderer->uploadForwardMDICommands(context.frameIndex, transparentBootstrap);
  const uint64_t forwardIndirectBufferHandle = m_renderer->getForwardMDIIndirectBuffer(context.frameIndex);
  if(forwardIndirectBufferHandle == 0
     || !m_renderer->prepareAndDispatchVisibilityPatch(*context.cmd,
                                                       context.frameIndex,
                                                       forwardIndirectBufferHandle,
                                                       0x80000000u,
                                                       0u))
  {
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
    restoreDepthForSampling();
    context.cmd->endEvent();
    return;
  }

  const rhi::RenderPassDesc passDesc{
      .renderArea = {{0, 0}, renderExtent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = &depthTarget,
  };
  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, renderExtent});

  VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
      m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout,
                          shaderio::LSetDraw,
                          1,
                          &drawDescriptorSet,
                          0,
                          nullptr);

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
    const uint64_t vertexBufferHandle = representativeMesh->vertexBufferHandle;
    const uint64_t indexBufferHandle = representativeMesh->indexBufferHandle;
    const uint64_t vertexOffset = 0;
    context.cmd->bindVertexBuffers(0, &vertexBufferHandle, &vertexOffset, 1);
    context.cmd->bindIndexBuffer(indexBufferHandle, 0, rhi::IndexFormat::uint32);

    context.cmd->drawIndexedIndirectCount(forwardIndirectBufferHandle,
                                          0,
                                          countBufferHandle,
                                          offsetof(shaderio::GPUCullDrawCounts, transparentCount),
                                          transparentCapacity,
                                          m_renderer->getGPUCullingIndirectCommandStride());
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
  restoreDepthForSampling();
  context.cmd->endEvent();
}

}  // namespace demo
