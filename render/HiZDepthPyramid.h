#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIDevice.h"

#include <vector>

namespace demo
{
	namespace rhi
	{
		class CommandBuffer;
	}

	class HiZDepthPyramid
	{
	public:
		struct MobilePolicy
		{
			uint32_t downsampleDivisor{2};
			uint32_t maxMipCount{32};
			uint32_t minMipSize{1};
		};

		void init(rhi::Device& device, uint32_t frameCount, rhi::Extent2D sourceSize);
		void shutdown();
		void configureMobilePolicy(MobilePolicy policy);
		void resize(rhi::Extent2D sourceSize);
		void generate(uint32_t frameIndex,
		              rhi::CommandBuffer& rhiCmd,
		              rhi::Extent2D sourceSize,
		              rhi::TextureViewHandle sourceDepthView,
		              TextureHandle sourceDepth,
		              rhi::TextureHandle sourceDepthRHI);
		void markBoundForCulling(rhi::ArgumentTableHandle table, uint32_t binding);
		[[nodiscard]] uint32_t getMipCount() const { return m_mipCount; }
		[[nodiscard]] uint32_t getFullMipCount() const { return m_fullMipCount; }
		[[nodiscard]] rhi::Extent2D getExtent() const { return m_size; }
		[[nodiscard]] rhi::Extent2D getSourceExtent() const { return m_sourceSize; }
		[[nodiscard]] rhi::TextureHandle getImageHandle() const { return m_imageHandle; }
		[[nodiscard]] rhi::TextureViewHandle getMipView(uint32_t mipLevel) const;

		[[nodiscard]] const rhi::TextureViewHandle* getMipViewsData() const
		{
			return m_mipViews.empty() ? nullptr : m_mipViews.data();
		}

		[[nodiscard]] TextureHandle getSourceDepth() const { return m_sourceDepth; }
		[[nodiscard]] bool isValid() const { return m_valid; }
		[[nodiscard]] uint64_t getGenerationCount() const { return m_generationCount; }
		[[nodiscard]] rhi::ArgumentTableHandle getLastBoundArgumentTable() const { return m_lastBoundArgumentTable; }
		[[nodiscard]] uint32_t getLastBoundBinding() const { return m_lastBoundBinding; }
		[[nodiscard]] const MobilePolicy& getMobilePolicy() const { return m_mobilePolicy; }
		[[nodiscard]] uint64_t getEstimatedMemoryBytes() const { return m_estimatedMemoryBytes; }

	private:
		struct PerFrameResources
		{
			rhi::BufferHandle uniformBufferHandle{};
			rhi::ArgumentTableHandle argumentTable{};
		};

		void updateArgumentTable(uint32_t frameIndex, rhi::TextureViewHandle sourceDepthView);
		void recreateResources();
		void destroyImageResources();

		rhi::Device* m_rhiDevice{nullptr};
		uint32_t m_frameCount{0};
		MobilePolicy m_mobilePolicy{};
		rhi::Extent2D m_sourceSize{};
		rhi::Extent2D m_size{};
		uint32_t m_fullMipCount{0};
		uint32_t m_mipCount{0};
		rhi::TextureHandle m_imageHandle{};
		TextureHandle m_sourceDepth{};
		std::vector<rhi::TextureViewHandle> m_mipViews;
		std::vector<PerFrameResources> m_perFrame;
		rhi::ArgumentLayoutHandle m_argumentLayout{};
		rhi::PipelineHandle m_pipeline{};
		rhi::ArgumentTableHandle m_lastBoundArgumentTable{};
		uint32_t m_lastBoundBinding{0};
		uint64_t m_generationCount{0};
		uint64_t m_estimatedMemoryBytes{0};
		bool m_valid{false};
		bool m_layoutInitialized{false};
	};
} // namespace demo
