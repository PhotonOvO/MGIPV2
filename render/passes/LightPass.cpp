#include "LightPass.h"
#include "../Renderer.h"
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"  // BLOCKER: Needed for native Vulkan pipeline/descriptor binding until RHI bindPipeline/bindBindGroup work
#include "../../shaders/shader_io.h"

#include <array>

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
            kPassSwapchainHandle,
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

    // Get swapchain image view and extent
    rhi::TextureViewHandle swapchainViewHandle = rhi::TextureViewHandle::fromNative(
        m_renderer->getCurrentSwapchainImageView());
    const rhi::Extent2D extent = {
        m_renderer->getSwapchainExtent().width,
        m_renderer->getSwapchainExtent().height
    };

    if(swapchainViewHandle.isNull())
    {
        context.cmd->endEvent();
        return;
    }

    // Setup color attachment for dynamic rendering
    rhi::RenderTargetDesc colorTarget = {
        .texture = {},  // Not used when view carries native pointer
        .view = swapchainViewHandle,
        .state = rhi::ResourceState::general,
        .loadOp = rhi::LoadOp::load,  // Preserve previous content
        .storeOp = rhi::StoreOp::store,
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

    // Draw fullscreen triangle using RHI interface
    context.cmd->draw(3, 1, 0, 0);

    // End render pass using RHI interface
    context.cmd->endRenderPass();

    context.cmd->endEvent();
}

}  // namespace demo