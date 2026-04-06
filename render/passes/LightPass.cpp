#include "LightPass.h"
#include "../Renderer.h"
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>

namespace demo {

LightPass::LightPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> LightPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassOutputHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void LightPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr)
        return;

    context.cmd->beginEvent("LightPass");

    // Get output texture view and fixed extent
    rhi::TextureViewHandle outputViewHandle = rhi::TextureViewHandle::fromNative(
        m_renderer->getOutputTextureView());
    const rhi::Extent2D extent = {
        SceneResources::kOutputTextureWidth,
        SceneResources::kOutputTextureHeight
    };

    if(outputViewHandle.isNull())
    {
        context.cmd->endEvent();
        return;
    }

    // Setup color attachment for dynamic rendering
    rhi::RenderTargetDesc colorTarget = {
        .texture = {},  // Not used when view carries native pointer
        .view = outputViewHandle,
        .state = rhi::ResourceState::general,
        .loadOp = rhi::LoadOp::clear,  // Clear output texture
        .storeOp = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},  // Black background
    };

    // Begin render pass using RHI interface
    const rhi::RenderPassDesc passDesc = {
        .renderArea = {{0, 0}, extent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = nullptr,
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

    // Bind light pipeline
    // BLOCKER: RHI bindPipeline is a stub placeholder, using native Vulkan binding
    const PipelineHandle lightPipeline = m_renderer->getLightPipelineHandle();
    if(lightPipeline.isNull())
    {
        context.cmd->endRenderPass();
        context.cmd->endEvent();
        return;
    }

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(lightPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

    // Get pipeline layout for descriptor binding
    // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
        m_renderer->getLightPipelineLayout());

    // Bind GBuffer texture descriptor set (set 0)
    // BLOCKER: RHI bindBindGroup is a stub placeholder, using native Vulkan binding
    const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getGBufferTextureDescriptorSet());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    // Setup light parameters via push constants
    shaderio::LightParams lightParams;
    // Direction TO the light source (sun direction, pointing towards the scene)
    lightParams.lightDirection = glm::normalize(glm::vec3(0.5f, 0.8f, 0.3f));
    // Light color (warm sunlight)
    lightParams.lightColor = glm::vec3(1.0f, 0.95f, 0.85f) * 3.0f;  // Bright enough
    // Ambient color (sky ambient)
    lightParams.ambientColor = glm::vec3(0.1f, 0.12f, 0.15f);

    // Push constants for light parameters
    vkCmdPushConstants(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                       pipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(shaderio::LightParams),
                       &lightParams);

    // Draw fullscreen triangle using RHI interface
    context.cmd->draw(3, 1, 0, 0);

    // End render pass using RHI interface
    context.cmd->endRenderPass();

    context.cmd->endEvent();
}

}  // namespace demo