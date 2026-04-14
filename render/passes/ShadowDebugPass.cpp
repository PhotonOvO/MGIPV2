#include "ShadowDebugPass.h"
#include "../Renderer.h"
#include "../ShadowResources.h"
#include "../ClipSpaceConvention.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

namespace demo {

ShadowDebugPass::ShadowDebugPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ShadowDebugPass::getDependencies() const
{
    // No explicit dependencies - runs after LightPass, draws on existing framebuffer
    return {};
}

void ShadowDebugPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr || context.params->cameraUniforms == nullptr)
        return;

    shaderio::ShadowUniforms* shadowData = m_renderer->getShadowUniformsData();
    if(shadowData == nullptr)
        return;

    // Get the active light view-projection matrix
    const glm::mat4& lightViewProj = shadowData->lightViewProjectionMatrix;
    const glm::mat4 invLightViewProj = glm::inverse(lightViewProj);

    const clipspace::ProjectionConvention projectionConvention =
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
    // NDC corners for ortho projection under the active clip-space convention.
    const glm::vec4 ndcCorners[8] = {
        glm::vec4(-1.0f, -1.0f, projectionConvention.ndcNearZ, 1.0f),
        glm::vec4( 1.0f, -1.0f, projectionConvention.ndcNearZ, 1.0f),
        glm::vec4(-1.0f,  1.0f, projectionConvention.ndcNearZ, 1.0f),
        glm::vec4( 1.0f,  1.0f, projectionConvention.ndcNearZ, 1.0f),
        glm::vec4(-1.0f, -1.0f, projectionConvention.ndcFarZ, 1.0f),
        glm::vec4( 1.0f, -1.0f, projectionConvention.ndcFarZ, 1.0f),
        glm::vec4(-1.0f,  1.0f, projectionConvention.ndcFarZ, 1.0f),
        glm::vec4( 1.0f,  1.0f, projectionConvention.ndcFarZ, 1.0f),
    };

    // Transform to world space
    glm::vec3 worldCorners[8];
    for(int i = 0; i < 8; ++i)
    {
        glm::vec4 worldCorner = invLightViewProj * ndcCorners[i];
        if(std::fabs(worldCorner.w) > 0.001f)
            worldCorner /= worldCorner.w;
        worldCorners[i] = glm::vec3(worldCorner);
    }

    // Line indices for frustum wireframe (24 vertices for 12 lines)
    const int lineIndices[24] = {
        // Near plane edges (0-1-3-2-0)
        0, 1, 1, 3, 3, 2, 2, 0,
        // Far plane edges (4-5-7-6-4)
        4, 5, 5, 7, 7, 6, 6, 4,
        // Connecting edges
        0, 4, 1, 5, 2, 6, 3, 7,
    };

    // Colors for each axis direction (in world space)
    const float colorX[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // Red for world X direction
    const float colorY[4] = {1.0f, 1.0f, 0.0f, 1.0f};  // Yellow for world Y direction (vertical)
    const float colorZ[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // Blue for world Z direction

    // Build vertex buffer with world-space direction-based coloring
    std::array<DebugVertex, 24> vertices;
    for(int i = 0; i < 24; i += 2)  // Process pairs of vertices (lines)
    {
        // Get the two endpoints in world space
        const glm::vec3& p0 = worldCorners[lineIndices[i]];
        const glm::vec3& p1 = worldCorners[lineIndices[i + 1]];

        // Compute direction vector in world space
        const glm::vec3 dir = p1 - p0;
        const float absX = std::abs(dir.x);
        const float absY = std::abs(dir.y);
        const float absZ = std::abs(dir.z);

        // Choose color based on dominant axis in world space
        const float* lineColor;
        if(absY >= absX && absY >= absZ)
        {
            lineColor = colorY;  // Vertical direction (world Y axis)
        }
        else if(absX >= absY && absX >= absZ)
        {
            lineColor = colorX;  // Horizontal X direction
        }
        else
        {
            lineColor = colorZ;  // Horizontal Z direction
        }

        // Set both vertices of this line to the same color
        for(int j = 0; j < 2; ++j)
        {
            const int vertexIdx = i + j;
            const glm::vec3& pos = worldCorners[lineIndices[vertexIdx]];
            vertices[vertexIdx].position[0] = pos.x;
            vertices[vertexIdx].position[1] = pos.y;
            vertices[vertexIdx].position[2] = pos.z;
            vertices[vertexIdx].color[0] = lineColor[0];
            vertices[vertexIdx].color[1] = lineColor[1];
            vertices[vertexIdx].color[2] = lineColor[2];
            vertices[vertexIdx].color[3] = lineColor[3];
        }
    }

    // Allocate transient buffer for vertices
    if(context.transientAllocator == nullptr)
        return;

    const uint32_t vertexAlignment = 16;  // 16-byte alignment for vertex buffers
    const TransientAllocator::Allocation vertexAlloc =
        context.transientAllocator->allocate(sizeof(vertices), vertexAlignment);

    // Copy vertex data
    std::memcpy(vertexAlloc.cpuPtr, vertices.data(), sizeof(vertices));
    context.transientAllocator->flushAllocation(vertexAlloc, sizeof(vertices));

    // Get viewport extent for camera projection
    const rhi::Extent2D extent = context.params->viewportSize;

    // Allocate and write camera uniforms
    shaderio::CameraUniforms cameraData{};
    if(context.params->cameraUniforms != nullptr)
    {
        cameraData = *context.params->cameraUniforms;
    }
    else
    {
        // Fallback camera
        cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        cameraData.projection = clipspace::makePerspectiveProjection(
            glm::radians(45.0f), static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f,
            projectionConvention);
        cameraData.viewProjection = cameraData.projection * cameraData.view;
        cameraData.inverseViewProjection = glm::inverse(cameraData.viewProjection);
        cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
    }

    const uint32_t cameraAlignment = 256;  // Dynamic UBO alignment
    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(cameraData), cameraAlignment);
    std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
    context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

    context.cmd->beginEvent("ShadowDebugPass");

    // Use existing color and depth from LightPass output
    const rhi::RenderTargetDesc colorTarget{
        .texture = {},  // Use existing output
        .view = rhi::TextureViewHandle::fromNative(m_renderer->getOutputTextureView()),
        .state = rhi::ResourceState::general,
        .loadOp = rhi::LoadOp::load,  // Load existing content
        .storeOp = rhi::StoreOp::store,
    };

    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
        .view = rhi::TextureViewHandle::fromNative(m_renderer->getDepthTextureView()),
        .state = rhi::ResourceState::General,  // Match existing layout
        .loadOp = rhi::LoadOp::load,  // Load existing depth
        .storeOp = rhi::StoreOp::store,
    };

    const rhi::RenderPassDesc passDesc = {
        .renderArea = {{0, 0}, extent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
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

    // Bind debug line pipeline
    PipelineHandle debugPipeline = m_renderer->getDebugLinePipelineHandle();
    if(!debugPipeline.isNull())
    {
        VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getPipelineOpaque(debugPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
        vkCmdBindPipeline(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                          VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

        // Bind pipeline layout
        VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());

        // Bind camera descriptor set (set LSetScene) with dynamic offset
        const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
        if(!cameraBindGroupHandle.isNull())
        {
            uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
            VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);
            const uint32_t cameraDynamicOffset = cameraAlloc.offset;  // Use actual allocation offset
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
        }

        // Bind vertex buffer
        const uint64_t vertexBufferHandle = context.transientAllocator->getBufferOpaque();
        const uint64_t vertexOffset = vertexAlloc.offset;
        context.cmd->bindVertexBuffers(0, &vertexBufferHandle, &vertexOffset, 1);

        // Draw 12 lines (24 vertices)
        context.cmd->draw(24, 1, 0, 0);
    }

    context.cmd->endRenderPass();
    context.cmd->endEvent();
}

}  // namespace demo
