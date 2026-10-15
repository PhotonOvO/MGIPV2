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
			// ImGui samples the offscreen viewport image and draws over it. PresentPass owns the
			// transfer-source transition for that target, so this pass only needs the sampled-read
			// layout. A readWrite/General declaration made the graph emit its own
			// TransferSrc -> General transition for a target PresentPass had already left in
			// General, which validation reported as VUID-VkImageMemoryBarrier2-oldLayout-01197.
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

		m_renderer->executeImGuiPass(*context.commandBuffer, *context.params);
		// No trailing barrier here. The pass's dependency (readWrite + General) already
		// makes the graph leave the offscreen output in the persistent General layout, and
		// PresentPass blits from it afterwards. A second explicit transition to General
		// contradicted the tracked layout (VUID-VkImageMemoryBarrier2-oldLayout-01197).
	}
} // namespace demo
