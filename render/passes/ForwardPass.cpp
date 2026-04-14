#include "ForwardPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

ForwardPass::ForwardPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ForwardPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 1> dependencies = {
        PassResourceDependency::texture(
            kPassOutputHandle,
            ResourceAccess::readWrite,  // Read existing LightPass output, write blended result
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void ForwardPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
    {
        return;
    }

    context.cmd->beginEvent("ForwardPass");

    // Get output texture view and extent (follows screen size)
    const VkImageView outputImageView = m_renderer->getOutputTextureView();
    const VkExtent2D vkExtent = m_renderer->getSceneResources().getSize();
    const rhi::Extent2D outputExtent = {vkExtent.width, vkExtent.height};

    if(outputImageView == VK_NULL_HANDLE)
    {
        context.cmd->endEvent();
        return;
    }

    // Check if we have transparent meshes to render
    if(context.gltfModel == nullptr || context.gltfModel->meshes.empty())
    {
        context.cmd->endEvent();
        return;
    }

    MeshPool& meshPool = m_renderer->getMeshPool();
    SceneResources& sceneResources = m_renderer->getSceneResources();
    // Use output texture extent for rendering
    const rhi::Extent2D renderExtent = outputExtent;
    if(renderExtent.width == 0 || renderExtent.height == 0)
    {
        context.cmd->endEvent();
        return;
    }

    // Collect transparent meshes and sort by distance
    std::vector<std::pair<size_t, float>> transparentMeshes;

    glm::vec3 cameraPos(0.0f);
    if(context.params->cameraUniforms != nullptr)
    {
        cameraPos = context.params->cameraUniforms->cameraPosition;
    }

    for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
    {
        MeshHandle meshHandle = context.gltfModel->meshes[i];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr) continue;

        // Check material alpha mode
        bool isTransparent = false;
        if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
        {
            MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
            auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
            if(indices.alphaMode == shaderio::LAlphaBlend)
            {
                isTransparent = true;
            }
        }

        if(!isTransparent) continue;

        // Calculate distance from camera (use mesh center)
        glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
        float distance = glm::length(meshCenter - cameraPos);
        transparentMeshes.push_back({i, distance});
    }

    // No transparent meshes, skip rendering
    if(transparentMeshes.empty())
    {
        context.cmd->endEvent();
        return;
    }

    // Sort far to near (back to front)
    std::sort(transparentMeshes.begin(), transparentMeshes.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    // Setup color attachment with LOAD loadOp (preserve LightPass output)
    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getOutputTextureImage()),
        .aspect      = rhi::TextureAspect::color,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::read,
        .dstAccess   = rhi::ResourceAccess::write,
        .oldState    = rhi::ResourceState::General,
        .newState    = rhi::ResourceState::ColorAttachment,
        .isSwapchain = false,
    });

    rhi::RenderTargetDesc colorTarget = {
        .texture = {},  // Not used when view carries native pointer
        .view = rhi::TextureViewHandle::fromNative(outputImageView),
        .state = rhi::ResourceState::ColorAttachment,
        .loadOp = rhi::LoadOp::load,  // Preserve existing content
        .storeOp = rhi::StoreOp::store,
    };

    // Note: Not using depth attachment for transparent pass since:
    // 1. OutputTexture is 1920x1080 but depth buffer is viewport-sized
    // 2. Transparent objects are sorted back-to-front for proper blending

    // Begin render pass using RHI interface
    const rhi::RenderPassDesc passDesc = {
        .renderArea = {{0, 0}, renderExtent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = nullptr,
    };
    context.cmd->beginRenderPass(passDesc);

    // Set viewport and scissor using RHI interface
    const rhi::Viewport viewport{
        0.0f, 0.0f,
        static_cast<float>(renderExtent.width),
        static_cast<float>(renderExtent.height),
        0.0f, 1.0f
    };
    const rhi::Rect2D scissor{{0, 0}, renderExtent};
    context.cmd->setViewport(viewport);
    context.cmd->setScissor(scissor);

    // Bind Forward pipeline
    // BLOCKER: RHI bindPipeline is a stub placeholder, using native Vulkan binding
    const PipelineHandle forwardPipeline = m_renderer->getForwardPipelineHandle();
    if(forwardPipeline.isNull())
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

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(forwardPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

    // Get pipeline layout for descriptor set binding
    // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
        m_renderer->getGBufferPipelineLayout());

    // Bind texture descriptor set (set 0)
    // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
    const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getGBufferColorDescriptorSet());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    // Allocate CameraUniforms from transient allocator
    const uint32_t cameraAlignment = 256;
    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), cameraAlignment);

    // Setup camera matrices
    shaderio::CameraUniforms cameraData{};
    if(context.params->cameraUniforms != nullptr)
    {
        cameraData = *context.params->cameraUniforms;
    }
    else
    {
        cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        cameraData.projection = glm::perspective(glm::radians(45.0f),
            static_cast<float>(renderExtent.width) / static_cast<float>(renderExtent.height), 0.1f, 100.0f);
        cameraData.projection[1][1] *= -1.0f;
        cameraData.viewProjection = cameraData.projection * cameraData.view;
        cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
    }

    std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
    context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

    // Get camera descriptor set (per-frame, uses dynamic offsets)
    // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    if(!cameraBindGroupHandle.isNull())
    {
        uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
        VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);

        // Bind camera descriptor set (set 1) with dynamic offset
        const uint32_t cameraDynamicOffset = cameraAlloc.offset;
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
    }

    // Get draw descriptor set (per-frame)
    const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);

    // Render transparent meshes (sorted back to front)
    for(const auto& [meshIndex, distance] : transparentMeshes)
    {
        MeshHandle meshHandle = context.gltfModel->meshes[meshIndex];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr) continue;

        // Allocate DrawUniforms
        const uint32_t drawAlignment = 256;
        const TransientAllocator::Allocation drawAlloc =
            context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);

        // Setup draw uniforms
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
        drawData.alphaMode = shaderio::LAlphaBlend;
        drawData.alphaCutoff = 0.5f;

        if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
        {
            MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
            drawData.baseColorFactor = m_renderer->getMaterialBaseColorFactor(matHandle);

            auto texIndices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
            drawData.baseColorTextureIndex = texIndices.baseColor;
            drawData.normalTextureIndex = texIndices.normal;
            drawData.metallicRoughnessTextureIndex = texIndices.metallicRoughness;
            drawData.occlusionTextureIndex = texIndices.occlusion;
            drawData.metallicFactor = texIndices.metallicFactor;
            drawData.roughnessFactor = texIndices.roughnessFactor;
            drawData.normalScale = texIndices.normalScale;
            drawData.alphaMode = texIndices.alphaMode;
            drawData.alphaCutoff = texIndices.alphaCutoff;
        }

        std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
        context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

        // Bind draw descriptor set (set 2) with dynamic offset
        // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
        if(!drawBindGroupHandle.isNull())
        {
            uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
            VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);
            const uint32_t drawDynamicOffset = drawAlloc.offset;
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);
        }

        // Bind vertex and index buffers using RHI interface with opaque uint64_t handles
        const uint64_t vertexHandle = mesh->vertexBufferHandle;
        const uint64_t vertexOffset = 0;
        context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);

        const uint64_t indexHandle = mesh->indexBufferHandle;
        context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);

        // Draw indexed using RHI interface
        context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
    }

    // End render pass using RHI interface
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
