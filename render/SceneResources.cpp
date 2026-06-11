#include "SceneResources.h"
#include "BatchUploadContext.h"
#include "RHIFormatBridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace demo
{
	namespace
	{
		uint32_t computeMipCount(rhi::Extent2D extent)
		{
			uint32_t mipCount = 1;
			uint32_t size = std::max(extent.width, extent.height);
			while (size > 1)
			{
				size /= 2u;
				++mipCount;
			}
			return mipCount;
		}

		std::vector<uint8_t> generateBuiltInColorGradingLut()
		{
			constexpr uint32_t kLutSize = SceneResources::kColorGradingLutSize;
			constexpr float kInvMax = 1.0f / static_cast<float>(kLutSize - 1u);
			std::vector<uint8_t> pixels(static_cast<size_t>(kLutSize) * kLutSize * kLutSize * 4u);

			const auto saturate = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
			const auto smoothstep = [](float edge0, float edge1, float x)
			{
				const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1.0e-5f), 0.0f, 1.0f);
				return t * t * (3.0f - 2.0f * t);
			};
			const auto toByte = [&](float value)
			{
				return static_cast<uint8_t>(std::round(saturate(value) * 255.0f));
			};

			for (uint32_t b = 0; b < kLutSize; ++b)
			{
				for (uint32_t g = 0; g < kLutSize; ++g)
				{
					for (uint32_t r = 0; r < kLutSize; ++r)
					{
						float rr = static_cast<float>(r) * kInvMax;
						float gg = static_cast<float>(g) * kInvMax;
						float bb = static_cast<float>(b) * kInvMax;
						const float lum = rr * 0.2126f + gg * 0.7152f + bb * 0.0722f;

						const float shadowWeight = 1.0f - smoothstep(0.15f, 0.58f, lum);
						const float midWeight = 1.0f - std::abs(lum - 0.5f) * 2.0f;
						const float highlightWeight = smoothstep(0.38f, 1.0f, lum);

						rr = (rr - 0.5f) * 1.08f + 0.5f;
						gg = (gg - 0.5f) * 1.05f + 0.5f;
						bb = (bb - 0.5f) * 1.08f + 0.5f;

						rr += highlightWeight * 0.055f + midWeight * 0.018f;
						gg += highlightWeight * 0.018f + shadowWeight * 0.025f;
						bb += shadowWeight * 0.075f - highlightWeight * 0.035f;

						const float warmPush = highlightWeight * 0.045f;
						const float tealPush = shadowWeight * 0.055f;
						rr = rr + warmPush - tealPush * 0.015f;
						gg = gg + tealPush * 0.025f;
						bb = bb + tealPush - warmPush * 0.020f;

						const size_t index = (static_cast<size_t>(b) * kLutSize * kLutSize
							+ static_cast<size_t>(g) * kLutSize
							+ static_cast<size_t>(r)) * 4u;
						pixels[index + 0u] = toByte(rr);
						pixels[index + 1u] = toByte(gg);
						pixels[index + 2u] = toByte(bb);
						pixels[index + 3u] = 255u;
					}
				}
			}

			return pixels;
		}
	} // namespace

	void SceneResources::init(rhi::Device& device, rhi::CommandBuffer& cmd, const CreateInfo& createInfo)
	{
		ASSERT(m_createInfo.color.empty(), "Missing deinit()");
		m_rhiDevice = &device;
		m_createInfo = createInfo;
		m_debugBridge = createInfo.debugBridge;
		create(cmd);
	}

	void SceneResources::deinit()
	{
		destroy();
		*this = SceneResources{};
	}

	void SceneResources::update(rhi::CommandBuffer& cmd, rhi::Extent2D newSize)
	{
		if (newSize.width == m_createInfo.size.width && newSize.height == m_createInfo.size.height)
		{
			return;
		}

		destroy();
		m_createInfo.size = newSize;
		create(cmd);
	}

	ImTextureID SceneResources::getImTextureID(uint32_t i) const
	{
		return m_imguiTextureIds[i];
	}

	rhi::Extent2D SceneResources::getSize() const
	{
		return m_createInfo.size;
	}

	rhi::TextureHandle SceneResources::getColorImage(uint32_t i) const
	{
		return m_resources.colorImages[i].image;
	}

	rhi::TextureHandle SceneResources::getDepthImage() const
	{
		return m_resources.depthImage.image;
	}

	rhi::TextureViewHandle SceneResources::getColorImageView(uint32_t i) const
	{
		return m_resources.colorViews[i];
	}

	rhi::TextureViewHandle SceneResources::getDepthImageView() const
	{
		return m_resources.depthView;
	}

	rhi::TextureFormat SceneResources::getColorFormat(uint32_t i) const
	{
		return m_createInfo.color[i];
	}

	rhi::TextureFormat SceneResources::getDepthFormat() const
	{
		return m_createInfo.depth;
	}

	rhi::SampleCount SceneResources::getSampleCount() const
	{
		return m_createInfo.sampleCount;
	}

	float SceneResources::getAspectRatio() const
	{
		return float(m_createInfo.size.width) / float(m_createInfo.size.height);
	}

	void SceneResources::create(rhi::CommandBuffer& cmdBuffer)
	{
		const auto numColor = static_cast<uint32_t>(m_createInfo.color.size());

		// Centralizes RHI view creation. Debug naming flows through the desc debugName
		// (backend names both images and views). levelCount/baseMip/swizzle default to
		// the common single-mip color-view case.
		const auto createView = [&](rhi::TextureHandle image, rhi::TextureFormat format, rhi::TextureAspect aspect,
		                            const std::string& name,
		                            rhi::ComponentMapping swizzle = {}, uint32_t baseMip = 0,
		                            uint32_t levelCount = 1) -> rhi::TextureViewHandle
		{
			rhi::TextureViewCreateDesc desc{};
			desc.image = image;
			desc.format = format;
			desc.viewType = rhi::ImageViewType::e2D;
			desc.aspect = aspect;
			desc.baseMipLevel = baseMip;
			desc.levelCount = levelCount;
			desc.components = swizzle;
			desc.debugName = name.c_str();
			return m_rhiDevice->createTextureView(desc);
		};
		const auto makeTextureDesc = [](rhi::TextureFormat format,
		                                rhi::Extent2D extent,
		                                rhi::TextureUsageFlags usage,
		                                rhi::SampleCount sampleCount = rhi::SampleCount::count1,
		                                uint32_t mipLevels = 1,
		                                const char* debugName = nullptr)
		{
			return rhi::TextureDesc{
				.dimension = rhi::TextureDimension::e2D,
				.format = format,
				.usage = usage,
				.extent = {extent.width, extent.height, 1},
				.mipLevels = mipLevels,
				.arrayLayers = 1,
				.sampleCount = sampleCount,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = debugName,
			};
		};
		const auto colorTargetUsage = rhi::TextureUsageFlags::colorAttachment
			| rhi::TextureUsageFlags::sampled
			| rhi::TextureUsageFlags::transferSrc
			| rhi::TextureUsageFlags::transferDst;

		m_resources.colorImages.resize(numColor);
		m_resources.colorViews.resize(numColor);
		m_resources.uiImageViews.resize(numColor);
		m_imguiTextureIds.resize(numColor);

		for (uint32_t c = 0; c < numColor; ++c)
		{
			const std::string imageName = "SceneColor" + std::to_string(c);
			const rhi::TextureDesc imageInfo =
				makeTextureDesc(m_createInfo.color[c],
				                m_createInfo.size,
				                colorTargetUsage | rhi::TextureUsageFlags::storage,
				                m_createInfo.sampleCount,
				                1,
				                imageName.c_str());
			m_resources.colorImages[c].image = createImage(imageInfo);

			m_resources.colorViews[c] = createView(m_resources.colorImages[c].image, m_createInfo.color[c],
			                                       rhi::TextureAspect::color, "SceneColorView" + std::to_string(c));

			m_resources.uiImageViews[c] = createView(m_resources.colorImages[c].image, m_createInfo.color[c],
			                                         rhi::TextureAspect::color, "SceneColorUIView" + std::to_string(c),
			                                         rhi::ComponentMapping{.a = rhi::ComponentSwizzle::one});
		}

		// Create output texture (follows screen size, like Unity/UE)
		{
			m_resources.outputTextureImage.image =
				createImage(makeTextureDesc(kOutputTextureFormat, m_createInfo.size, colorTargetUsage,
				                            rhi::SampleCount::count1, 1, "OutputTexture"));

			m_resources.outputTextureView = createView(m_resources.outputTextureImage.image, kOutputTextureFormat,
			                                           rhi::TextureAspect::color, "OutputTextureView");
		}

		// Create HDR scene color and mobile bloom targets for the GPU-driven post chain.
		{
			m_resources.sceneColorHdrImage.image =
				createImage(makeTextureDesc(kSceneColorHdrFormat, m_createInfo.size, colorTargetUsage,
				                            rhi::SampleCount::count1, 1, "GPUDrivenSceneColorHDR"));

			m_resources.sceneColorHdrView = createView(m_resources.sceneColorHdrImage.image, kSceneColorHdrFormat,
			                                           rhi::TextureAspect::color, "GPUDrivenSceneColorHDRView");

			m_resources.bloomHalfExtent = {
				std::max(1u, (m_createInfo.size.width + 1u) / 2u),
				std::max(1u, (m_createInfo.size.height + 1u) / 2u),
			};
			m_resources.bloomQuarterExtent = {
				std::max(1u, (m_createInfo.size.width + 3u) / 4u),
				std::max(1u, (m_createInfo.size.height + 3u) / 4u),
			};
			const auto downsampledExtent = [](rhi::Extent2D baseExtent, uint32_t divisor)
			{
				return rhi::Extent2D{
					std::max(1u, (baseExtent.width + divisor - 1u) / divisor),
					std::max(1u, (baseExtent.height + divisor - 1u) / divisor),
				};
			};
			m_resources.bloomEighthExtent = downsampledExtent(m_createInfo.size, 8u);
			m_resources.bloomSixteenthExtent = downsampledExtent(m_createInfo.size, 16u);
			m_resources.bloomThirtySecondExtent = downsampledExtent(m_createInfo.size, 32u);
			m_resources.bloomUpsampleSixteenthExtent = m_resources.bloomSixteenthExtent;
			m_resources.bloomUpsampleEighthExtent = m_resources.bloomEighthExtent;
			m_resources.bloomUpsampleQuarterExtent = m_resources.bloomQuarterExtent;
			m_resources.bloomOutputExtent = m_resources.bloomHalfExtent;

			const auto createBloomTarget = [&](rhi::Extent2D extent, const char* imageName, const char* viewName,
			                                   ImageResource& image, rhi::TextureViewHandle& view)
			{
				image.image = createImage(makeTextureDesc(kBloomFormat, extent, colorTargetUsage,
				                                          rhi::SampleCount::count1, 1, imageName));

				view = createView(image.image, kBloomFormat, rhi::TextureAspect::color, viewName);
			};

			createBloomTarget(m_resources.bloomHalfExtent,
			                  "GPUDrivenBloomHalf",
			                  "GPUDrivenBloomHalfView",
			                  m_resources.bloomHalfImage,
			                  m_resources.bloomHalfView);
			createBloomTarget(m_resources.bloomQuarterExtent,
			                  "GPUDrivenBloomQuarter",
			                  "GPUDrivenBloomQuarterView",
			                  m_resources.bloomQuarterImage,
			                  m_resources.bloomQuarterView);
			createBloomTarget(m_resources.bloomEighthExtent,
			                  "GPUDrivenBloomEighth",
			                  "GPUDrivenBloomEighthView",
			                  m_resources.bloomEighthImage,
			                  m_resources.bloomEighthView);
			createBloomTarget(m_resources.bloomSixteenthExtent,
			                  "GPUDrivenBloomSixteenth",
			                  "GPUDrivenBloomSixteenthView",
			                  m_resources.bloomSixteenthImage,
			                  m_resources.bloomSixteenthView);
			createBloomTarget(m_resources.bloomThirtySecondExtent,
			                  "GPUDrivenBloomThirtySecond",
			                  "GPUDrivenBloomThirtySecondView",
			                  m_resources.bloomThirtySecondImage,
			                  m_resources.bloomThirtySecondView);
			createBloomTarget(m_resources.bloomUpsampleSixteenthExtent,
			                  "GPUDrivenBloomUpsampleSixteenth",
			                  "GPUDrivenBloomUpsampleSixteenthView",
			                  m_resources.bloomUpsampleSixteenthImage,
			                  m_resources.bloomUpsampleSixteenthView);
			createBloomTarget(m_resources.bloomUpsampleEighthExtent,
			                  "GPUDrivenBloomUpsampleEighth",
			                  "GPUDrivenBloomUpsampleEighthView",
			                  m_resources.bloomUpsampleEighthImage,
			                  m_resources.bloomUpsampleEighthView);
			createBloomTarget(m_resources.bloomUpsampleQuarterExtent,
			                  "GPUDrivenBloomUpsampleQuarter",
			                  "GPUDrivenBloomUpsampleQuarterView",
			                  m_resources.bloomUpsampleQuarterImage,
			                  m_resources.bloomUpsampleQuarterView);
			createBloomTarget(m_resources.bloomOutputExtent,
			                  "GPUDrivenBloomOutput",
			                  "GPUDrivenBloomOutputView",
			                  m_resources.bloomOutputImage,
			                  m_resources.bloomOutputView);

			m_resources.velocityImage.image =
				createImage(makeTextureDesc(kVelocityFormat, m_createInfo.size, colorTargetUsage,
				                            rhi::SampleCount::count1, 1, "GPUDrivenVelocity"));

			m_resources.velocityView = createView(m_resources.velocityImage.image, kVelocityFormat,
			                                      rhi::TextureAspect::color, "GPUDrivenVelocityView");

			for (uint32_t historyIndex = 0; historyIndex < static_cast<uint32_t>(m_resources.sceneColorHistoryImages.
				     size());
			     ++historyIndex)
			{
				const std::string imageName = "GPUDrivenSceneColorHistory" + std::to_string(historyIndex);
				m_resources.sceneColorHistoryImages[historyIndex].image =
					createImage(makeTextureDesc(kSceneColorHdrFormat, m_createInfo.size, colorTargetUsage,
					                            rhi::SampleCount::count1, 1, imageName.c_str()));

				m_resources.sceneColorHistoryViews[historyIndex] =
					createView(m_resources.sceneColorHistoryImages[historyIndex].image, kSceneColorHdrFormat,
					           rhi::TextureAspect::color,
					           "GPUDrivenSceneColorHistoryView" + std::to_string(historyIndex));
			}

			m_resources.colorGradingLutExtent = {kColorGradingLutSize * kColorGradingLutSize, kColorGradingLutSize};
			m_resources.colorGradingLutImage.image =
				createImage(makeTextureDesc(kColorGradingLutFormat,
				                            m_resources.colorGradingLutExtent,
				                            rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst |
				                            rhi::TextureUsageFlags::transferSrc,
				                            rhi::SampleCount::count1,
				                            1,
				                            "BuiltInColorGradingLUT"));

			m_resources.colorGradingLutView = createView(m_resources.colorGradingLutImage.image, kColorGradingLutFormat,
			                                             rhi::TextureAspect::color, "BuiltInColorGradingLUTView");

			const std::vector<uint8_t> lutPixels = generateBuiltInColorGradingLut();
			const rhi::TextureBarrier lutUploadBarrier{
				.texture = m_resources.colorGradingLutImage.image,
				.before = rhi::ResourceState::Undefined,
				.after = rhi::ResourceState::TransferDst,
				.range = {.aspect = rhi::TextureAspect::color, .levelCount = 1, .layerCount = 1},
			};
			// Upload layout boundary: initialize the LUT image for the staging copy.
			cmdBuffer.resourceBarrier(&lutUploadBarrier, 1, nullptr, 0);
			BatchUploadContext upload;
			upload.init(*m_rhiDevice, static_cast<uint64_t>(lutPixels.size()) + 16u);
			const BatchUploadContext::Slice slice = upload.allocate(lutPixels.size(), 16);
			std::memcpy(slice.cpuPtr, lutPixels.data(), lutPixels.size());
			const rhi::BufferTextureCopyDesc region{
				.texture = m_resources.colorGradingLutImage.image,
				.aspect = rhi::TextureAspect::color,
				.width = m_resources.colorGradingLutExtent.width,
				.height = m_resources.colorGradingLutExtent.height,
				.depth = 1,
			};
			upload.recordTextureUpload(slice, m_resources.colorGradingLutImage.image, region);
			upload.executeUploads(cmdBuffer);
			m_resources.colorGradingLutStaging.buffer = upload.releaseStagingBuffer();
			const rhi::TextureBarrier lutReadyBarrier{
				.texture = m_resources.colorGradingLutImage.image,
				.before = rhi::ResourceState::TransferDst,
				.after = rhi::ResourceState::General,
				.range = {.aspect = rhi::TextureAspect::color, .levelCount = 1, .layerCount = 1},
			};
			// Upload layout boundary: make the LUT image shader-readable after copy.
			cmdBuffer.resourceBarrier(&lutReadyBarrier, 1, nullptr, 0);
		}

		// Create fixed-resolution shadow map
		{
			m_resources.shadowMapImage.image =
				createImage(makeTextureDesc(m_createInfo.depth,
				                            {kShadowMapSize, kShadowMapSize},
				                            rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled,
				                            rhi::SampleCount::count1,
				                            1,
				                            "ShadowMap"));

			m_resources.shadowMapView = createView(m_resources.shadowMapImage.image, m_createInfo.depth,
			                                       rhi::TextureAspect::depth, "ShadowMapView");
		}

		if (m_createInfo.depth != rhi::TextureFormat::undefined)
		{
			m_resources.depthImage.image =
				createImage(makeTextureDesc(m_createInfo.depth,
				                            m_createInfo.size,
				                            rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled,
				                            m_createInfo.sampleCount,
				                            1,
				                            "SceneDepth"));

			m_resources.depthView = createView(m_resources.depthImage.image, m_createInfo.depth,
			                                   rhi::TextureAspect::depth, "SceneDepthView");

			m_resources.depthPyramidExtent = {
				std::max(1u, (m_createInfo.size.width + 1u) / 2u),
				std::max(1u, (m_createInfo.size.height + 1u) / 2u),
			};
			m_resources.depthPyramidMipCount = computeMipCount(m_resources.depthPyramidExtent);

			m_resources.depthPyramidImage.image =
				createImage(makeTextureDesc(rhi::TextureFormat::r32Sfloat,
				                            m_resources.depthPyramidExtent,
				                            rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::storage |
				                            rhi::TextureUsageFlags::transferSrc,
				                            rhi::SampleCount::count1,
				                            m_resources.depthPyramidMipCount,
				                            "DepthPyramid"));

			m_resources.depthPyramidMipViews.resize(m_resources.depthPyramidMipCount);
			for (uint32_t mipLevel = 0; mipLevel < m_resources.depthPyramidMipCount; ++mipLevel)
			{
				m_resources.depthPyramidMipViews[mipLevel] =
					createView(m_resources.depthPyramidImage.image, rhi::TextureFormat::r32Sfloat,
					           rhi::TextureAspect::color,
					           "DepthPyramidMipView" + std::to_string(mipLevel), rhi::ComponentMapping{}, mipLevel, 1);
			}
		}

		for (uint32_t c = 0; c < numColor; ++c)
		{
			const rhi::TextureSubresourceRange range{
				.aspect = rhi::TextureAspect::color, .levelCount = 1, .layerCount = 1
			};
			const rhi::TextureBarrier barrier{
				.texture = m_resources.colorImages[c].image,
				.before = rhi::ResourceState::Undefined,
				.after = rhi::ResourceState::General,
				.range = range,
			};
			cmdBuffer.resourceBarrier(&barrier, 1, nullptr, 0);
			cmdBuffer.clearColorTexture(m_resources.colorImages[c].image, range,
			                            rhi::ClearColorValue{0.F, 0.F, 0.F, 0.F});
		}

		// Initialize output texture layout
		const rhi::TextureSubresourceRange outputRange{
			.aspect = rhi::TextureAspect::color, .levelCount = 1, .layerCount = 1
		};
		const auto initAndClearColor = [&](rhi::TextureHandle image, const rhi::ClearColorValue& clearValue)
		{
			const rhi::TextureBarrier barrier{
				.texture = image,
				.before = rhi::ResourceState::Undefined,
				.after = rhi::ResourceState::General,
				.range = outputRange,
			};
			cmdBuffer.resourceBarrier(&barrier, 1, nullptr, 0);
			cmdBuffer.clearColorTexture(image, outputRange, clearValue);
		};
		const rhi::ClearColorValue outputClearValue{0.0f, 0.0f, 0.0f, 1.0f};
		initAndClearColor(m_resources.outputTextureImage.image, outputClearValue);
		initAndClearColor(m_resources.sceneColorHdrImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomHalfImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomQuarterImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomEighthImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomSixteenthImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomThirtySecondImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomUpsampleSixteenthImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomUpsampleEighthImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomUpsampleQuarterImage.image, outputClearValue);
		initAndClearColor(m_resources.bloomOutputImage.image, outputClearValue);
		const rhi::ClearColorValue velocityClearValue{0.0f, 0.0f, 0.0f, 0.0f};
		initAndClearColor(m_resources.velocityImage.image, velocityClearValue);
		for (const ImageResource& historyImage : m_resources.sceneColorHistoryImages)
		{
			initAndClearColor(historyImage.image, outputClearValue);
		}

		if (m_createInfo.depth != rhi::TextureFormat::undefined)
		{
			const rhi::TextureSubresourceRange depthRange{
				.aspect = rhi::TextureAspect::depth, .levelCount = 1, .layerCount = 1
			};
			const std::array<rhi::TextureBarrier, 2> depthBarriers{
				{
					{
						.texture = m_resources.depthImage.image, .before = rhi::ResourceState::Undefined,
						.after = rhi::ResourceState::General, .range = depthRange
					},
					{
						.texture = m_resources.shadowMapImage.image, .before = rhi::ResourceState::Undefined,
						.after = rhi::ResourceState::General, .range = depthRange
					},
				}
			};
			cmdBuffer.resourceBarrier(depthBarriers.data(), static_cast<uint32_t>(depthBarriers.size()), nullptr, 0);
			const rhi::TextureBarrier pyramidBarrier{
				.texture = m_resources.depthPyramidImage.image,
				.before = rhi::ResourceState::Undefined,
				.after = rhi::ResourceState::General,
				.range = {
					.aspect = rhi::TextureAspect::color, .levelCount = m_resources.depthPyramidMipCount, .layerCount = 1
				},
			};
			cmdBuffer.resourceBarrier(&pyramidBarrier, 1, nullptr, 0);
		}

		if (m_debugBridge != nullptr && m_debugBridge->isInitialized())
		{
			for (size_t d = 0; d < m_resources.uiImageViews.size(); ++d)
			{
				m_imguiTextureIds[d] = m_debugBridge->registerTexture(
					*m_rhiDevice, m_createInfo.linearSampler, m_resources.uiImageViews[d],
					DebugInteropBridge::ImageLayout::General);
			}
		}

		// Create ImGui descriptor for output texture
		if (m_debugBridge != nullptr && m_debugBridge->isInitialized())
		{
			m_resources.outputTextureImID = m_debugBridge->registerTexture(
				*m_rhiDevice, m_createInfo.linearSampler, m_resources.outputTextureView,
				DebugInteropBridge::ImageLayout::General);
		}
	}

	void SceneResources::destroy()
	{
		if (m_rhiDevice == nullptr)
		{
			return;
		}

		if (m_resources.outputTextureImID && m_debugBridge != nullptr)
		{
			m_debugBridge->unregisterTexture(m_resources.outputTextureImID);
		}
		m_resources.outputTextureImID = {};

		const auto destroyView = [&](rhi::TextureViewHandle& view)
		{
			if (!view.isNull())
			{
				m_rhiDevice->destroyTextureView(view);
				view = {};
			}
		};
		const auto destroyImage = [&](ImageResource& image)
		{
			if (!image.image.isNull())
			{
				m_rhiDevice->destroyTexture(image.image);
				image = {};
			}
		};

		destroyView(m_resources.outputTextureView);
		destroyView(m_resources.sceneColorHdrView);
		destroyView(m_resources.bloomHalfView);
		destroyView(m_resources.bloomQuarterView);
		destroyView(m_resources.bloomEighthView);
		destroyView(m_resources.bloomSixteenthView);
		destroyView(m_resources.bloomThirtySecondView);
		destroyView(m_resources.bloomUpsampleSixteenthView);
		destroyView(m_resources.bloomUpsampleEighthView);
		destroyView(m_resources.bloomUpsampleQuarterView);
		destroyView(m_resources.bloomOutputView);
		destroyView(m_resources.colorGradingLutView);
		destroyView(m_resources.velocityView);
		for (rhi::TextureViewHandle& historyView : m_resources.sceneColorHistoryViews)
		{
			destroyView(historyView);
		}

		if (m_debugBridge != nullptr)
		{
			for (ImTextureID textureId : m_imguiTextureIds)
			{
				m_debugBridge->unregisterTexture(textureId);
			}
		}
		m_imguiTextureIds.clear();

		for (rhi::TextureViewHandle& view : m_resources.depthPyramidMipViews)
		{
			destroyView(view);
		}
		m_resources.depthPyramidMipViews.clear();

		for (rhi::TextureViewHandle& view : m_resources.colorViews)
		{
			destroyView(view);
		}
		m_resources.colorViews.clear();

		for (rhi::TextureViewHandle& view : m_resources.uiImageViews)
		{
			destroyView(view);
		}
		m_resources.uiImageViews.clear();

		destroyView(m_resources.depthView);
		destroyView(m_resources.shadowMapView);

		destroyImage(m_resources.outputTextureImage);
		destroyImage(m_resources.sceneColorHdrImage);
		destroyImage(m_resources.bloomHalfImage);
		destroyImage(m_resources.bloomQuarterImage);
		destroyImage(m_resources.bloomEighthImage);
		destroyImage(m_resources.bloomSixteenthImage);
		destroyImage(m_resources.bloomThirtySecondImage);
		destroyImage(m_resources.bloomUpsampleSixteenthImage);
		destroyImage(m_resources.bloomUpsampleEighthImage);
		destroyImage(m_resources.bloomUpsampleQuarterImage);
		destroyImage(m_resources.bloomOutputImage);
		destroyImage(m_resources.colorGradingLutImage);
		if (!m_resources.colorGradingLutStaging.buffer.isNull())
		{
			m_rhiDevice->destroyBuffer(m_resources.colorGradingLutStaging.buffer);
			m_resources.colorGradingLutStaging = {};
		}
		destroyImage(m_resources.velocityImage);
		for (ImageResource& historyImage : m_resources.sceneColorHistoryImages)
		{
			destroyImage(historyImage);
		}
		destroyImage(m_resources.shadowMapImage);
		for (ImageResource& image : m_resources.colorImages)
		{
			destroyImage(image);
		}
		destroyImage(m_resources.depthImage);
		destroyImage(m_resources.depthPyramidImage);

		m_resources = {};
	}

	rhi::TextureHandle SceneResources::createImage(const rhi::TextureDesc& imageInfo) const
	{
		return m_rhiDevice->createTexture(imageInfo);
	}

	rhi::TextureViewHandle SceneResources::getOutputTextureView() const
	{
		return m_resources.outputTextureView;
	}

	ImTextureID SceneResources::getOutputTextureImID() const
	{
		return m_resources.outputTextureImID;
	}

	rhi::TextureHandle SceneResources::getOutputTextureImage() const
	{
		return m_resources.outputTextureImage.image;
	}

	uint64_t SceneResources::getOutputTextureEstimatedBytes() const
	{
		return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 4u;
	}

	rhi::TextureHandle SceneResources::getSceneColorHdrImage() const
	{
		return m_resources.sceneColorHdrImage.image;
	}

	rhi::TextureViewHandle SceneResources::getSceneColorHdrView() const
	{
		return m_resources.sceneColorHdrView;
	}

	uint64_t SceneResources::getSceneColorHdrEstimatedBytes() const
	{
		return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 8u;
	}

	rhi::TextureHandle SceneResources::getBloomHalfImage() const
	{
		return m_resources.bloomHalfImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomHalfView() const
	{
		return m_resources.bloomHalfView;
	}

	rhi::Extent2D SceneResources::getBloomHalfExtent() const
	{
		return m_resources.bloomHalfExtent;
	}

	rhi::TextureHandle SceneResources::getBloomQuarterImage() const
	{
		return m_resources.bloomQuarterImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomQuarterView() const
	{
		return m_resources.bloomQuarterView;
	}

	rhi::Extent2D SceneResources::getBloomQuarterExtent() const
	{
		return m_resources.bloomQuarterExtent;
	}

	rhi::TextureHandle SceneResources::getBloomEighthImage() const
	{
		return m_resources.bloomEighthImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomEighthView() const
	{
		return m_resources.bloomEighthView;
	}

	rhi::Extent2D SceneResources::getBloomEighthExtent() const
	{
		return m_resources.bloomEighthExtent;
	}

	rhi::TextureHandle SceneResources::getBloomSixteenthImage() const
	{
		return m_resources.bloomSixteenthImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomSixteenthView() const
	{
		return m_resources.bloomSixteenthView;
	}

	rhi::Extent2D SceneResources::getBloomSixteenthExtent() const
	{
		return m_resources.bloomSixteenthExtent;
	}

	rhi::TextureHandle SceneResources::getBloomThirtySecondImage() const
	{
		return m_resources.bloomThirtySecondImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomThirtySecondView() const
	{
		return m_resources.bloomThirtySecondView;
	}

	rhi::Extent2D SceneResources::getBloomThirtySecondExtent() const
	{
		return m_resources.bloomThirtySecondExtent;
	}

	rhi::TextureHandle SceneResources::getBloomUpsampleSixteenthImage() const
	{
		return m_resources.bloomUpsampleSixteenthImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomUpsampleSixteenthView() const
	{
		return m_resources.bloomUpsampleSixteenthView;
	}

	rhi::Extent2D SceneResources::getBloomUpsampleSixteenthExtent() const
	{
		return m_resources.bloomUpsampleSixteenthExtent;
	}

	rhi::TextureHandle SceneResources::getBloomUpsampleEighthImage() const
	{
		return m_resources.bloomUpsampleEighthImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomUpsampleEighthView() const
	{
		return m_resources.bloomUpsampleEighthView;
	}

	rhi::Extent2D SceneResources::getBloomUpsampleEighthExtent() const
	{
		return m_resources.bloomUpsampleEighthExtent;
	}

	rhi::TextureHandle SceneResources::getBloomUpsampleQuarterImage() const
	{
		return m_resources.bloomUpsampleQuarterImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomUpsampleQuarterView() const
	{
		return m_resources.bloomUpsampleQuarterView;
	}

	rhi::Extent2D SceneResources::getBloomUpsampleQuarterExtent() const
	{
		return m_resources.bloomUpsampleQuarterExtent;
	}

	rhi::TextureHandle SceneResources::getBloomOutputImage() const
	{
		return m_resources.bloomOutputImage.image;
	}

	rhi::TextureViewHandle SceneResources::getBloomOutputView() const
	{
		return m_resources.bloomOutputView;
	}

	rhi::Extent2D SceneResources::getBloomOutputExtent() const
	{
		return m_resources.bloomOutputExtent;
	}

	rhi::TextureHandle SceneResources::getColorGradingLutImage() const
	{
		return m_resources.colorGradingLutImage.image;
	}

	rhi::TextureViewHandle SceneResources::getColorGradingLutView() const
	{
		return m_resources.colorGradingLutView;
	}

	rhi::Extent2D SceneResources::getColorGradingLutExtent() const
	{
		return m_resources.colorGradingLutExtent;
	}

	uint64_t SceneResources::getBloomEstimatedBytes() const
	{
		const auto estimateBytes = [](rhi::Extent2D extent)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * 8u;
		};
		return estimateBytes(m_resources.bloomHalfExtent)
			+ estimateBytes(m_resources.bloomQuarterExtent)
			+ estimateBytes(m_resources.bloomEighthExtent)
			+ estimateBytes(m_resources.bloomSixteenthExtent)
			+ estimateBytes(m_resources.bloomThirtySecondExtent)
			+ estimateBytes(m_resources.bloomUpsampleSixteenthExtent)
			+ estimateBytes(m_resources.bloomUpsampleEighthExtent)
			+ estimateBytes(m_resources.bloomUpsampleQuarterExtent)
			+ estimateBytes(m_resources.bloomOutputExtent);
	}

	rhi::TextureHandle SceneResources::getVelocityImage() const
	{
		return m_resources.velocityImage.image;
	}

	rhi::TextureViewHandle SceneResources::getVelocityView() const
	{
		return m_resources.velocityView;
	}

	uint64_t SceneResources::getVelocityEstimatedBytes() const
	{
		return static_cast<uint64_t>(m_createInfo.size.width) * static_cast<uint64_t>(m_createInfo.size.height) * 4u;
	}

	rhi::TextureHandle SceneResources::getSceneColorHistoryImage(uint32_t index) const
	{
		return m_resources.sceneColorHistoryImages[index % static_cast<uint32_t>(m_resources.sceneColorHistoryImages.
			size())].image;
	}

	rhi::TextureViewHandle SceneResources::getSceneColorHistoryView(uint32_t index) const
	{
		return m_resources.sceneColorHistoryViews[index % static_cast<uint32_t>(m_resources.sceneColorHistoryViews.
			size())];
	}

	uint64_t SceneResources::getSceneColorHistoryEstimatedBytes() const
	{
		return getSceneColorHdrEstimatedBytes()
			* static_cast<uint64_t>(m_resources.sceneColorHistoryImages.size());
	}

	rhi::TextureHandle SceneResources::getShadowMapImage() const
	{
		return m_resources.shadowMapImage.image;
	}

	rhi::TextureViewHandle SceneResources::getShadowMapView() const
	{
		return m_resources.shadowMapView;
	}

	rhi::Extent2D SceneResources::getShadowMapExtent() const
	{
		return {kShadowMapSize, kShadowMapSize};
	}

	rhi::TextureHandle SceneResources::getDepthPyramidImage() const
	{
		return m_resources.depthPyramidImage.image;
	}

	rhi::TextureViewHandle SceneResources::getDepthPyramidMipView(uint32_t mipLevel) const
	{
		if (m_resources.depthPyramidMipViews.empty())
		{
			return {};
		}
		return m_resources.depthPyramidMipViews[std::min<uint32_t>(
			mipLevel, static_cast<uint32_t>(m_resources.depthPyramidMipViews.size() - 1))];
	}

	rhi::Extent2D SceneResources::getDepthPyramidExtent() const
	{
		return m_resources.depthPyramidExtent;
	}

	uint32_t SceneResources::getDepthPyramidMipCount() const
	{
		return m_resources.depthPyramidMipCount;
	}
} // namespace demo
