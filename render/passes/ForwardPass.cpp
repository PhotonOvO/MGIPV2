#include "ForwardPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../ClipSpaceConvention.h"
#include "../../common/TracyProfiling.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace demo {

namespace {

// Dynamic UBO alignment requirement (minUniformBufferOffsetAlignment)
constexpr uint32_t kDrawUniformsStride = 256;

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

ForwardPass::ForwardPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ForwardPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassOutputHandle,
            ResourceAccess::readWrite,  // Read existing LightPass output, write blended result
            rhi::ShaderStage::fragment
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

void ForwardPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
    {
        return;
    }

    context.cmd->beginEvent("ForwardPass");

    SceneResources& sceneResources = m_renderer->getSceneResources();
    const auto restoreDepthForSampling = [&]() {
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = rhi::TextureHandle{kPassSceneDepthHandle.index, kPassSceneDepthHandle.generation},
            .nativeImage = reinterpret_cast<uint64_t>(sceneResources.getDepthImage()),
            .aspect      = sceneDepthAspect(sceneResources.getDepthFormat()),
            .srcStage    = rhi::PipelineStage::FragmentShader,
            .dstStage    = rhi::PipelineStage::FragmentShader,
            .srcAccess   = rhi::ResourceAccess::read,
            .dstAccess   = rhi::ResourceAccess::read,
            .oldState    = rhi::ResourceState::DepthStencilAttachment,
            .newState    = rhi::ResourceState::General,
            .isSwapchain = false,
        });
    };

    // Get output texture view and extent (follows screen size)
    const VkImageView outputImageView = m_renderer->getOutputTextureView();
    const VkExtent2D vkExtent = sceneResources.getSize();
    const rhi::Extent2D outputExtent = {vkExtent.width, vkExtent.height};

    if(outputImageView == VK_NULL_HANDLE)
    {
        restoreDepthForSampling();
        context.cmd->endEvent();
        return;
    }

    // Check if we have transparent meshes to render
    if(context.gltfModel == nullptr || context.gltfModel->meshes.empty())
    {
        restoreDepthForSampling();
        context.cmd->endEvent();
        return;
    }

    MeshPool& meshPool = m_renderer->getMeshPool();
    const uint64_t indirectBufferHandle = m_renderer->getGPUCullingIndirectBufferOpaque(context.frameIndex);
    const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

    // Use output texture extent for rendering
    const rhi::Extent2D renderExtent = outputExtent;
    if(renderExtent.width == 0 || renderExtent.height == 0)
    {
        restoreDepthForSampling();
        context.cmd->endEvent();
        return;
    }

    // Use pre-built transparent mesh list with cached sorting
    glm::vec3 cameraPos(0.0f);
    if(context.params->cameraUniforms != nullptr)
    {
        cameraPos = context.params->cameraUniforms->cameraPosition;
    }

    // Check if sorting needs update (camera moved significantly)
    const float cameraMoveThreshold = 0.5f;  // meters
    const float cameraDelta = glm::length(cameraPos - context.gltfModel->lastSortCameraPos);

    GltfUploadResult* mutableGltfModel = const_cast<GltfUploadResult*>(context.gltfModel);
    {
        TRACY_ZONE_SCOPED("ForwardPass::transparentSorting");
        if(context.gltfModel->transparentSortDirty || cameraDelta > cameraMoveThreshold)
        {
            // Update distances
            mutableGltfModel->lastSortCameraPos = cameraPos;
            for(size_t slot = 0; slot < context.gltfModel->transparentMeshIndices.size(); ++slot)
            {
                size_t meshIdx = context.gltfModel->transparentMeshIndices[slot];
                MeshHandle meshHandle = context.gltfModel->meshes[meshIdx];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr)
                {
                    mutableGltfModel->transparentDistances[slot] = std::numeric_limits<float>::max();
                    continue;
                }
                glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
                mutableGltfModel->transparentDistances[slot] = glm::length(meshCenter - cameraPos);
            }

            // Re-sort indices by distance (far to near)
            std::vector<size_t> sortedIndices = context.gltfModel->transparentMeshIndices;
            std::sort(sortedIndices.begin(), sortedIndices.end(),
                [&](size_t a, size_t b) {
                    size_t slotA = std::find(context.gltfModel->transparentMeshIndices.begin(),
                                             context.gltfModel->transparentMeshIndices.end(), a) -
                           context.gltfModel->transparentMeshIndices.begin();
                    size_t slotB = std::find(context.gltfModel->transparentMeshIndices.begin(),
                                             context.gltfModel->transparentMeshIndices.end(), b) -
                           context.gltfModel->transparentMeshIndices.begin();
                    return context.gltfModel->transparentDistances[slotA] >
                           context.gltfModel->transparentDistances[slotB];
                });

            mutableGltfModel->transparentMeshIndices = sortedIndices;
            mutableGltfModel->transparentSortDirty = false;
        }
    }

    // Use pre-sorted transparent mesh indices directly
    std::vector<std::pair<size_t, float>> transparentMeshes;
    for(size_t slot = 0; slot < context.gltfModel->transparentMeshIndices.size(); ++slot)
    {
        transparentMeshes.push_back({context.gltfModel->transparentMeshIndices[slot],
                                     context.gltfModel->transparentDistances[slot]});
    }

    // No transparent meshes, skip rendering
    if(transparentMeshes.empty())
    {
        restoreDepthForSampling();
        context.cmd->endEvent();
        return;
    }

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

    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
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
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::FragmentShader,
        .srcAccess   = rhi::ResourceAccess::read,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::General,
        .newState    = rhi::ResourceState::DepthStencilAttachment,
        .isSwapchain = false,
    });

    // Begin render pass using RHI interface
    const rhi::RenderPassDesc passDesc = {
        .renderArea = {{0, 0}, renderExtent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = &depthTarget,
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
        restoreDepthForSampling();
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

    // Use shared camera allocation from PassContext (allocated once per frame by Renderer)
    if(!context.cameraAllocValid)
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
        restoreDepthForSampling();
        context.cmd->endEvent();
        return;
    }
    const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

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

    // Batch allocate DrawUniforms for all transparent meshes
    if(!transparentMeshes.empty() && !drawBindGroupHandle.isNull())
    {
        // First pass: compute DrawUniforms for all transparent meshes
        std::vector<shaderio::DrawUniforms> uniformsData;
        std::vector<const MeshRecord*> meshRecords;
        {
            TRACY_ZONE_SCOPED("ForwardPass::collectMeshes");
            uniformsData.reserve(transparentMeshes.size());
            meshRecords.reserve(transparentMeshes.size());

            for(const auto& [meshIndex, distance] : transparentMeshes)
            {
                (void)distance;
                MeshHandle meshHandle = context.gltfModel->meshes[meshIndex];
                const MeshRecord* mesh = meshPool.tryGet(meshHandle);
                if(mesh == nullptr) continue;

                meshRecords.push_back(mesh);

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
                drawData.alphaMode = mesh->alphaMode;
                drawData.alphaCutoff = mesh->alphaCutoff;

                // Use cached material data from mesh (no per-frame lookup)
                drawData.baseColorFactor = mesh->baseColorFactor;
                drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
                drawData.normalTextureIndex = mesh->normalTextureIndex;
                drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
                drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
                drawData.metallicFactor = mesh->metallicFactor;
                drawData.roughnessFactor = mesh->roughnessFactor;
                drawData.normalScale = mesh->normalScale;

                uniformsData.push_back(drawData);
            }
        }

        // Batch allocate
        const uint32_t batchSize = static_cast<uint32_t>(uniformsData.size()) * kDrawUniformsStride;
        const TransientAllocator::Allocation batchAlloc =
            context.transientAllocator->allocate(batchSize, kDrawUniformsStride);

        {
            TRACY_ZONE_SCOPED("ForwardPass::batchUpload");
            // Write all DrawUniforms at once
            for(size_t slot = 0; slot < uniformsData.size(); ++slot)
            {
                std::byte* dst = static_cast<std::byte*>(batchAlloc.cpuPtr) + slot * kDrawUniformsStride;
                std::memcpy(dst, &uniformsData[slot], sizeof(shaderio::DrawUniforms));
            }
            context.transientAllocator->flushAllocation(batchAlloc, batchSize);
        }

        // Second pass: bind and draw
        uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
        VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);

        {
            TRACY_ZONE_SCOPED("ForwardPass::drawLoop");
            for(size_t slot = 0; slot < meshRecords.size(); ++slot)
            {
                const MeshRecord* mesh = meshRecords[slot];
                size_t meshIndex = transparentMeshes[slot].first;

                // Bind draw descriptor set with dynamic offset
                const uint32_t drawDynamicOffset = batchAlloc.offset + static_cast<uint32_t>(slot) * kDrawUniformsStride;
                vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                        VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                        shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);

                // Bind vertex and index buffers
                const uint64_t vertexHandle = mesh->vertexBufferHandle;
                const uint64_t vertexOffset = 0;
                context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);

                const uint64_t indexHandle = mesh->indexBufferHandle;
                context.cmd->bindIndexBuffer(indexHandle, 0, rhi::IndexFormat::uint32);

                if(indirectBufferHandle != 0)
                {
                    context.cmd->drawIndexedIndirect(indirectBufferHandle,
                                                     static_cast<uint64_t>(meshIndex) * indirectCommandStride,
                                                     1,
                                                     indirectCommandStride);
                }
                else
                {
                    context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
                }
            }
        }
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
    restoreDepthForSampling();

    context.cmd->endEvent();
}

}  // namespace demo
