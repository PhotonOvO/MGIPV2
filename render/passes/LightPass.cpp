#include "LightPass.h"
#include "../Renderer.h"
#include "../TransientAllocator.h"
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
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
    static const std::array<PassResourceDependency, 9> dependencies = {
        PassResourceDependency::texture(
            kPassGBuffer0Handle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBuffer1Handle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBuffer2Handle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassSceneDepthHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassCSMShadowHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
        PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
        PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
        PassResourceDependency::texture(
            kPassOutputHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment,
            rhi::ResourceState::ColorAttachment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void LightPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.transientAllocator == nullptr)
        return;

    context.cmd->beginEvent("GPUDrivenLight");

    const rhi::TextureViewHandle outputViewHandle =
        rhi::TextureViewHandle::fromNative(m_renderer->getOutputTextureView());
    const VkExtent2D    outputExtent = m_renderer->getSceneResources().getSize();
    const rhi::Extent2D extent       = {outputExtent.width, outputExtent.height};
    const PipelineHandle lightPipeline = m_renderer->getLightPipelineHandle();

    if(outputViewHandle.isNull() || lightPipeline.isNull() || !context.cameraAllocValid)
    {
        context.cmd->endEvent();
        return;
    }

    // Single source of truth for the output-texture state flip around the pass.
    const auto transitionOutput = [&](rhi::ResourceState from, rhi::ResourceState to,
                                      rhi::ResourceAccess srcAccess, rhi::ResourceAccess dstAccess) {
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
            .nativeImage = reinterpret_cast<uint64_t>(m_renderer->getSceneResources().getOutputTextureImage()),
            .aspect      = rhi::TextureAspect::color,
            .srcStage    = rhi::PipelineStage::FragmentShader,
            .dstStage    = rhi::PipelineStage::FragmentShader,
            .srcAccess   = srcAccess,
            .dstAccess   = dstAccess,
            .oldState    = from,
            .newState    = to,
            .isSwapchain = false,
        });
    };

    transitionOutput(rhi::ResourceState::General, rhi::ResourceState::ColorAttachment,
                     rhi::ResourceAccess::read, rhi::ResourceAccess::write);

    const rhi::RenderTargetDesc colorTarget = {
        .texture    = {},
        .view       = outputViewHandle,
        .state      = rhi::ResourceState::ColorAttachment,
        .loadOp     = rhi::LoadOp::clear,
        .storeOp    = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const rhi::RenderPassDesc passDesc = {
        .renderArea       = {{0, 0}, extent},
        .colorTargets     = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget      = nullptr,
    };
    context.cmd->beginRenderPass(passDesc);

    context.cmd->setViewport({0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    context.cmd->setScissor({{0, 0}, extent});

    // Pipeline + per-frame camera/scene uniforms bind through pure RHI. The
    // resolver maps the handles to native objects and tracks the layout.
    context.cmd->bindPipeline(rhi::PipelineBindPoint::graphics, lightPipeline);

    const uint32_t cameraDynamicOffsets[] = {context.cameraAlloc.offset, 0u};
    context.cmd->bindBindGroup(shaderio::LSetScene, m_renderer->getCameraBindGroup(context.frameIndex),
                               cameraDynamicOffsets, 2);

    // Hidden dependency: the GBuffer input texture set is still an externally
    // managed VkDescriptorSet (Renderer::m_device.gbufferTextureSets), not a
    // BindGroup, so it remains the one native bind in this pass. Promoting it to
    // a BindGroup would make this pass fully native-free.
    const VkDescriptorSet  textureSet     = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferTextureDescriptorSet());
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getLightPipelineLayout());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    context.cmd->draw(3, 1, 0, 0);
    context.cmd->endRenderPass();

    transitionOutput(rhi::ResourceState::ColorAttachment, rhi::ResourceState::General,
                     rhi::ResourceAccess::write, rhi::ResourceAccess::read);

    context.cmd->endEvent();
}

}  // namespace demo
