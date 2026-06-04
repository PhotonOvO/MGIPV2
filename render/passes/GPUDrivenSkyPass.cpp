#include "GPUDrivenSkyPass.h"
#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo {

GPUDrivenSkyPass::GPUDrivenSkyPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenSkyPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 3> dependencies = {
      PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::readWrite, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSkyPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr)
  {
    return;
  }

  const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
  if(sceneView == nullptr || sceneView->outputImage == VK_NULL_HANDLE || sceneView->outputView.isNull()
     || sceneView->sceneDepthExtent.width == 0 || sceneView->sceneDepthExtent.height == 0)
  {
    return;
  }

  const PipelineHandle skyPipeline = m_renderer->getSkyPipelineHandle();
  if(skyPipeline.isNull())
  {
    return;
  }

  context.cmd->beginEvent("GPUDrivenSkyPass");

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
      .aspect      = rhi::TextureAspect::color,
      .srcStage    = rhi::PipelineStage::FragmentShader,
      .dstStage    = rhi::PipelineStage::FragmentShader,
      .srcAccess   = rhi::ResourceAccess::read,
      .dstAccess   = rhi::ResourceAccess::write,
      .oldState    = rhi::ResourceState::General,
      .newState    = rhi::ResourceState::ColorAttachment,
      .isSwapchain = false,
  });

  rhi::RenderTargetDesc colorTarget{
      .texture = {},
      .view = sceneView->outputView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
  };

  const rhi::Extent2D extent{sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};
  rhi::RenderEncoder* enc = context.cmdBuffer->beginRenderPass(rhi::RenderPassDesc{
      .renderArea = {{0, 0}, extent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
      .depthTarget = nullptr,
  });
  enc->setViewport(
      rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  enc->setScissor(rhi::Rect2D{{0, 0}, extent});

  enc->setPipeline(skyPipeline);
  const BindGroupHandle inputBindGroup = m_renderer->getLightingInputBindGroup(context.frameIndex);
  enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
                        rhi::ArgumentTableHandle{inputBindGroup.index, inputBindGroup.generation});  // bridge (Wave 8)

  if(context.cameraAllocValid)
  {
    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    if(!cameraBindGroupHandle.isNull())
    {
      enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, shaderio::LSetScene, {}, context.cameraAlloc.offset, 0);
      enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetScene,
                            rhi::ArgumentTableHandle{cameraBindGroupHandle.index, cameraBindGroupHandle.generation});  // bridge (Wave 8)
    }
  }

  enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
  context.cmdBuffer->endEncoding();

  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
      .texture     = rhi::TextureHandle{kPassOutputHandle.index, kPassOutputHandle.generation},
      .nativeImage = reinterpret_cast<uint64_t>(sceneView->outputImage),
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
