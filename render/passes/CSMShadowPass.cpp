#include "CSMShadowPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../CSMShadowResources.h"
#include "../../common/TracyProfiling.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace demo {

namespace {

// Dynamic UBO alignment requirement (minUniformBufferOffsetAlignment)
constexpr uint32_t kDrawUniformsStride = 256;
constexpr uint32_t kDrawUniformStorageStride = static_cast<uint32_t>(sizeof(shaderio::DrawUniforms));

[[nodiscard]] uint32_t alignUpTo(uint32_t value, uint32_t alignment)
{
    const uint32_t safeAlignment = std::max(1u, alignment);
    const uint32_t remainder = value % safeAlignment;
    return remainder == 0 ? value : (value + safeAlignment - remainder);
}

struct PendingLegacyDraw
{
    const MeshRecord* mesh;
    shaderio::DrawUniforms uniforms;
};

struct PendingMdiDraw
{
    const GltfUploadResult::ShadowPackedMesh* packedMesh;
    shaderio::DrawUniforms uniforms;
};

[[nodiscard]] shaderio::DrawUniforms buildShadowDrawUniforms(const MeshRecord& mesh)
{
    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh.transform;
    drawData.baseColorFactor = mesh.baseColorFactor;
    drawData.baseColorTextureIndex = mesh.baseColorTextureIndex;
    drawData.normalTextureIndex = mesh.normalTextureIndex;
    drawData.metallicRoughnessTextureIndex = mesh.metallicRoughnessTextureIndex;
    drawData.occlusionTextureIndex = mesh.occlusionTextureIndex;
    drawData.metallicFactor = mesh.metallicFactor;
    drawData.roughnessFactor = mesh.roughnessFactor;
    drawData.normalScale = mesh.normalScale;
    drawData.alphaMode = mesh.alphaMode;
    drawData.alphaCutoff = mesh.alphaCutoff;
    return drawData;
}

[[nodiscard]] bool isAabbVisibleInShadowCascade(const glm::vec3& worldBoundsMin,
                                                const glm::vec3& worldBoundsMax,
                                                const glm::mat4& cascadeViewProjection)
{
    const glm::vec4 corners[8] = {
        {worldBoundsMin.x, worldBoundsMin.y, worldBoundsMin.z, 1.0f},
        {worldBoundsMax.x, worldBoundsMin.y, worldBoundsMin.z, 1.0f},
        {worldBoundsMin.x, worldBoundsMax.y, worldBoundsMin.z, 1.0f},
        {worldBoundsMax.x, worldBoundsMax.y, worldBoundsMin.z, 1.0f},
        {worldBoundsMin.x, worldBoundsMin.y, worldBoundsMax.z, 1.0f},
        {worldBoundsMax.x, worldBoundsMin.y, worldBoundsMax.z, 1.0f},
        {worldBoundsMin.x, worldBoundsMax.y, worldBoundsMax.z, 1.0f},
        {worldBoundsMax.x, worldBoundsMax.y, worldBoundsMax.z, 1.0f},
    };

    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;

    for(const glm::vec4& corner : corners)
    {
        const glm::vec4 clip = cascadeViewProjection * corner;

        outsideLeft = outsideLeft && (clip.x < -clip.w);
        outsideRight = outsideRight && (clip.x > clip.w);
        outsideBottom = outsideBottom && (clip.y < -clip.w);
        outsideTop = outsideTop && (clip.y > clip.w);
        outsideNear = outsideNear && (clip.z < 0.0f);
        outsideFar = outsideFar && (clip.z > clip.w);
    }

    return !(outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar);
}

}  // namespace

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

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(csm.getCascadeImage()),
      .aspect      = rhi::TextureAspect::depth,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });

  // Render each cascade layer
  {
    TRACY_ZONE_SCOPED("CSMShadowPass::cascadeLoop");
    for(uint32_t i = 0; i < cascadeCount; ++i)
    {
      renderCascadeLayer(context, i);
    }
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

  const bool useMultiDrawIndirect = context.params->useCsmShadowMultiDrawIndirect
                                    && context.gltfModel != nullptr
                                    && !context.gltfModel->shadowPackedMeshes.empty()
                                    && context.gltfModel->shadowPackedVertexBuffer.buffer != VK_NULL_HANDLE
                                    && context.gltfModel->shadowPackedIndexBuffer.buffer != VK_NULL_HANDLE
                                    && !m_renderer->getCSMShadowMDIDrawBindGroup(context.frameIndex, cascadeIndex).isNull()
                                    && m_renderer->getShadowCullingPipelineHandle().isNull() == false
                                    && m_renderer->getShadowCullingDescriptorSetOpaque(context.frameIndex) != 0
                                    && m_renderer->getShadowCullingIndirectBufferOpaque(context.frameIndex) != 0;

  if(useMultiDrawIndirect && !prepareMultiDrawIndirect(context, cascadeIndex))
  {
    return;
  }

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

  const PipelineHandle csmPipeline = useMultiDrawIndirect ? m_renderer->getCSMShadowPipelineHandle()
                                                          : m_renderer->getShadowPipelineHandle();
  if(csmPipeline.isNull())
  {
    context.cmd->endRenderPass();
    return;
  }

  const VkPipeline nativePipeline =
      reinterpret_cast<VkPipeline>(m_renderer->getPipelineOpaque(csmPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
      useMultiDrawIndirect ? m_renderer->getCSMShadowPipelineLayout() : m_renderer->getGBufferPipelineLayout());
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
  if(useMultiDrawIndirect)
  {
    drawMeshesMultiDrawIndirect(context, pipelineLayout, cascadeIndex);
  }
  else
  {
    drawMeshesLegacy(context, pipelineLayout, cascadeIndex);
  }

  context.cmd->endRenderPass();
}

bool CSMShadowPass::prepareMultiDrawIndirect(const PassContext& context, uint32_t cascadeIndex) const
{
  if(context.gltfModel == nullptr)
  {
    return false;
  }

  const uint32_t objectCount = static_cast<uint32_t>(context.gltfModel->shadowPackedMeshes.size());
  if(objectCount == 0)
  {
    return false;
  }

  const PipelineHandle computePipelineHandle = m_renderer->getShadowCullingPipelineHandle();
  const VkPipelineLayout computePipelineLayout =
      reinterpret_cast<VkPipelineLayout>(m_renderer->getShadowCullingPipelineLayout());
  const VkDescriptorSet descriptorSet =
      reinterpret_cast<VkDescriptorSet>(m_renderer->getShadowCullingDescriptorSetOpaque(context.frameIndex));
  const VkBuffer indirectBuffer =
      reinterpret_cast<VkBuffer>(m_renderer->getShadowCullingIndirectBufferOpaque(context.frameIndex));
  if(computePipelineHandle.isNull() || computePipelineLayout == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE
     || indirectBuffer == VK_NULL_HANDLE)
  {
    return false;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);
  vkCmdBindPipeline(vkCmd,
                    VK_PIPELINE_BIND_POINT_COMPUTE,
                    reinterpret_cast<VkPipeline>(m_renderer->getPipelineOpaque(
                        computePipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE))));
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

  const shaderio::ShadowCullPushConstants pushConstants = m_renderer->buildShadowCullPushConstants(cascadeIndex, objectCount);
  const VkPushConstantsInfo pushInfo{
      .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
      .layout     = computePipelineLayout,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(shaderio::ShadowCullPushConstants),
      .pValues    = &pushConstants,
  };
  rhi::vulkan::cmdPushConstants(*context.cmd, pushInfo);
  vkCmdDispatch(vkCmd, (objectCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount, 1u, 1u);

  const VkBufferMemoryBarrier2 indirectBarrier{
      .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
      .dstAccessMask       = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer              = indirectBuffer,
      .offset              = 0,
      .size                = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &indirectBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
  return true;
}

void CSMShadowPass::drawMeshesLegacy(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const
{
  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();
  const shaderio::ShadowUniforms* shadowData = m_renderer->getCSMShadowResources().getShadowUniformsData();

  if(context.gltfModel == nullptr || drawBindGroupHandle.isNull())
  {
    return;
  }

  // First pass: collect visible meshes and compute DrawUniforms
  // Use pre-built shadow caster list (already excludes transparent meshes)
  std::vector<PendingLegacyDraw> pendingDraws;
  pendingDraws.reserve(context.gltfModel->shadowCasterIndices.size());
  {
    TRACY_ZONE_SCOPED("CSMShadowPass::collectMeshes");
    for(size_t idx : context.gltfModel->shadowCasterIndices)
    {
      MeshHandle meshHandle = context.gltfModel->meshes[idx];
      const MeshRecord* mesh = meshPool.tryGet(meshHandle);
      if(mesh == nullptr)
      {
        continue;
      }

      if(!isAabbVisibleInShadowCascade(mesh->worldBoundsMin, mesh->worldBoundsMax, shadowData->cascadeViewProjection[cascadeIndex]))
      {
        continue;
      }

      pendingDraws.push_back({mesh, buildShadowDrawUniforms(*mesh)});
    }
  }

  // Batch allocate DrawUniforms for all visible meshes
  if(pendingDraws.empty())
  {
    return;
  }

  const uint32_t batchSize = static_cast<uint32_t>(pendingDraws.size()) * kDrawUniformsStride;
  const TransientAllocator::Allocation batchAlloc =
      context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

  {
    TRACY_ZONE_SCOPED("CSMShadowPass::batchUpload");
    // Write all DrawUniforms at once
    for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
    {
      std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + slot * kDrawUniformsStride;
      std::memcpy(dst, &pendingDraws[slot].uniforms, sizeof(shaderio::DrawUniforms));
    }
    context.transientAllocator->flushAllocation(batchAlloc, batchSize);
  }

  // Second pass: bind descriptors and draw
  VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
      m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));

  {
    TRACY_ZONE_SCOPED("CSMShadowPass::drawLoop");
    const demo::rhi::CommandList& commandList = *context.cmd;
    constexpr VkDeviceSize vertexOffset = 0;
    constexpr VkDeviceSize indexOffset = 0;
    VkBuffer lastVertexBuffer = VK_NULL_HANDLE;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
    for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
    {
      const PendingLegacyDraw& draw = pendingDraws[slot];

      // Bind draw descriptor set with dynamic offset
      const uint32_t drawDynamicOffset = batchAlloc.offset + static_cast<uint32_t>(slot) * kDrawUniformsStride;
      rhi::vulkan::cmdBindDescriptorSets(commandList, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetDraw, 1,
                                         &drawDescriptorSet, 1, &drawDynamicOffset);

      const VkBuffer vertexBuffer = draw.mesh->getNativeVertexBuffer();
      if(vertexBuffer != lastVertexBuffer)
      {
        rhi::vulkan::cmdBindVertexBuffers(commandList, 0, 1, &vertexBuffer, &vertexOffset);
        lastVertexBuffer = vertexBuffer;
      }

      const VkBuffer indexBuffer = draw.mesh->getNativeIndexBuffer();
      if(indexBuffer != lastIndexBuffer)
      {
        rhi::vulkan::cmdBindIndexBuffer(commandList, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);
        lastIndexBuffer = indexBuffer;
      }

      // Draw mesh
      rhi::vulkan::cmdDrawIndexed(commandList, draw.mesh->indexCount, 1, 0, 0, 0);
    }
  }
}

void CSMShadowPass::drawMeshesMultiDrawIndirect(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const
{
  const BindGroupHandle drawBindGroupHandle = m_renderer->getCSMShadowMDIDrawBindGroup(context.frameIndex, cascadeIndex);
  if(context.gltfModel == nullptr || drawBindGroupHandle.isNull() || context.gltfModel->shadowPackedMeshes.empty())
  {
    return;
  }

  const demo::rhi::CommandList& commandList = *context.cmd;
  VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
      m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
  rhi::vulkan::cmdBindDescriptorSets(commandList, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, shaderio::LSetDraw, 1,
                                     &drawDescriptorSet, 0, nullptr);

  constexpr VkDeviceSize vertexOffset = 0;
  constexpr VkDeviceSize indexOffset = 0;
  const VkBuffer vertexBuffer = context.gltfModel->shadowPackedVertexBuffer.buffer;
  const VkBuffer indexBuffer = context.gltfModel->shadowPackedIndexBuffer.buffer;
  rhi::vulkan::cmdBindVertexBuffers(commandList, 0, 1, &vertexBuffer, &vertexOffset);
  rhi::vulkan::cmdBindIndexBuffer(commandList, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);

  {
    TRACY_ZONE_SCOPED("CSMShadowPass::drawLoopMDI");
    context.cmd->drawIndexedIndirect(m_renderer->getShadowCullingIndirectBufferOpaque(context.frameIndex), 0,
                                     static_cast<uint32_t>(context.gltfModel->shadowPackedMeshes.size()),
                                     sizeof(VkDrawIndexedIndirectCommand));
  }
}

}  // namespace demo
