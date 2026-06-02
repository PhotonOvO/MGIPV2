#include "GPUDrivenSSRPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>

namespace demo {

GPUDrivenSSRPass::GPUDrivenSSRPass(GPUDrivenRenderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GPUDrivenSSRPass::getDependencies() const
{
  static const std::array<PassResourceDependency, 4> dependencies = {
      PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
      PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GPUDrivenSSRPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.cmd == nullptr || context.params == nullptr || context.transientAllocator == nullptr
     || !context.cameraAllocValid)
  {
    return;
  }

  const VkBuffer cameraBuffer = reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque());
  const uint32_t cameraOffset = context.cameraAlloc.offset;
  if(!context.params->debugOptions.enableSSR || m_renderer->getSSRTracePipelineOpaque() == 0
     || m_renderer->getSSRRawImageOpaque() == 0 || cameraBuffer == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t frameIndex = context.frameIndex;
  const VkDescriptorSet ssrSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getSSRDescriptorSetAt(frameIndex));
  const BindGroupHandle ssrBindGroup = m_renderer->getSSRBindGroup(frameIndex);
  const PipelineHandle ssrPipeline = m_renderer->getSSRTracePipelineHandle();
  if(ssrSet == VK_NULL_HANDLE || ssrBindGroup.isNull() || ssrPipeline.isNull())
  {
    return;
  }

  // Host-side rebinding of the per-frame camera UBO into the adopted descriptor set.
  // This is a descriptor write, not command recording, so it stays a direct Vulkan call.
  const VkDescriptorBufferInfo cameraBufferInfo{
      .buffer = cameraBuffer,
      .offset = cameraOffset,
      .range = sizeof(shaderio::CameraUniforms),
  };
  const VkWriteDescriptorSet cameraWrite{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ssrSet,
      .dstBinding = 5,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = &cameraBufferInfo,
  };
  vkUpdateDescriptorSets(m_renderer->getNativeDeviceHandle(), 1, &cameraWrite, 0, nullptr);

  const VkExtent2D halfExtent = m_renderer->getPhase7HalfExtent();
  const shaderio::GPUDrivenSSRPushConstants push{
      .params0 = glm::vec4(halfExtent.width,
                           halfExtent.height,
                           static_cast<float>(std::max(1, context.params->debugOptions.ssrMaxSteps)),
                           context.params->debugOptions.ssrThickness),
      .params1 = glm::vec4(0.05f, 80.0f, 1.0f, 0.0f),
  };

  context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, ssrPipeline);
  context.cmd->bindBindGroup(0, ssrBindGroup, nullptr, 0);
  context.cmd->pushConstants(rhi::ShaderStage::compute, 0, sizeof(push), &push);
  context.cmd->dispatch((halfExtent.width + 7u) / 8u, (halfExtent.height + 7u) / 8u, 1u);

  // SSR raw output is sampled by the lighting/composite fragment stage.
  context.cmd->memoryBarrier(rhi::PipelineStage::Compute, rhi::ResourceAccess::write,
                             rhi::PipelineStage::FragmentShader, rhi::ResourceAccess::read);
}

}  // namespace demo
