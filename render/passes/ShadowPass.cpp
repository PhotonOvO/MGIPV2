#include "ShadowPass.h"

#include "../MeshPool.h"
#include "../Renderer.h"
#include "../ShadowResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>

namespace demo {

ShadowPass::ShadowPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ShadowPass::getDependencies() const
{
  return {};
}

void ShadowPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.gltfModel == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || context.params->cameraUniforms == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("ShadowPass");

  ShadowResources& shadowResources = m_renderer->getShadowResources();
  shadowResources.updateShadowMatrices(*context.params->cameraUniforms, context.params->lightDirection);

  const uint32_t shadowMapSize = shadowResources.getShadowMapSize();
  const rhi::Extent2D extent{shadowMapSize, shadowMapSize};
  const VkCommandBuffer nativeCommandBuffer = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

  {
    const VkImageMemoryBarrier2 shadowBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = shadowResources.getShadowMapImage(),
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo depInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &shadowBarrier,
    };
    vkCmdPipelineBarrier2(nativeCommandBuffer, &depInfo);
  }

  const rhi::DepthTargetDesc depthTarget{
      .texture = {},
      .view = rhi::TextureViewHandle::fromNative(shadowResources.getShadowMapView()),
      .state = rhi::ResourceState::DepthStencilAttachment,
      .loadOp = rhi::LoadOp::clear,
      .storeOp = rhi::StoreOp::store,
      .clearValue = {1.0f, 0},
  };

  const rhi::RenderPassDesc passDesc = {
      .renderArea = {{0, 0}, extent},
      .colorTargets = nullptr,
      .colorTargetCount = 0,
      .depthTarget = &depthTarget,
  };
  context.cmd->beginRenderPass(passDesc);

  const rhi::Viewport viewport{
      0.0f, 0.0f,
      static_cast<float>(extent.width),
      static_cast<float>(extent.height),
      0.0f, 1.0f
  };
  const rhi::Rect2D scissor{{0, 0}, extent};
  context.cmd->setViewport(viewport);
  context.cmd->setScissor(scissor);

  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getShadowPipelineLayout());
  if(pipelineLayout == VK_NULL_HANDLE)
  {
    context.cmd->endRenderPass();
    context.cmd->endEvent();
    return;
  }

  const VkDescriptorSet materialTextureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
  if(materialTextureSet != VK_NULL_HANDLE)
  {
    vkCmdBindDescriptorSets(
        nativeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetTextures, 1,
        &materialTextureSet, 0, nullptr);
  }

  const VkDescriptorSet shadowSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getShadowUniformsDescriptorSet());
  if(shadowSet != VK_NULL_HANDLE)
  {
    vkCmdBindDescriptorSets(
        nativeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetScene, 1, &shadowSet, 0,
        nullptr);
  }

  const shaderio::ShadowUniforms* shadowUniforms = shadowResources.getShadowUniformsData();
  vkCmdSetDepthBias(
      nativeCommandBuffer, shadowUniforms->shadowBiasAndKernel.x, 0.0f, shadowUniforms->shadowBiasAndKernel.y);

  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();

  PipelineHandle currentPipeline{};
  for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
  {
    const MeshRecord* mesh = meshPool.tryGet(context.gltfModel->meshes[i]);
    if(mesh == nullptr)
    {
      continue;
    }

    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix            = mesh->transform;
    drawData.baseColorFactor        = glm::vec4(1.0f);
    drawData.baseColorTextureIndex  = -1;
    drawData.normalTextureIndex     = -1;
    drawData.metallicRoughnessTextureIndex = -1;
    drawData.occlusionTextureIndex  = -1;
    drawData.metallicFactor         = 1.0f;
    drawData.roughnessFactor        = 1.0f;
    drawData.normalScale            = 1.0f;
    drawData.alphaMode              = shaderio::LAlphaOpaque;
    drawData.alphaCutoff            = 0.5f;

    if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
    {
      const MaterialHandle materialHandle = context.gltfModel->materials[mesh->materialIndex];
      drawData.baseColorFactor            = m_renderer->getMaterialBaseColorFactor(materialHandle);

      const auto material = m_renderer->getMaterialTextureIndices(materialHandle, context.gltfModel);
      drawData.baseColorTextureIndex = material.baseColor;
      drawData.alphaMode             = material.alphaMode;
      drawData.alphaCutoff           = material.alphaCutoff;

      if(drawData.alphaMode == shaderio::LAlphaBlend)
      {
        continue;
      }
    }

    const PipelineHandle targetPipeline = drawData.alphaMode == shaderio::LAlphaMask
        ? m_renderer->getShadowPipelineAlphaTestHandle()
        : m_renderer->getShadowPipelineOpaqueHandle();
    if(targetPipeline.isNull())
    {
      continue;
    }

    if(targetPipeline != currentPipeline)
    {
      const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
          m_renderer->getPipelineOpaque(targetPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
      vkCmdBindPipeline(nativeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
      currentPipeline = targetPipeline;
    }

    const uint32_t drawAlignment = 256;
    const TransientAllocator::Allocation drawAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);
    std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
    context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

    if(!drawBindGroupHandle.isNull())
    {
      const VkDescriptorSet drawSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      const uint32_t drawDynamicOffset = drawAlloc.offset;
      vkCmdBindDescriptorSets(
          nativeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetDraw, 1, &drawSet, 1,
          &drawDynamicOffset);
    }

    const uint64_t vertexHandle = mesh->vertexBufferHandle;
    const uint64_t vertexOffset = 0;
    context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);

    const uint64_t indexHandle = mesh->indexBufferHandle;
    context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);
    context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
  }

  context.cmd->endRenderPass();

  {
    const VkImageMemoryBarrier2 shadowBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = shadowResources.getShadowMapImage(),
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo depInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &shadowBarrier,
    };
    vkCmdPipelineBarrier2(nativeCommandBuffer, &depInfo);
  }

  context.cmd->endEvent();
}

}  // namespace demo
