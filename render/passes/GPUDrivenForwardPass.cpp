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
  std::vector<uint32_t> sortedTransparentDrawIndices;
  const auto transparentDrawIndices = m_renderer->getTransparentDrawIndices();
  sortedTransparentDrawIndices.assign(transparentDrawIndices.begin(), transparentDrawIndices.end());

  std::vector<std::pair<MeshHandle, float>> transparentMeshes;
  transparentMeshes.reserve(sortedTransparentDrawIndices.size());

  glm::vec3 cameraPos(0.0f);
  if(context.params->cameraUniforms != nullptr)
  {
    cameraPos = context.params->cameraUniforms->cameraPosition;
  }

  {
    TRACY_ZONE_SCOPED("GPUDrivenForwardPass::transparentSorting");
    const uint32_t transparentCount = static_cast<uint32_t>(sortedTransparentDrawIndices.size());
    for(uint32_t transparentIndex = 0; transparentIndex < transparentCount; ++transparentIndex)
    {
      const uint32_t drawIndex = sortedTransparentDrawIndices[transparentIndex];
      MeshHandle meshHandle = kNullMeshHandle;
      if(!m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
      {
        continue;
      }

      const MeshRecord* mesh = meshPool.tryGet(meshHandle);
      if(mesh == nullptr || mesh->alphaMode != shaderio::LAlphaBlend)
      {
        continue;
      }

      const glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
      transparentMeshes.push_back({meshHandle, glm::length(meshCenter - cameraPos)});
    }

    std::sort(transparentMeshes.begin(), transparentMeshes.end(), [](const auto& left, const auto& right) {
      return left.second > right.second;
    });

    sortedTransparentDrawIndices.clear();
    sortedTransparentDrawIndices.reserve(transparentMeshes.size());
    for(const auto& transparentMesh : transparentMeshes)
    {
      uint32_t drawIndex = 0;
      if(!m_renderer->tryGetMeshDrawIndex(transparentMesh.first, drawIndex))
      {
        continue;
      }
      sortedTransparentDrawIndices.push_back(drawIndex);
    }
  }

  if(transparentMeshes.empty() || sortedTransparentDrawIndices.empty())
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

  std::vector<shaderio::DrawUniforms> mdiDrawData;
  mdiDrawData.reserve(transparentMeshes.size());
  std::vector<shaderio::GPUCullIndirectCommand> indirectCommands;
  indirectCommands.reserve(transparentMeshes.size());

  for(size_t slot = 0; slot < transparentMeshes.size(); ++slot)
  {
    const MeshRecord* mesh = meshPool.tryGet(transparentMeshes[slot].first);
    if(mesh == nullptr)
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
    drawData.alphaMode = shaderio::LAlphaBlend;
    drawData.alphaCutoff = mesh->alphaCutoff;
    mdiDrawData.push_back(drawData);
    indirectCommands.push_back(shaderio::GPUCullIndirectCommand{
        .indexCount = mesh->indexCount,
        .instanceCount = 1,
        .firstIndex = mesh->firstIndex,
        .vertexOffset = mesh->vertexOffset,
        .firstInstance = static_cast<uint32_t>(slot),
    });
  }

  if(mdiDrawData.empty() || indirectCommands.empty())
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

  m_renderer->uploadSharedMDIDrawData(context.frameIndex, mdiDrawData);
  m_renderer->uploadForwardMDICommands(context.frameIndex, indirectCommands);

  const uint64_t indirectBufferHandle = m_renderer->getForwardMDIIndirectBuffer(context.frameIndex);
  if(indirectBufferHandle == 0)
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

  m_renderer->prepareAndDispatchTransparentVisibilityPatch(
      *context.cmd, context.frameIndex, sortedTransparentDrawIndices, indirectBufferHandle);

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

  size_t runBegin = 0;
  while(runBegin < transparentMeshes.size())
  {
    const MeshRecord* runMesh = meshPool.tryGet(transparentMeshes[runBegin].first);
    if(runMesh == nullptr)
    {
      ++runBegin;
      continue;
    }

    const uint64_t vertexBufferHandle = runMesh->vertexBufferHandle;
    const uint64_t indexBufferHandle = runMesh->indexBufferHandle;
    size_t runEnd = runBegin + 1;
    while(runEnd < transparentMeshes.size())
    {
      const MeshRecord* candidateMesh = meshPool.tryGet(transparentMeshes[runEnd].first);
      if(candidateMesh == nullptr || candidateMesh->vertexBufferHandle != vertexBufferHandle
         || candidateMesh->indexBufferHandle != indexBufferHandle)
      {
        break;
      }
      ++runEnd;
    }

    const uint64_t vertexOffset = 0;
    context.cmd->bindVertexBuffers(0, &vertexBufferHandle, &vertexOffset, 1);
    context.cmd->bindIndexBuffer(indexBufferHandle, 0, rhi::IndexFormat::uint32);
    context.cmd->drawIndexedIndirect(
        indirectBufferHandle,
        static_cast<uint64_t>(runBegin) * sizeof(shaderio::GPUCullIndirectCommand),
        static_cast<uint32_t>(runEnd - runBegin),
        static_cast<uint32_t>(sizeof(shaderio::GPUCullIndirectCommand)));
    runBegin = runEnd;
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
