#include "GBufferPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../ClipSpaceConvention.h"
#include "../../common/TracyProfiling.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work

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
            ResourceAccess::read,
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
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = rhi::TextureHandle{static_cast<uint32_t>(kPassGBuffer0Handle.index + i), kPassGBuffer0Handle.generation},
            .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getColorImage(i)),
            .aspect      = rhi::TextureAspect::color,
            .srcStage    = rhi::PipelineStage::FragmentShader,
            .dstStage    = rhi::PipelineStage::FragmentShader,
            .srcAccess   = rhi::ResourceAccess::read,
            .dstAccess   = rhi::ResourceAccess::write,
            .oldState    = rhi::ResourceState::General,
            .newState    = rhi::ResourceState::ColorAttachment,
            .isSwapchain = false,
        });

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
        .loadOp = rhi::LoadOp::load,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {0.0f, 0},
    };

    context.cmd->transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getDepthImage()),
        .aspect      = sceneDepthAspect(sceneResources.getDepthFormat()),
        .srcStage    = rhi::PipelineStage::Compute,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::read,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::General,
        .newState    = rhi::ResourceState::DepthStencilAttachment,
        .isSwapchain = false,
    });

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
        const uint64_t indirectBufferHandle = m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
        const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

        // Use shared camera allocation from PassContext (allocated once per frame by Renderer)
        if(!context.cameraAllocValid)
        {
            context.cmd->endRenderPass();
            context.cmd->endEvent();
            return;
        }
        const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

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

        // First pass: collect visible meshes and compute DrawUniforms
        // Use pre-built mesh lists to avoid full mesh scan
        std::vector<PendingDraw> pendingDraws;
        {
            TRACY_ZONE_SCOPED("GBuffer::collectMeshes");
            // Process opaque meshes
            const PipelineHandle opaquePipeline = m_renderer->getGBufferOpaquePipelineHandle();
            for(size_t idx : context.gltfModel->opaqueMeshIndices)
            {
                MeshHandle meshHandle = context.gltfModel->meshes[idx];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr)
                {
                    continue;
                }

                // Compute DrawUniforms using cached material data from mesh
                shaderio::DrawUniforms drawData{};
                drawData.modelMatrix = mesh->transform;
                drawData.alphaMode = shaderio::LAlphaOpaque;
                drawData.alphaCutoff = mesh->alphaCutoff;
                drawData.baseColorFactor = mesh->baseColorFactor;
                drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
                drawData.normalTextureIndex = mesh->normalTextureIndex;
                drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
                drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
                drawData.metallicFactor = mesh->metallicFactor;
                drawData.roughnessFactor = mesh->roughnessFactor;
                drawData.normalScale = mesh->normalScale;

                pendingDraws.push_back({idx, mesh, drawData, opaquePipeline});
            }

            // Process alpha-test meshes
            const PipelineHandle alphaTestPipeline = m_renderer->getGBufferAlphaTestPipelineHandle();
            for(size_t idx : context.gltfModel->alphaTestMeshIndices)
            {
                MeshHandle meshHandle = context.gltfModel->meshes[idx];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr)
                {
                    continue;
                }

                // Compute DrawUniforms using cached material data from mesh
                shaderio::DrawUniforms drawData{};
                drawData.modelMatrix = mesh->transform;
                drawData.alphaMode = shaderio::LAlphaMask;
                drawData.alphaCutoff = mesh->alphaCutoff;
                drawData.baseColorFactor = mesh->baseColorFactor;
                drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
                drawData.normalTextureIndex = mesh->normalTextureIndex;
                drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
                drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
                drawData.metallicFactor = mesh->metallicFactor;
                drawData.roughnessFactor = mesh->roughnessFactor;
                drawData.normalScale = mesh->normalScale;

                pendingDraws.push_back({idx, mesh, drawData, alphaTestPipeline});
            }
        }

        // Batch allocate DrawUniforms for all visible meshes
        if(!pendingDraws.empty() && !drawBindGroupHandle.isNull())
        {
            const uint32_t batchSize = static_cast<uint32_t>(pendingDraws.size()) * kDrawUniformsStride;
            const TransientAllocator::Allocation batchAlloc =
                context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

            {
                TRACY_ZONE_SCOPED("GBuffer::batchUpload");
                // Write all DrawUniforms at once
                for(size_t slot = 0; slot < pendingDraws.size(); ++slot)
                {
                    std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + slot * kDrawUniformsStride;
                    std::memcpy(dst, &pendingDraws[slot].uniforms, sizeof(shaderio::DrawUniforms));
                }
                context.transientAllocator->flushAllocation(batchAlloc, batchSize);
            }

            // Second pass: bind pipeline/descriptors and draw
            const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
                m_renderer->getGBufferPipelineLayout());
            uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
            VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);

            {
                TRACY_ZONE_SCOPED("GBuffer::drawLoop");
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
                    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                            shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);

                    // Bind vertex and index buffers
                    const uint64_t vertexHandle = draw.mesh->vertexBufferHandle;
                    const uint64_t vertexOffset = 0;
                    context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);

                    const uint64_t indexHandle = draw.mesh->indexBufferHandle;
                    context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);

                    if(indirectBufferHandle != 0)
                    {
                        context.cmd->drawIndexedIndirect(indirectBufferHandle,
                                                         static_cast<uint64_t>(draw.meshIndex) * indirectCommandStride,
                                                         1,
                                                         indirectCommandStride);
                    }
                    else
                    {
                        context.cmd->drawIndexed(draw.mesh->indexCount, 1, 0, 0, 0);
                    }
                }
            }
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
        .aspect      = sceneDepthAspect(sceneResources.getDepthFormat()),
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
