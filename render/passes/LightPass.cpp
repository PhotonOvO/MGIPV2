#include "LightPass.h"
#include "../Renderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanPipelines.h"

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

    // Get swapchain extent
    const VkExtent2D extent = m_renderer->getSwapchainExtent();
    const VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const VkRect2D scissor{{0, 0}, extent};

    // Use textured pipeline as placeholder for light pass
    // (Full implementation would use a dedicated light pipeline)
    const PipelineHandle lightPipeline = m_renderer->getGraphicsPipelineHandle(Renderer::GraphicsPipelineVariant::textured);

    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);

    // Draw fullscreen triangle
    rhi::vulkan::cmdDraw(*context.cmd, 3, 1, 0, 0);

    context.cmd->endEvent();
}

}  // namespace demo