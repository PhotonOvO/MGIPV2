#include "CSMShadowPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../CSMShadowResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>
#include <cstring>

namespace demo {

CSMShadowPass::CSMShadowPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> CSMShadowPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 1> dependencies = {
      PassResourceDependency::texture(kPassCSMShadowHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                       rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void CSMShadowPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("CSMShadowPass");

  CSMShadowResources& csm = m_renderer->getCSMShadowResources();
  const uint32_t cascadeCount = csm.getCascadeCount();

  // Render each cascade layer
  for(uint32_t i = 0; i < cascadeCount; ++i)
  {
    renderCascadeLayer(context, i);
  }

  // Final transition: DepthStencil -> General (for sampling in LightPass)
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
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

void CSMShadowPass::renderCascadeLayer(const PassContext& context, uint32_t cascadeIndex) const
{
  CSMShadowResources& csm = m_renderer->getCSMShadowResources();
  shaderio::ShadowUniforms* shadowData = csm.getShadowUniformsData();

  // Get cascade-specific depth target view
  VkImageView layerView = csm.getCascadeLayerView(cascadeIndex);
  const VkExtent2D cascadeExtent = csm.getCascadeExtent();
  const rhi::Extent2D extent{cascadeExtent.width, cascadeExtent.height};

  // Begin render pass with this layer's depth view
  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = rhi::TextureViewHandle::fromNative(layerView),
      .state      = rhi::ResourceState::DepthStencilAttachment,
      .loadOp     = rhi::LoadOp::clear,
      .storeOp    = rhi::StoreOp::store,
      .clearValue = {0.0f, 0},  // Reverse-Z far depth, matching the shadow pipeline compare op.
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

  const PipelineHandle csmPipeline = m_renderer->getCSMShadowPipelineHandle();
  if(csmPipeline.isNull())
  {
    context.cmd->endRenderPass();
    return;
  }

  const VkPipeline nativePipeline =
      reinterpret_cast<VkPipeline>(m_renderer->getPipelineOpaque(csmPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
  rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

  // Bind texture descriptor set (bindless textures)
  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                          shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

  // Upload cascade-specific camera uniforms with cascade view-projection matrix
  const TransientAllocator::Allocation cameraAlloc =
      context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);

  shaderio::CameraUniforms cascadeCamera{};
  cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
  cascadeCamera.projection = cascadeCamera.viewProjection;  // Simplified for shadow pass
  cascadeCamera.view = glm::mat4(1.0f);
  cascadeCamera.inverseViewProjection = glm::inverse(cascadeCamera.viewProjection);
  cascadeCamera.cameraPosition = glm::vec3(0.0f);
  const float baseConstantBias = context.params->lightSettings.depthBias;
  const float baseSlopeBias = context.params->lightSettings.normalBias;
  const float biasScale = shadowData->cascadeBiasScale.z;
  const float cascadeBiasScale = 1.0f + static_cast<float>(cascadeIndex) * biasScale;
  const glm::vec3 lightTravelDir = glm::normalize(context.params->lightSettings.direction);
  const glm::vec3 dirToLight = -lightTravelDir;
  cascadeCamera.shadowConstantBias = baseConstantBias * cascadeBiasScale;
  cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, baseSlopeBias * cascadeBiasScale);

  std::memcpy(cameraAlloc.cpuPtr, &cascadeCamera, sizeof(cascadeCamera));
  context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cascadeCamera));

  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  if(!cameraBindGroupHandle.isNull())
  {
    VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
    const uint32_t cameraDynamicOffset = cameraAlloc.offset;
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
  }

  // Draw meshes for this cascade layer
  drawMeshes(context, pipelineLayout, cascadeIndex);

  context.cmd->endRenderPass();
}

void CSMShadowPass::drawMeshes(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const
{
  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();
  (void)cascadeIndex;

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

      // Skip alpha-blended meshes for shadow pass
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

      // Allocate and upload draw uniforms
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

      // Apply cascade-specific bias in the draw uniforms (if shader supports it)
      // Note: cascadeBiasScale packed values are passed via ShadowUniforms

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

      // Bind vertex and index buffers
      const uint64_t vertexHandle = mesh->vertexBufferHandle;
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(mesh->indexBufferHandle, 0, rhi::IndexFormat::uint32);

      // Draw mesh
      context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
    }
  }
}

}  // namespace demo
