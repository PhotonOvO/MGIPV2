#include "ShadowPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>
#include <cstring>

namespace demo {

ShadowPass::ShadowPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ShadowPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::texture(kPassShadowHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void ShadowPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("ShadowPass");

  SceneResources& sceneResources = m_renderer->getSceneResources();
  const VkExtent2D shadowExtent = sceneResources.getShadowMapExtent();
  const rhi::Extent2D extent{shadowExtent.width, shadowExtent.height};

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = rhi::TextureViewHandle::fromNative(sceneResources.getShadowMapView()),
      .state      = rhi::ResourceState::DepthStencilAttachment,
      .loadOp     = rhi::LoadOp::clear,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {1.0f, 0},
  };

  const rhi::RenderPassDesc passDesc{
      .renderArea       = {{0, 0}, extent},
      .colorTargets     = nullptr,
      .colorTargetCount = 0,
      .depthTarget      = &depthTarget,
  };
  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle shadowPipeline = m_renderer->getShadowPipelineHandle();
  if(shadowPipeline.isNull())
  {
    context.cmd->endRenderPass();
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassShadowHandle.index, kPassShadowHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getShadowMapImage()),
        .aspect      = rhi::TextureAspect::depth,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::write,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::DepthStencilAttachment,
        .newState    = rhi::ResourceState::General,
        .isSwapchain = false,
    });
    context.cmd->endEvent();
    return;
  }

  const VkPipeline nativePipeline =
      reinterpret_cast<VkPipeline>(m_renderer->getPipelineOpaque(shadowPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
  rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                          shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

  const TransientAllocator::Allocation cameraAlloc =
      context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
  const shaderio::CameraUniforms shadowCamera = m_renderer->getShadowCameraUniforms();
  std::memcpy(cameraAlloc.cpuPtr, &shadowCamera, sizeof(shadowCamera));
  context.transientAllocator->flushAllocation(cameraAlloc, sizeof(shadowCamera));

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

  if(context.gltfModel != nullptr)
  {
    for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
    {
      MeshHandle meshHandle = context.gltfModel->meshes[i];
      const MeshRecord* mesh = meshPool.tryGet(meshHandle);
      if(mesh == nullptr)
      {
        continue;
      }
      int32_t alphaMode = shaderio::LAlphaOpaque;
      if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
      {
        MaterialHandle materialHandle = context.gltfModel->materials[mesh->materialIndex];
        alphaMode = m_renderer->getMaterialTextureIndices(materialHandle, context.gltfModel).alphaMode;
        if(alphaMode == shaderio::LAlphaBlend)
        {
          continue;
        }
      }

      const TransientAllocator::Allocation drawAlloc =
          context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), 256);

      shaderio::DrawUniforms drawData{};
      drawData.modelMatrix = mesh->transform;
      drawData.baseColorFactor = glm::vec4(1.0f);
      drawData.baseColorTextureIndex = -1;
      drawData.normalTextureIndex = -1;
      drawData.metallicRoughnessTextureIndex = -1;
      drawData.occlusionTextureIndex = -1;
      drawData.metallicFactor = 1.0f;
      drawData.roughnessFactor = 1.0f;
      drawData.normalScale = 1.0f;
      drawData.alphaMode = alphaMode;
      drawData.alphaCutoff = 0.5f;

      if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
      {
        MaterialHandle materialHandle = context.gltfModel->materials[mesh->materialIndex];
        drawData.baseColorFactor = m_renderer->getMaterialBaseColorFactor(materialHandle);
        auto indices = m_renderer->getMaterialTextureIndices(materialHandle, context.gltfModel);
        drawData.baseColorTextureIndex = indices.baseColor;
        drawData.alphaMode = indices.alphaMode;
        drawData.alphaCutoff = indices.alphaCutoff;
      }

      std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
      context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

      if(!drawBindGroupHandle.isNull())
      {
        VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
            m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
        const uint32_t drawDynamicOffset = drawAlloc.offset;
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);
      }

      const uint64_t vertexHandle = mesh->vertexBufferHandle;
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(mesh->indexBufferHandle, 0, rhi::IndexFormat::uint32);
      context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
    }
  }

  context.cmd->endRenderPass();

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassShadowHandle.index, kPassShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getShadowMapImage()),
      .aspect      = rhi::TextureAspect::depth,
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
