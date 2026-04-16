#include "LightPass.h"
#include "../Renderer.h"
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

    context.cmd->beginEvent("LightPass");

    // Get output texture view and fixed extent
    rhi::TextureViewHandle outputViewHandle = rhi::TextureViewHandle::fromNative(
        m_renderer->getOutputTextureView());
    const VkExtent2D outputExtent = m_renderer->getSceneResources().getSize();
    const rhi::Extent2D extent = {outputExtent.width, outputExtent.height};

    if(outputViewHandle.isNull())
    {
        context.cmd->endEvent();
        return;
    }

    rhi::RenderTargetDesc colorTarget = {
        .texture = {},  // Not used when view carries native pointer
        .view = outputViewHandle,
        .state = rhi::ResourceState::ColorAttachment,
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
        context.cmd->endEvent();
        return;
    }

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(lightPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
        m_renderer->getLightPipelineLayout());

    const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getGBufferTextureDescriptorSet());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
    shaderio::CameraUniforms cameraData =
        context.params != nullptr && context.params->cameraUniforms != nullptr
            ? *context.params->cameraUniforms
            : m_renderer->getShadowCameraUniforms();
    std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
    context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    if(!cameraBindGroupHandle.isNull())
    {
      VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      const uint32_t cameraDynamicOffset = cameraAlloc.offset;
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout,
                              shaderio::LSetScene,
                              1,
                              &cameraDescriptorSet,
                              1,
                              &cameraDynamicOffset);
    }

    // Draw fullscreen triangle using RHI interface
    context.cmd->draw(3, 1, 0, 0);

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

    context.cmd->endEvent();
}

}  // namespace demo
