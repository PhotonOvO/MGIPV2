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
            kPassSceneDepthHandle,
            ResourceAccess::read,
            rhi::ShaderStage::compute
        ),
    };
    return {deps.data(), static_cast<uint32_t>(deps.size())};
}

void LightCullingPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr)
        return;

    context.cmd->beginEvent("LightCulling");

    // Get native command buffer for compute dispatch
    VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

    // Calculate dispatch dimensions based on screen size
    const VkExtent2D extent = m_renderer->getSceneResources().getSize();
    const uint32_t tileCountX = (extent.width + 15) / 16;  // TILE_SIZE_X = 16
    const uint32_t tileCountY = (extent.height + 15) / 16; // TILE_SIZE_Y = 16

    // Bind compute pipeline
    PipelineHandle cullingPipeline = m_renderer->getLightCullingPipelineHandle();
    if(cullingPipeline.isNull())
    {
        context.cmd->endEvent();
        return;
    }

    // Must have descriptor set bound for compute dispatch
    uint64_t descriptorSetOpaque = m_renderer->getLightCullingDescriptorSet();
    if(descriptorSetOpaque == 0)
    {
        // Light culling resources not initialized - skip pass
        context.cmd->endEvent();
        return;
    }

    VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(cullingPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE)));
    vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, nativePipeline);

    VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getLightCullingPipelineLayout());

    // Set 0: pass-local light buffers, depth input, and tile output.
    VkDescriptorSet descriptorSet = reinterpret_cast<VkDescriptorSet>(descriptorSetOpaque);
    vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                             0, 1, &descriptorSet, 0, nullptr);

    // Set 1: scene-level UBOs. Light culling reads LBindLightCulling from the
    // same scene bind group used by graphics passes.
    const BindGroupHandle sceneBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    if(sceneBindGroupHandle.isNull())
    {
        context.cmd->endEvent();
        return;
    }
    VkDescriptorSet sceneDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(sceneBindGroupHandle, BindGroupSetSlot::shaderSpecific));
    const uint32_t cameraDynamicOffset = 0;
    vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            shaderio::LSetScene, 1, &sceneDescriptorSet, 1, &cameraDynamicOffset);

    // Dispatch compute shader
    vkCmdDispatch(vkCmd, tileCountX, tileCountY, 1);

    // Add memory barrier to ensure culling completes before LightPass reads results
    const VkMemoryBarrier2 memoryBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
    };
    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memoryBarrier,
    };
    vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);

    context.cmd->endEvent();
}

}  // namespace demo
