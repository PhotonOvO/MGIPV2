#include "GPUDrivenImguiPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo
{
	GPUDrivenImguiPass::GPUDrivenImguiPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenImguiPass::getDependencies() const
	{
		static const std::array dependencies{
			PassResourceDependency::texture(
				kPassOutputHandle, ResourceAccess::read,
				rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenImguiPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenImgui");
		m_renderer->executeImGuiPass(*context.commandBuffer, *context.params);
		// The dependency transitions the offscreen output from General to
		// ShaderRead for the viewport image. Once ImGui has issued its final sampled
		// read, restore the persistent General boundary for the next frame and any
		// later consumer.
		const rhi::TextureHandle output =
			m_renderer->getPassOutputTextureRHIHandle();
		if (!output.isNull())
		{
			const std::array barriers{
				rhi::TextureBarrier{
					.texture = output,
					.before = rhi::ResourceState::ShaderRead,
					.after = rhi::ResourceState::General,
					.range = {
						.aspect = rhi::TextureAspect::color,
						.levelCount = ~0u,
						.layerCount = ~0u,
					},
				},
			};
			context.commandBuffer->resourceBarrier(barriers, {});
		}
		context.commandBuffer->endEvent();
	}
} // namespace demo
