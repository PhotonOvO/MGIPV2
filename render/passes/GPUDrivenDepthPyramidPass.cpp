#include "GPUDrivenDepthPyramidPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo
{
	GPUDrivenDepthPyramidPass::GPUDrivenDepthPyramidPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenDepthPyramidPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			// The pyramid is consumed as a sampled image by GPU-driven culling, and the
			// descriptor is written with VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			// (VulkanDevice's default for a sampled binding that is not readWrite).
			// Declaring General here left the image in a layout the descriptor was not
			// written for, which validation reports as both an imageLayout mismatch on
			// the descriptor and an oldLayout mismatch on the later barrier.
			PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::write, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenDepthPyramidPass::execute(const PassContext& context) const
	{
		if (m_renderer != nullptr && context.commandBuffer != nullptr && context.params != nullptr)
		{
			context.commandBuffer->beginEvent("GPUDrivenDepthPyramid");
			m_renderer->executeDepthPyramidPass(*context.commandBuffer, *context.params);
			context.commandBuffer->endEvent();
		}
	}
} // namespace demo
