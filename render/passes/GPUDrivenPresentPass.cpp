#include "GPUDrivenPresentPass.h"

#include "../GPUDrivenRenderer.h"

#include <array>

namespace demo
{
	GPUDrivenPresentPass::GPUDrivenPresentPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenPresentPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
			PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenPresentPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenPresent");
		const rhi::Extent2D srcExtent = m_renderer->getSceneViewDepthExtent();
		const rhi::Extent2D dstExtent = m_renderer->getSwapchainExtent();
		// Wave 6: blit through the registry. Both images are color, single mip/layer, so
		// the resourceBarrier aspect/range pitfalls that block depth/array textures do not
		// apply here. If either handle is unresolved, the pass exits without recording a blit.
		const rhi::TextureHandle srcTex = m_renderer->getPassOutputTextureRHIHandle();
		const rhi::TextureHandle dstTex = m_renderer->getCurrentSwapchainTextureRHIHandle();
		if (srcTex.isNull() || dstTex.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}

		const float srcAspect = static_cast<float>(srcExtent.width) / static_cast<float>(srcExtent.height);
		const float dstAspect = static_cast<float>(dstExtent.width) / static_cast<float>(dstExtent.height);
		int32_t dstY0 = 0;
		int32_t dstY1 = static_cast<int32_t>(dstExtent.height);
		int32_t dstX0 = 0;
		int32_t dstX1 = static_cast<int32_t>(dstExtent.width);

		if (dstAspect > srcAspect)
		{
			const int32_t scaledWidth = static_cast<int32_t>(dstExtent.height * srcAspect);
			const int32_t barWidth = (dstExtent.width - scaledWidth) / 2;
			dstX0 = barWidth;
			dstX1 = barWidth + scaledWidth;
		}
		else if (dstAspect < srcAspect)
		{
			const int32_t scaledHeight = static_cast<int32_t>(dstExtent.width / srcAspect);
			const int32_t barHeight = (dstExtent.height - scaledHeight) / 2;
			dstY0 = barHeight;
			dstY1 = barHeight + scaledHeight;
		}

		// Discard the acquired swapchain image, clear the complete target, then
		// aspect-fit the scene into it. The blit may not cover letterbox bars, so a
		// clear is required before transparent ImGui windows are composited.
		const rhi::TextureSubresourceRange colorRange{rhi::TextureAspect::color, 0, 1, 0, 1};
		const rhi::TextureBarrier toClear[] = {
			{
				.texture = srcTex, .before = rhi::ResourceState::General, .after = rhi::ResourceState::TransferSrc,
				.range = colorRange
			},
			{
				// Swapchain images are undefined on first acquire and remain in PRESENT
				// between frames; Undefined is the intentional discard boundary for the
				// clear-and-blit destination.
				.texture = dstTex, .before = rhi::ResourceState::Undefined, .after = rhi::ResourceState::General,
				.range = colorRange
			},
		};
		context.commandBuffer->resourceBarrier(toClear, {});
		context.commandBuffer->clearColorTexture(
			dstTex, colorRange, rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f});
		const std::array toBlit{
			rhi::TextureBarrier{
				.texture = dstTex,
				.before = rhi::ResourceState::General,
				.after = rhi::ResourceState::TransferDst,
				.range = colorRange,
			},
		};
		context.commandBuffer->resourceBarrier(toBlit, {});

		rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
		enc->blitTexture(rhi::TextureBlitDesc{
			.srcTexture = srcTex,
			.dstTexture = dstTex,
			.aspect = rhi::TextureAspect::color,
			.srcOffsets = {
				{0, 0, 0}, {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1}
			},
			.dstOffsets = {{dstX0, dstY0, 0}, {dstX1, dstY1, 1}},
		});
		context.commandBuffer->endEncoding();

		// Leave the swapchain in General only when ImGui will consume it. Headless
		// automation can transition directly to Present in this existing barrier.
		const rhi::ResourceState swapchainAfterBlit = context.params->renderUi
			? rhi::ResourceState::General
			: rhi::ResourceState::Present;
		const rhi::TextureBarrier fromBlit[] = {
			{
				.texture = dstTex, .before = rhi::ResourceState::TransferDst, .after = swapchainAfterBlit,
				.range = colorRange
			},
			{
				.texture = srcTex, .before = rhi::ResourceState::TransferSrc, .after = rhi::ResourceState::General,
				.range = colorRange
			},
		};
		context.commandBuffer->resourceBarrier(fromBlit, {});

		context.commandBuffer->endEvent();
	}
} // namespace demo
