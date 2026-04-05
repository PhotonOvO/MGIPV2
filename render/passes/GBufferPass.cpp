#include "GBufferPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanDescriptor.h"

#include <array>
#include <cstring>

namespace demo {

GBufferPass::GBufferPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GBufferPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 3> dependencies = {
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
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
    const VkExtent2D extent = sceneResources.getSize();

    // Setup MRT color attachments (GBuffer0/1/2)
    std::array<VkRenderingAttachmentInfo, 3> colorAttachments{};
    for(uint32_t i = 0; i < 3; ++i)
    {
        colorAttachments[i] = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = sceneResources.getGBufferImageView(i),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue  = {{{0.0f, 0.0f, 0.0f, 0.0f}}},
        };
    }

    // Setup depth attachment
    const VkRenderingAttachmentInfo depthAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = sceneResources.getDepthImageView(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = {{{1.0f, 0}}},
    };

    // Begin dynamic rendering with MRT
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size()),
        .pColorAttachments    = colorAttachments.data(),
        .pDepthAttachment     = &depthAttachment,
    };
    rhi::vulkan::cmdBeginRendering(*context.cmd, renderingInfo);

    // Set viewport and scissor
    const VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const VkRect2D scissor{{0, 0}, extent};
    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);

    // Only render if glTF model is loaded
    if(context.gltfModel != nullptr && !context.gltfModel->meshes.empty() && context.drawStream != nullptr)
    {
        // Bind GBuffer pipeline
        const PipelineHandle gbufferPipeline = m_renderer->getGBufferPipelineHandle();
        if(!gbufferPipeline.isNull())
        {
            const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
                m_renderer->getPipelineOpaque(gbufferPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
            rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

            // Get pipeline layout for descriptor set binding
            const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                m_renderer->getGBufferPipelineLayout());

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

            // Get camera and draw descriptor sets
            const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup();
            const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup();

            // Update camera descriptor to point to the buffer
            if(!cameraBindGroupHandle.isNull())
            {
                // Get the camera descriptor set
                uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
                VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);

                VkDescriptorBufferInfo bufferInfo{
                    .buffer = reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                    .offset = cameraAlloc.offset,
                    .range  = sizeof(shaderio::CameraUniforms),
                };
                VkWriteDescriptorSet write{
                    .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet          = cameraDescriptorSet,
                    .dstBinding      = shaderio::LBindCamera,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .pBufferInfo     = &bufferInfo,
                };
                vkUpdateDescriptorSets(reinterpret_cast<VkDevice>(m_renderer->getDeviceOpaque()), 1, &write, 0, nullptr);
            }

            // Bind texture descriptor set (set 0)
            const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
                m_renderer->getGBufferColorDescriptorSet());
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

            // Bind camera descriptor set (set 1)
            if(!cameraBindGroupHandle.isNull())
            {
                uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
                VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);
                vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                        VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                        shaderio::LSetScene, 1, &cameraDescriptorSet, 0, nullptr);
            }

            for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
            {
                MeshHandle meshHandle = context.gltfModel->meshes[i];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr)
                {
                    continue;
                }

                // Allocate DrawUniforms for this draw call
                const uint32_t drawAlignment = 256;
                const TransientAllocator::Allocation drawAlloc =
                    context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);

                // Setup draw uniforms
                shaderio::DrawUniforms drawData{};
                drawData.modelMatrix = mesh->transform;
                drawData.baseColorFactor = glm::vec4(1.0f);
                drawData.materialIndex = 0;

                // Write draw data to buffer
                std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
                context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

                // Update draw descriptor to point to the buffer
                if(!drawBindGroupHandle.isNull())
                {
                    uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
                    VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);

                    VkDescriptorBufferInfo bufferInfo{
                        .buffer = reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                        .offset = drawAlloc.offset,
                        .range  = sizeof(shaderio::DrawUniforms),
                    };
                    VkWriteDescriptorSet write{
                        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet          = drawDescriptorSet,
                        .dstBinding      = shaderio::LBindDrawModel,
                        .dstArrayElement = 0,
                        .descriptorCount = 1,
                        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .pBufferInfo     = &bufferInfo,
                    };
                    vkUpdateDescriptorSets(reinterpret_cast<VkDevice>(m_renderer->getDeviceOpaque()), 1, &write, 0, nullptr);

                    // Bind draw descriptor set (set 2)
                    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                            shaderio::LSetDraw, 1, &drawDescriptorSet, 0, nullptr);
                }

                // Bind vertex and index buffers
                const VkDeviceSize vertexOffset = 0;
                rhi::vulkan::cmdBindVertexBuffers(*context.cmd, 0, 1, &mesh->vertexBuffer, &vertexOffset);
                rhi::vulkan::cmdBindIndexBuffer(*context.cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                // Draw indexed
                rhi::vulkan::cmdDrawIndexed(*context.cmd, mesh->indexCount, 1, 0, 0, 0);
            }
        }
    }
    // No fallback triangle - if no model, only clear

    rhi::vulkan::cmdEndRendering(*context.cmd);

    context.cmd->endEvent();
}

}  // namespace demo