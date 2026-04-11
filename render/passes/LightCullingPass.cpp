#include "LightCullingPass.h"
#include "../Renderer.h"
#include "../LightResources.h"
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>

namespace demo {

LightCullingPass::LightCullingPass(Renderer* renderer)
    : m_renderer(renderer)
{}

PassNode::HandleSlice<PassResourceDependency> LightCullingPass::getDependencies() const
{
    // Depends on depth buffer from GBufferPass (read-only for culling)
    static const std::array<PassResourceDependency, 1> deps = {
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::read,
            rhi::ShaderStage::compute
        ),
    };
    return {deps.data(), static_cast<uint32_t>(deps.size())};
}

void LightCullingPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr)
        return;

    context.cmd->beginEvent("LightCulling");

    // Get native command buffer for compute dispatch
    VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

    // Calculate dispatch dimensions based on screen size
    const VkExtent2D extent = m_renderer->getSceneResources().getSize();
    const uint32_t tileCountX = (extent.width + 15) / 16;  // TILE_SIZE_X = 16
    const uint32_t tileCountY = (extent.height + 15) / 16; // TILE_SIZE_Y = 16

    // TODO: Bind compute pipeline and descriptor sets when pipeline is created
    // This pass currently sets up the structure - pipeline creation happens in Renderer init
    //
    // vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    // vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, firstSet, count, sets, 0, nullptr);
    // vkCmdDispatch(vkCmd, tileCountX, tileCountY, 1);

    context.cmd->endEvent();
}

}  // namespace demo