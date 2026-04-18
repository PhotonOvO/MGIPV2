#include "DepthPrepass.h"

#include "../ClipSpaceConvention.h"
#include "../MeshPool.h"
#include "../Renderer.h"
#include "../SceneResources.h"
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

  const TransientAllocator::Allocation cameraAlloc =
      context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
  shaderio::CameraUniforms cameraData{};
  if(context.params->cameraUniforms != nullptr)
  {
    cameraData = *context.params->cameraUniforms;
  }
  else
  {
    cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraData.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    cameraData.viewProjection = cameraData.projection * cameraData.view;
    cameraData.inverseViewProjection = glm::inverse(cameraData.viewProjection);
    cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
  }
  std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
  context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

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

  // First pass: collect visible meshes and compute DrawUniforms
  std::vector<PendingDraw> pendingDraws;
  for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
  {
    const MeshRecord* mesh = meshPool.tryGet(context.gltfModel->meshes[i]);
    if(mesh == nullptr)
    {
      continue;
    }

    // Use pre-computed alphaMode from mesh - skip transparent meshes
    const int32_t alphaMode = mesh->alphaMode;
    if(alphaMode == shaderio::LAlphaBlend)
    {
      continue;
    }

    const PipelineHandle pipeline = alphaMode == shaderio::LAlphaMask
                                        ? m_renderer->getDepthPrepassAlphaTestPipelineHandle()
                                        : m_renderer->getDepthPrepassOpaquePipelineHandle();
    if(pipeline.isNull())
    {
      continue;
    }

    // Compute DrawUniforms
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
    drawData.alphaCutoff = mesh->alphaCutoff;

    pendingDraws.push_back({i, mesh, drawData, pipeline});
  }

  // Batch allocate DrawUniforms for all visible meshes
  if(!pendingDraws.empty() && !drawBindGroupHandle.isNull())
  {
    const uint32_t batchSize = static_cast<uint32_t>(pendingDraws.size()) * kDrawUniformsStride;
    const TransientAllocator::Allocation batchAlloc =
        context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

    // Write all DrawUniforms at once
    for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
    {
      std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + slot * kDrawUniformsStride;
      std::memcpy(dst, &pendingDraws[slot].uniforms, sizeof(shaderio::DrawUniforms));
    }
    context.transientAllocator->flushAllocation(batchAlloc, batchSize);

    // Second pass: bind pipeline/descriptors and draw
    VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));

    PipelineHandle currentPipeline{};
    for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
    {
      const PendingDraw& draw = pendingDraws[slot];

      // Bind pipeline if changed
      if(draw.pipeline != currentPipeline)
      {
        const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getPipelineOpaque(draw.pipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
        rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
        currentPipeline = draw.pipeline;
      }

      // Bind draw descriptor set with dynamic offset
      const uint32_t drawDynamicOffset = batchAlloc.offset + static_cast<uint32_t>(slot) * kDrawUniformsStride;
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                              shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);

      // Bind vertex and index buffers
      const uint64_t vertexHandle = draw.mesh->vertexBufferHandle;
      const uint64_t vertexOffset = 0;
      context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);
      context.cmd->bindIndexBuffer(draw.mesh->indexBufferHandle, 0, rhi::IndexFormat::uint32);
      context.cmd->drawIndexed(draw.mesh->indexCount, 1, 0, 0, 0);
    }
  }

  context.cmd->endRenderPass();
  context.cmd->endEvent();
}

}  // namespace demo
