#include "ShadowPass.h"
#include "../Renderer.h"
#include "../ShadowResources.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work

#include <array>
#include <cstring>

namespace demo {

ShadowPass::ShadowPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ShadowPass::getDependencies() const
{
    // No dependencies - shadow pass renders before GBuffer
    return {};
}

void ShadowPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.gltfModel == nullptr)
        return;

    context.cmd->beginEvent("ShadowPass");

    ShadowResources& shadowResources = m_renderer->getShadowResources();
    const uint32_t cascadeCount = shadowResources.getCascadeCount();
    const uint32_t shadowMapSize = shadowResources.getShadowMapSize();
    const rhi::Extent2D extent{shadowMapSize, shadowMapSize};

    // Render each cascade
    for(uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
    {
        const std::string cascadeName = "Cascade" + std::to_string(cascade);
        context.cmd->beginEvent(cascadeName.c_str());

        VkImageView cascadeView = shadowResources.getCascadeView(cascade);

        // Setup depth-only render pass
        const rhi::DepthTargetDesc depthTarget{
            .texture = {},  // Not used when view carries native pointer
            .view = rhi::TextureViewHandle::fromNative(cascadeView),
            .state = rhi::ResourceState::DepthStencilAttachment,
            .loadOp = rhi::LoadOp::clear,
            .storeOp = rhi::StoreOp::store,
            .clearValue = {1.0f, 0},  // Clear depth to 1.0 (far plane)
        };

        const rhi::RenderPassDesc passDesc = {
            .renderArea = {{0, 0}, extent},
            .colorTargets = nullptr,
            .colorTargetCount = 0,
            .depthTarget = &depthTarget,
        };
        context.cmd->beginRenderPass(passDesc);

        // Set viewport and scissor
        const rhi::Viewport viewport{
            0.0f, 0.0f,
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
            0.0f, 1.0f
        };
        const rhi::Rect2D scissor{{0, 0}, extent};
        context.cmd->setViewport(viewport);
        context.cmd->setScissor(scissor);

        // Only render if glTF model is loaded and draw stream is available
        if(context.gltfModel != nullptr && !context.gltfModel->meshes.empty() && context.transientAllocator != nullptr)
        {
            MeshPool& meshPool = m_renderer->getMeshPool();

            // Allocate DrawUniforms from transient allocator
            const uint32_t drawAlignment = 256;
            const TransientAllocator::Allocation drawAlloc =
                context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);

            // Get draw descriptor set (per-frame)
            const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);

            // Track current pipeline to avoid unnecessary rebinds
            PipelineHandle currentPipeline{};

            // TODO: Bind shadow pipeline when available
            // TODO: Push constants for light view-projection matrix from ShadowUniforms

            for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
            {
                MeshHandle meshHandle = context.gltfModel->meshes[i];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr)
                {
                    continue;
                }

                // Skip transparent meshes (handled by ForwardPass)
                int32_t alphaMode = shaderio::LAlphaOpaque;
                if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
                {
                    MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
                    auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
                    alphaMode = indices.alphaMode;

                    // Skip transparent meshes
                    if(alphaMode == shaderio::LAlphaBlend)
                    {
                        continue;
                    }
                }

                // Setup draw uniforms with model matrix
                shaderio::DrawUniforms drawData{};
                drawData.modelMatrix = mesh->transform;
                drawData.baseColorFactor = glm::vec4(1.0f);
                drawData.alphaMode = alphaMode;
                drawData.alphaCutoff = 0.5f;

                // Write draw data to buffer
                std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
                context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

                // Bind draw descriptor set (set 2) with dynamic offset
                // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
                if(!drawBindGroupHandle.isNull())
                {
                    // TODO: Get shadow pipeline layout when available
                    // For now, use GBuffer pipeline layout as placeholder
                    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                        m_renderer->getGBufferPipelineLayout());
                    uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
                    VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);
                    const uint32_t drawDynamicOffset = drawAlloc.offset;
                    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                            shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);
                }

                // Bind vertex and index buffers using RHI interface
                const uint64_t vertexHandle = mesh->vertexBufferHandle;
                const uint64_t vertexOffset = 0;
                context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);

                const uint64_t indexHandle = mesh->indexBufferHandle;
                context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);

                // Draw indexed
                context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
            }
        }

        context.cmd->endRenderPass();
        context.cmd->endEvent();
    }

    context.cmd->endEvent();
}

}  // namespace demo