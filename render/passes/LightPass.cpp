#include "LightPass.h"
#include "../Renderer.h"
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanPipelines.h"
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
    const VkImageView swapchainImageView = m_renderer->getCurrentSwapchainImageView();
    const VkExtent2D extent = m_renderer->getSwapchainExtent();

    if(swapchainImageView == VK_NULL_HANDLE)
    {
        context.cmd->endEvent();
        return;
    }

    // Setup color attachment for dynamic rendering
    const VkRenderingAttachmentInfo colorAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = swapchainImageView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Preserve previous content
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };

    // Begin dynamic rendering (INDEPENDENT render pass)
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttachment,
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

    // Bind light pipeline
    const PipelineHandle lightPipeline = m_renderer->getLightPipelineHandle();
    if(lightPipeline.isNull())
    {
        rhi::vulkan::cmdEndRendering(*context.cmd);
        context.cmd->endEvent();
        return;
    }

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(lightPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

    // Get pipeline layout for descriptor binding
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
        m_renderer->getLightPipelineLayout());

    // Bind GBuffer texture descriptor set (set 0)
    const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getGBufferTextureDescriptorSet());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    // Draw fullscreen triangle
    rhi::vulkan::cmdDraw(*context.cmd, 3, 1, 0, 0);

    // End dynamic rendering
    rhi::vulkan::cmdEndRendering(*context.cmd);

    context.cmd->endEvent();
}

}  // namespace demo