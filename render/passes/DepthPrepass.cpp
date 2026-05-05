#include "DepthPrepass.h"

#include "../ClipSpaceConvention.h"
#include "../MeshPool.h"
#include "../Renderer.h"
#include "../SceneResources.h"
#include "../../common/TracyProfiling.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>
#include <vector>

namespace demo {

namespace {

// Dynamic UBO alignment requirement (minUniformBufferOffsetAlignment)
constexpr uint32_t kDrawUniformsStride = 256;

struct PendingDraw
{
    size_t meshIndex;
    const MeshRecord* mesh;
    shaderio::DrawUniforms uniforms;
    PipelineHandle pipeline;
};

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

DepthPrepass::DepthPrepass(Renderer* renderer)
    : m_renderer(renderer)
{}

PassNode::HandleSlice<PassResourceDependency> DepthPrepass::getDependencies() const
{
  static const std::array<PassResourceDependency, 2> dependencies = {
      PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
                                      rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void DepthPrepass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
  {
    return;
  }

  context.cmd->beginEvent("DepthPrepass");

  SceneResources& sceneResources = m_renderer->getSceneResources();
  const VkExtent2D vkExtent = sceneResources.getSize();
  const rhi::Extent2D extent{vkExtent.width, vkExtent.height};

  const rhi::DepthTargetDesc depthTarget{
      .texture    = {},
      .view       = rhi::TextureViewHandle::fromNative(sceneResources.getDepthImageView()),
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
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getDepthImage()),
      .aspect      = sceneDepthAspect(sceneResources.getDepthFormat()),
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::DepthStencilAttachment,
      .isSwapchain = false,
  });
  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  if(context.gltfModel == nullptr || context.gltfModel->meshes.empty() || context.drawStream == nullptr)
  {
    context.cmd->endRenderPass();
    context.cmd->endEvent();
    return;
  }

  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
  const VkDescriptorSet  textureSet     = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                          shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

  // Use shared camera allocation from PassContext (allocated once per frame by Renderer)
  if(!context.cameraAllocValid)
  {
    context.cmd->endRenderPass();
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
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
  }

  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();

  const uint64_t indirectBufferHandle = m_renderer->getPreviousGPUCullingIndirectBufferOpaque(context.frameIndex);
  const uint32_t previousIndirectObjectCount =
      m_renderer->getPreviousGPUCullingObjectCount(context.frameIndex, context.gltfModel);
  const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

  // Build per-draw data on the CPU, but prefer last frame's GPU-generated indirect commands
  // to decide whether each draw actually emits geometry in this frame's prepass.
  std::vector<PendingDraw> pendingDraws;
  {
    TRACY_ZONE_SCOPED("DepthPrepass::collectMeshes");
    // Process opaque meshes
    const PipelineHandle opaquePipeline = m_renderer->getDepthPrepassOpaquePipelineHandle();
    for(size_t idx : context.gltfModel->opaqueMeshIndices)
    {
      const MeshRecord* mesh = meshPool.tryGet(context.gltfModel->meshes[idx]);
      if(mesh == nullptr)
      {
        continue;
      }

      // Compute DrawUniforms
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
      drawData.alphaMode = shaderio::LAlphaOpaque;
      drawData.alphaCutoff = mesh->alphaCutoff;

      pendingDraws.push_back({idx, mesh, drawData, opaquePipeline});
    }

    // Process alpha-test meshes
    const PipelineHandle alphaTestPipeline = m_renderer->getDepthPrepassAlphaTestPipelineHandle();
    for(size_t idx : context.gltfModel->alphaTestMeshIndices)
    {
      const MeshRecord* mesh = meshPool.tryGet(context.gltfModel->meshes[idx]);
      if(mesh == nullptr)
      {
        continue;
      }

      // Compute DrawUniforms
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
      drawData.alphaMode = shaderio::LAlphaMask;
      drawData.alphaCutoff = mesh->alphaCutoff;

      pendingDraws.push_back({idx, mesh, drawData, alphaTestPipeline});
    }
  }

  // Batch allocate DrawUniforms for all visible meshes
  if(!pendingDraws.empty() && !drawBindGroupHandle.isNull())
  {
    const bool useMdi = indirectBufferHandle != 0 && !m_renderer->getDepthMDIDrawBindGroup(context.frameIndex).isNull();
    const uint32_t batchSize = static_cast<uint32_t>(pendingDraws.size()) * kDrawUniformsStride;
    const TransientAllocator::Allocation batchAlloc =
        useMdi ? TransientAllocator::Allocation{} : context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

    {
      TRACY_ZONE_SCOPED("DepthPrepass::batchUpload");
      if(useMdi)
      {
        std::vector<shaderio::DrawUniforms> mdiDrawData(context.gltfModel->meshes.size());
        for(const PendingDraw& draw : pendingDraws)
        {
          mdiDrawData[draw.meshIndex] = draw.uniforms;
        }
        m_renderer->uploadDepthMDIDrawData(context.frameIndex, mdiDrawData);
      }
      else
      {
        for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
        {
          std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + static_cast<uint32_t>(slot) * kDrawUniformsStride;
          std::memcpy(dst, &pendingDraws[slot].uniforms, sizeof(shaderio::DrawUniforms));
        }
        context.transientAllocator->flushAllocation(batchAlloc, batchSize);
      }
    }

    if(useMdi)
    {
      const VkPipelineLayout mdiPipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getMDIPipelineLayout());
      const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                              shaderio::LSetTextures, 1, &textureSet, 0, nullptr);
      if(!cameraBindGroupHandle.isNull())
      {
        VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
            m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
        const uint32_t cameraDynamicOffset = cameraAlloc.offset;
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                                shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
      }
      const BindGroupHandle mdiDrawBindGroupHandle = m_renderer->getDepthMDIDrawBindGroup(context.frameIndex);
      const VkDescriptorSet mdiDrawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(mdiDrawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, mdiPipelineLayout,
                              shaderio::LSetDraw, 1, &mdiDrawDescriptorSet, 0, nullptr);

      const uint64_t sharedVertexHandle = pendingDraws.front().mesh->vertexBufferHandle;
      const uint64_t sharedIndexHandle = pendingDraws.front().mesh->indexBufferHandle;
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &sharedVertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(sharedIndexHandle, 0, rhi::IndexFormat::uint32);

      TRACY_ZONE_SCOPED("DepthPrepass::drawLoopMDI");
      size_t runBegin = 0;
      while(runBegin < pendingDraws.size())
      {
        const PipelineHandle mdiPipeline =
            pendingDraws[runBegin].pipeline == m_renderer->getDepthPrepassAlphaTestPipelineHandle()
                ? m_renderer->getDepthPrepassAlphaTestMDIPipelineHandle()
                : m_renderer->getDepthPrepassOpaqueMDIPipelineHandle();
        const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getPipelineOpaque(mdiPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
        rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

        size_t runEnd = runBegin + 1;
        while(runEnd < pendingDraws.size()
              && pendingDraws[runEnd].pipeline == pendingDraws[runBegin].pipeline
              && pendingDraws[runEnd].meshIndex == pendingDraws[runEnd - 1].meshIndex + 1
              && pendingDraws[runEnd].meshIndex < previousIndirectObjectCount)
        {
          ++runEnd;
        }

        const uint32_t runCount = static_cast<uint32_t>(runEnd - runBegin);
        if(pendingDraws[runBegin].meshIndex < previousIndirectObjectCount)
        {
          context.cmd->drawIndexedIndirect(indirectBufferHandle,
                                           static_cast<uint64_t>(pendingDraws[runBegin].meshIndex) * indirectCommandStride,
                                           runCount,
                                           indirectCommandStride);
        }
        else
        {
          for(size_t slot = runBegin; slot < runEnd; ++slot)
          {
            const PendingDraw& draw = pendingDraws[slot];
            context.cmd->drawIndexed(draw.mesh->indexCount, 1, draw.mesh->firstIndex, draw.mesh->vertexOffset, 0);
          }
        }
        runBegin = runEnd;
      }
    }
    else
    {
      VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));

      TRACY_ZONE_SCOPED("DepthPrepass::drawLoop");
      PipelineHandle currentPipeline{};
      uint64_t currentVertexHandle = 0;
      uint64_t currentIndexHandle = 0;
      for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
      {
        const PendingDraw& draw = pendingDraws[slot];

        if(draw.pipeline != currentPipeline)
        {
          const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
              m_renderer->getPipelineOpaque(draw.pipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
          rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
          currentPipeline = draw.pipeline;
        }

        const uint32_t drawDynamicOffset = batchAlloc.offset + static_cast<uint32_t>(slot) * kDrawUniformsStride;
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);

        const uint64_t vertexHandle = draw.mesh->vertexBufferHandle;
        const uint64_t indexHandle = draw.mesh->indexBufferHandle;
        if(vertexHandle != currentVertexHandle)
        {
          const uint64_t vo = 0;
          context.cmd->bindVertexBuffers(0, &vertexHandle, &vo, 1);
          currentVertexHandle = vertexHandle;
        }
        if(indexHandle != currentIndexHandle)
        {
          context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);
          currentIndexHandle = indexHandle;
        }
        if(indirectBufferHandle != 0 && draw.meshIndex < previousIndirectObjectCount)
        {
          context.cmd->drawIndexedIndirect(indirectBufferHandle,
                                           static_cast<uint64_t>(draw.meshIndex) * indirectCommandStride,
                                           1,
                                           indirectCommandStride);
        }
        else
        {
          context.cmd->drawIndexed(draw.mesh->indexCount, 1, draw.mesh->firstIndex, draw.mesh->vertexOffset, 0);
        }
      }
    }
  }

  context.cmd->endRenderPass();
  context.cmd->endEvent();
}

}  // namespace demo
