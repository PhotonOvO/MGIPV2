#include "MipmapGenerator.h"

#include "../rhi/RHIEncoder.h"

#include <algorithm>
#include <array>

namespace demo
{
	uint32_t MipmapGenerator::calculateMipLevelCount(uint32_t width, uint32_t height)
	{
		uint32_t levels = 1;
		uint32_t maxDim = std::max(width, height);
		while (maxDim > 1)
		{
			maxDim >>= 1;
			++levels;
		}
		return levels;
	}

	void MipmapGenerator::generateMipmaps(rhi::CommandBuffer& cmd,
	                                      rhi::TextureHandle image,
	                                      uint32_t width,
	                                      uint32_t height,
	                                      uint32_t mipLevels)
	{
		if (image.isNull() || mipLevels <= 1)
		{
			return;
		}

		int32_t mipWidth = static_cast<int32_t>(width);
		int32_t mipHeight = static_cast<int32_t>(height);

		for (uint32_t mip = 0; mip + 1 < mipLevels; ++mip)
		{
			const rhi::TextureSubresourceRange srcRange{
				.aspect = rhi::TextureAspect::color,
				.baseMipLevel = mip,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			};
			const std::array<rhi::TextureBarrier, 2> toBlit{
				{
					{
						.texture = image, .before = rhi::ResourceState::General,
						.after = rhi::ResourceState::TransferSrc, .range = srcRange
					},
					{
						.texture = image,
						.before = rhi::ResourceState::General,
						.after = rhi::ResourceState::TransferDst,
						.range = {
							.aspect = rhi::TextureAspect::color,
							.baseMipLevel = mip + 1,
							.levelCount = 1,
							.baseArrayLayer = 0,
							.layerCount = 1
						}
					},
				}
			};
			cmd.resourceBarrier(toBlit.data(), static_cast<uint32_t>(toBlit.size()), nullptr, 0);

			rhi::ComputeEncoder* encoder = cmd.beginComputePass();
			encoder->blitTexture(rhi::TextureBlitDesc{
				.srcTexture = image,
				.dstTexture = image,
				.aspect = rhi::TextureAspect::color,
				.srcMipLevel = mip,
				.dstMipLevel = mip + 1,
				.layerCount = 1,
				.srcOffsets = {{0, 0, 0}, {mipWidth, mipHeight, 1}},
				.dstOffsets = {{0, 0, 0}, {std::max(1, mipWidth / 2), std::max(1, mipHeight / 2), 1}},
			});
			cmd.endEncoding();

			const std::array<rhi::TextureBarrier, 2> fromBlit{
				{
					{
						.texture = image, .before = rhi::ResourceState::TransferSrc,
						.after = rhi::ResourceState::General, .range = srcRange
					},
					{
						.texture = image,
						.before = rhi::ResourceState::TransferDst,
						.after = rhi::ResourceState::General,
						.range = {
							.aspect = rhi::TextureAspect::color,
							.baseMipLevel = mip + 1,
							.levelCount = 1,
							.baseArrayLayer = 0,
							.layerCount = 1
						}
					},
				}
			};
			cmd.resourceBarrier(fromBlit.data(), static_cast<uint32_t>(fromBlit.size()), nullptr, 0);

			mipWidth = std::max(1, mipWidth / 2);
			mipHeight = std::max(1, mipHeight / 2);
		}
	}
} // namespace demo
