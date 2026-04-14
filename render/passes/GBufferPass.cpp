#include "GBufferPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work

#include <array>
#include <cstring>

namespace demo {

GBufferPass::GBufferPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GBufferPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 5> dependencies = {
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
        PassResourceDependency::texture(
            kPassGBuffer0Handle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment,
            rhi::ResourceState::ColorAttachment
        ),
        PassResourceDependency::texture(
            kPassGBuffer1Handle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment,
            rhi::ResourceState::ColorAttachment
        ),
        PassResourceDependency::texture(
            kPassGBuffer2Handle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment,
            rhi::ResourceState::ColorAttachment
        ),
        PassResourceDependency::texture(
            kPassSceneDepthHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment,
            rhi::ResourceState::DepthStencilAttachment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GBufferPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
    {
        return;
    }

    context.cmd->beginEvent("GBufferPass");

    // Get SceneResources for GBuffer attachments
    SceneResources& sceneResources = m_renderer->getSceneResources();
    const rhi::Extent2D extent = {
        sceneResources.getSize().width,
        sceneResources.getSize().height
    };

    // Setup MRT color attachments (GBuffer0/1/2) using RHI RenderTargetDesc
    std::array<rhi::RenderTargetDesc, 3> colorTargets{};
    for(uint32_t i = 0; i < 3; ++i)
    {
        colorTargets[i] = {
            .texture = {},  // Not used when view carries native pointer
            .view = rhi::TextureViewHandle::fromNative(sceneResources.getGBufferImageView(i)),
            .state = rhi::ResourceState::ColorAttachment,
            .loadOp = rhi::LoadOp::clear,
            .storeOp = rhi::StoreOp::store,
            .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
        };
    }

    // Setup depth attachment using RHI DepthTargetDesc
    const rhi::DepthTargetDesc depthTarget{
        .texture = {},  // Not used when view carries native pointer
        .view = rhi::TextureViewHandle::fromNative(sceneResources.getDepthImageView()),
        .state = rhi::ResourceState::DepthStencilAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {1.0f, 0},
    };

    // Begin render pass using RHI interface
    const rhi::RenderPassDesc passDesc = {
        .renderArea = {{0, 0}, extent},
        .colorTargets = colorTargets.data(),
        .colorTargetCount = static_cast<uint32_t>(colorTargets.size()),
        .depthTarget = &depthTarget,
    };
    context.cmd->beginRenderPass(passDesc);

    // Set viewport and scissor using RHI interface
    const rhi::Viewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const rhi::Rect2D scissor{{0, 0}, extent};
    context.cmd->setViewport(viewport);
    context.cmd->setScissor(scissor);

    // Only render if glTF model is loaded
    if(context.gltfModel != nullptr && !context.gltfModel->meshes.empty() && context.drawStream != nullptr)
    {
        MeshPool& meshPool = m_renderer->getMeshPool();

        // Allocate CameraUniforms from transient allocator
        const uint32_t cameraAlignment = 256;  // Standard UBO alignment
        const TransientAllocator::Allocation cameraAlloc =
            context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), cameraAlignment);

        // Setup camera matrices from params or fallback
        shaderio::CameraUniforms cameraData{};
        if(context.params->cameraUniforms != nullptr)
        {
            cameraData = *context.params->cameraUniforms;
        }
        else
        {
            // Fallback: default camera
            cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            cameraData.projection = glm::perspective(glm::radians(45.0f),
                static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f);
            // Flip Y for Vulkan NDC (Y-axis points down in Vulkan clip space)
            cameraData.projection[1][1] *= -1.0f;
            cameraData.viewProjection = cameraData.projection * cameraData.view;
            cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
        }

        // Write camera data to buffer
        std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
        context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

        // Get camera and draw descriptor sets (per-frame)
        const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
        const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);

        // Bind global bindless texture group (set 0) if available
        // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
        if(!context.globalBindlessGroup.isNull())
        {
            const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                m_renderer->getGBufferPipelineLayout());
            const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
                m_renderer->getGBufferColorDescriptorSet());
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetTextures, 1, &textureSet, 0, nullptr);
        }

        // Bind camera descriptor set (set 1) with dynamic offset
        // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
        if(!cameraBindGroupHandle.isNull())
        {
            const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                m_renderer->getGBufferPipelineLayout());
            uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
            VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);
            const uint32_t cameraDynamicOffset = cameraAlloc.offset;
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
        }

        // Track current pipeline to avoid unnecessary rebinds
        PipelineHandle currentPipeline{};

        for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
        {
            MeshHandle meshHandle = context.gltfModel->meshes[i];
            const MeshRecord* mesh = meshPool.tryGet(meshHandle);
            if(mesh == nullptr)
            {
                continue;
            }

            // Check material alpha mode
            int32_t alphaMode = shaderio::LAlphaOpaque;
            if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
            {
                MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
                auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
                alphaMode = indices.alphaMode;

                // Skip transparent meshes (handled by ForwardPass)
                if(alphaMode == shaderio::LAlphaBlend)
                {
                    continue;
                }
            }

            // Select pipeline variant based on alpha mode
            PipelineHandle gbufferPipeline;
            if(alphaMode == shaderio::LAlphaMask)
            {
                gbufferPipeline = m_renderer->getGBufferAlphaTestPipelineHandle();
            }
            else
            {
                gbufferPipeline = m_renderer->getGBufferOpaquePipelineHandle();
            }

            // Bind pipeline if changed
            // BLOCKER: RHI bindPipeline is a stub placeholder, using native Vulkan binding
            if(gbufferPipeline != currentPipeline && !gbufferPipeline.isNull())
            {
                const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
                    m_renderer->getPipelineOpaque(gbufferPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
                rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
                currentPipeline = gbufferPipeline;
            }

            // Allocate DrawUniforms for this draw call
            const uint32_t drawAlignment = 256;
            const TransientAllocator::Allocation drawAlloc =
                context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);

            // Setup draw uniforms with material data
            shaderio::DrawUniforms drawData{};
            drawData.modelMatrix = mesh->transform;

            // Get material data
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
                MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
                drawData.baseColorFactor = m_renderer->getMaterialBaseColorFactor(matHandle);

                // Get all texture indices at once
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

            // Write draw data to buffer
            std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
            context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

            // Bind draw descriptor set (set 2) with dynamic offset
            // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
            if(!drawBindGroupHandle.isNull())
            {
                const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                    m_renderer->getGBufferPipelineLayout());
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
    }
    // No fallback triangle - if no model, only clear

    // End render pass using RHI interface
    context.cmd->endRenderPass();

    const std::array<std::pair<TextureHandle, VkImage>, 3> colorImages{{
        {kPassGBuffer0Handle, sceneResources.getColorImage(0)},
        {kPassGBuffer1Handle, sceneResources.getColorImage(1)},
        {kPassGBuffer2Handle, sceneResources.getColorImage(2)},
    }};
    for(const auto& [handle, image] : colorImages)
    {
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = rhi::TextureHandle{handle.index, handle.generation},
            .nativeImage = reinterpret_cast<uint64_t>(image),
            .aspect      = rhi::TextureAspect::color,
            .srcStage    = rhi::PipelineStage::FragmentShader,
            .dstStage    = rhi::PipelineStage::FragmentShader,
            .srcAccess   = rhi::ResourceAccess::write,
            .dstAccess   = rhi::ResourceAccess::read,
            .oldState    = rhi::ResourceState::ColorAttachment,
            .newState    = rhi::ResourceState::General,
            .isSwapchain = false,
        });
    }

    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getDepthImage()),
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
