#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIStageBarrier.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHITypes.h"
#include "DebugInteropBridge.h"
#include "Pass.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include <imgui.h>

namespace demo
{
	class SceneResources
	{
	public:
		struct CreateInfo
		{
			rhi::Extent2D size{};
			std::vector<rhi::TextureFormat> color;
			rhi::TextureFormat depth{rhi::TextureFormat::undefined};
			rhi::SamplerHandle linearSampler{};
			rhi::SampleCount sampleCount{rhi::SampleCount::count1};
			/// Optional bridge for ImGui texture registration.
			/// When non-null, UI image views and the output texture are registered
			/// with the ImGui Vulkan backend via the bridge (RDEV-05 / D-08 / D-09).
			DebugInteropBridge* debugBridge{nullptr};
		};

		SceneResources() = default;
		~SceneResources() { assert(m_rhiDevice == nullptr && "Missing deinit()"); }

		void init(rhi::Device& device, rhi::CommandBuffer& cmd, const CreateInfo& createInfo);
		void deinit();
		void update(rhi::CommandBuffer& cmd, rhi::Extent2D newSize);

		[[nodiscard]] ImTextureID getImTextureID(uint32_t i = 0) const;
		[[nodiscard]] rhi::Extent2D getSize() const;
		[[nodiscard]] rhi::TextureHandle getColorImage(uint32_t i = 0) const;
		[[nodiscard]] rhi::TextureHandle getDepthImage() const;
		[[nodiscard]] rhi::TextureViewHandle getColorImageView(uint32_t i = 0) const;
		[[nodiscard]] rhi::TextureViewHandle getDepthImageView() const;
		[[nodiscard]] rhi::TextureFormat getColorFormat(uint32_t i = 0) const;
		[[nodiscard]] rhi::TextureFormat getDepthFormat() const;
		[[nodiscard]] rhi::SampleCount getSampleCount() const;
		[[nodiscard]] float getAspectRatio() const;

		// GBuffer MRT accessors (alias for existing color accessors)
		[[nodiscard]] rhi::TextureViewHandle getGBufferImageView(uint32_t index) const
		{
			return getColorImageView(index);
		}

		// Output texture for PBR lighting result (follows screen size, like Unity/UE)
		static constexpr uint32_t kOutputTextureIndex = kPackedGBufferTargetCount; // After packed GBuffer targets

		[[nodiscard]] rhi::TextureViewHandle getOutputTextureView() const;
		[[nodiscard]] ImTextureID getOutputTextureImID() const;
		[[nodiscard]] rhi::TextureHandle getOutputTextureImage() const;
		[[nodiscard]] rhi::TextureFormat getOutputTextureFormat() const { return kOutputTextureFormat; }
		[[nodiscard]] uint64_t getOutputTextureEstimatedBytes() const;
		[[nodiscard]] rhi::TextureHandle getSceneColorHdrImage() const;
		[[nodiscard]] rhi::TextureViewHandle getSceneColorHdrView() const;
		[[nodiscard]] rhi::TextureFormat getSceneColorHdrFormat() const { return kSceneColorHdrFormat; }
		[[nodiscard]] uint64_t getSceneColorHdrEstimatedBytes() const;
		[[nodiscard]] rhi::TextureHandle getBloomHalfImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomHalfView() const;
		[[nodiscard]] rhi::Extent2D getBloomHalfExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomQuarterImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomQuarterView() const;
		[[nodiscard]] rhi::Extent2D getBloomQuarterExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomEighthImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomEighthView() const;
		[[nodiscard]] rhi::Extent2D getBloomEighthExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomSixteenthImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomSixteenthView() const;
		[[nodiscard]] rhi::Extent2D getBloomSixteenthExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomThirtySecondImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomThirtySecondView() const;
		[[nodiscard]] rhi::Extent2D getBloomThirtySecondExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomUpsampleSixteenthImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleSixteenthView() const;
		[[nodiscard]] rhi::Extent2D getBloomUpsampleSixteenthExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomUpsampleEighthImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleEighthView() const;
		[[nodiscard]] rhi::Extent2D getBloomUpsampleEighthExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomUpsampleQuarterImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleQuarterView() const;
		[[nodiscard]] rhi::Extent2D getBloomUpsampleQuarterExtent() const;
		[[nodiscard]] rhi::TextureHandle getBloomOutputImage() const;
		[[nodiscard]] rhi::TextureViewHandle getBloomOutputView() const;
		[[nodiscard]] rhi::Extent2D getBloomOutputExtent() const;
		[[nodiscard]] rhi::TextureHandle getColorGradingLutImage() const;
		[[nodiscard]] rhi::TextureViewHandle getColorGradingLutView() const;
		[[nodiscard]] rhi::Extent2D getColorGradingLutExtent() const;
		[[nodiscard]] uint64_t getBloomEstimatedBytes() const;
		[[nodiscard]] rhi::TextureHandle getVelocityImage() const;
		[[nodiscard]] rhi::TextureViewHandle getVelocityView() const;
		[[nodiscard]] rhi::TextureFormat getVelocityFormat() const { return kVelocityFormat; }
		[[nodiscard]] uint64_t getVelocityEstimatedBytes() const;
		[[nodiscard]] rhi::TextureHandle getSceneColorHistoryImage(uint32_t index) const;
		[[nodiscard]] rhi::TextureViewHandle getSceneColorHistoryView(uint32_t index) const;
		[[nodiscard]] uint64_t getSceneColorHistoryEstimatedBytes() const;

		static constexpr rhi::TextureFormat kOutputTextureFormat = rhi::TextureFormat::bgra8Unorm;
		static constexpr rhi::TextureFormat kSceneColorHdrFormat = rhi::TextureFormat::rgba16Sfloat;
		static constexpr rhi::TextureFormat kBloomFormat = rhi::TextureFormat::rgba16Sfloat;
		static constexpr rhi::TextureFormat kVelocityFormat = rhi::TextureFormat::rg16Sfloat;
		static constexpr rhi::TextureFormat kColorGradingLutFormat = rhi::TextureFormat::rgba8Unorm;

		static constexpr uint32_t kShadowMapSize = 2048;
		static constexpr uint32_t kColorGradingLutSize = 32;

		[[nodiscard]] rhi::TextureHandle getShadowMapImage() const;
		[[nodiscard]] rhi::TextureViewHandle getShadowMapView() const;
		[[nodiscard]] rhi::Extent2D getShadowMapExtent() const;
		[[nodiscard]] rhi::TextureHandle getDepthPyramidImage() const;
		[[nodiscard]] rhi::TextureViewHandle getDepthPyramidMipView(uint32_t mipLevel) const;
		[[nodiscard]] rhi::Extent2D getDepthPyramidExtent() const;
		[[nodiscard]] uint32_t getDepthPyramidMipCount() const;

	private:
		struct ImageResource
		{
			rhi::TextureHandle image{};
		};

		struct BufferResource
		{
			rhi::BufferHandle buffer{};
		};

		struct Resources
		{
			std::vector<ImageResource> colorImages;
			ImageResource depthImage{};
			rhi::TextureViewHandle depthView{};
			std::vector<rhi::TextureViewHandle> colorViews;
			std::vector<rhi::TextureViewHandle> uiImageViews;
			ImageResource outputTextureImage{}; // Fixed-res output for PBR result
			rhi::TextureViewHandle outputTextureView{};
			ImTextureID outputTextureImID{};
			ImageResource sceneColorHdrImage{};
			rhi::TextureViewHandle sceneColorHdrView{};
			ImageResource bloomHalfImage{};
			rhi::TextureViewHandle bloomHalfView{};
			rhi::Extent2D bloomHalfExtent{};
			ImageResource bloomQuarterImage{};
			rhi::TextureViewHandle bloomQuarterView{};
			rhi::Extent2D bloomQuarterExtent{};
			ImageResource bloomEighthImage{};
			rhi::TextureViewHandle bloomEighthView{};
			rhi::Extent2D bloomEighthExtent{};
			ImageResource bloomSixteenthImage{};
			rhi::TextureViewHandle bloomSixteenthView{};
			rhi::Extent2D bloomSixteenthExtent{};
			ImageResource bloomThirtySecondImage{};
			rhi::TextureViewHandle bloomThirtySecondView{};
			rhi::Extent2D bloomThirtySecondExtent{};
			ImageResource bloomUpsampleSixteenthImage{};
			rhi::TextureViewHandle bloomUpsampleSixteenthView{};
			rhi::Extent2D bloomUpsampleSixteenthExtent{};
			ImageResource bloomUpsampleEighthImage{};
			rhi::TextureViewHandle bloomUpsampleEighthView{};
			rhi::Extent2D bloomUpsampleEighthExtent{};
			ImageResource bloomUpsampleQuarterImage{};
			rhi::TextureViewHandle bloomUpsampleQuarterView{};
			rhi::Extent2D bloomUpsampleQuarterExtent{};
			ImageResource bloomOutputImage{};
			rhi::TextureViewHandle bloomOutputView{};
			rhi::Extent2D bloomOutputExtent{};
			ImageResource colorGradingLutImage{};
			rhi::TextureViewHandle colorGradingLutView{};
			rhi::Extent2D colorGradingLutExtent{};
			BufferResource colorGradingLutStaging{};
			ImageResource velocityImage{};
			rhi::TextureViewHandle velocityView{};
			std::array<ImageResource, 2> sceneColorHistoryImages{};
			std::array<rhi::TextureViewHandle, 2> sceneColorHistoryViews{};
			ImageResource shadowMapImage{};
			rhi::TextureViewHandle shadowMapView{};
			ImageResource depthPyramidImage{};
			std::vector<rhi::TextureViewHandle> depthPyramidMipViews;
			rhi::Extent2D depthPyramidExtent{};
			uint32_t depthPyramidMipCount{0};
		};

		void create(rhi::CommandBuffer& cmd);
		void destroy();
		[[nodiscard]] rhi::TextureHandle createImage(const rhi::TextureDesc& imageInfo) const;

		rhi::Device* m_rhiDevice{nullptr};
		CreateInfo m_createInfo{};
		Resources m_resources{};
		std::vector<ImTextureID> m_imguiTextureIds;
		/// Non-owning pointer; may be null when ImGui is not initialised.
		DebugInteropBridge* m_debugBridge{nullptr};
	};
} // namespace demo
