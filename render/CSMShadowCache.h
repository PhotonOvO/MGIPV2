#pragma once

#include "CSMShadowResources.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace demo
{
	struct CSMShadowCacheIdentity
	{
		rhi::TextureHandle cascadeTexture{};
		rhi::BufferHandle vertexBuffer{};
		rhi::BufferHandle indexBuffer{};
		uint64_t shadowContentVersion{0};
		uint32_t shadowMeshCount{0};
		uint32_t opaqueShadowMeshCount{0};
		uint32_t cascadeCount{0};

		friend constexpr bool operator==(
			const CSMShadowCacheIdentity& lhs,
			const CSMShadowCacheIdentity& rhs) noexcept = default;
	};

	class CSMShadowCache
	{
	public:
		[[nodiscard]] uint32_t dirtyCascadeMask(
			const CSMShadowResources::FrameData& frameData,
			const CSMShadowCacheIdentity& identity) const noexcept
		{
			const uint32_t activeMask = cascadeMask(identity.cascadeCount);
			// A zero version means an external scene source has not opted into the
			// invalidation contract, so retaining its shadow contents is unsafe.
			if (identity.shadowContentVersion == 0u || identity != m_identity)
			{
				return activeMask;
			}
			if (!sameVector(frameData.lightDirection, m_lightDirection)
				|| frameData.normalBiasWorld != m_normalBiasWorld)
			{
				return activeMask;
			}

			uint32_t dirtyMask = activeMask & ~m_validMask;
			for (uint32_t cascadeIndex = 0; cascadeIndex < identity.cascadeCount; ++cascadeIndex)
			{
				const uint32_t cascadeBit = 1u << cascadeIndex;
				if ((dirtyMask & cascadeBit) != 0u)
				{
					continue;
				}
				const CSMShadowResources::CascadeData& cascade = frameData.cascades[cascadeIndex];
				const CascadeSnapshot& snapshot = m_cascades[cascadeIndex];
				if (!sameMatrix(cascade.viewProjection, snapshot.viewProjection)
					|| !sameMatrix(cascade.cullingViewProjection, snapshot.cullingViewProjection))
				{
					dirtyMask |= cascadeBit;
				}
			}
			return dirtyMask;
		}

		void publish(
			const CSMShadowResources::FrameData& frameData,
			const CSMShadowCacheIdentity& identity,
			uint32_t renderedCascadeMask) noexcept
		{
			if (identity != m_identity)
			{
				m_validMask = 0u;
			}
			m_identity = identity;
			m_lightDirection = frameData.lightDirection;
			m_normalBiasWorld = frameData.normalBiasWorld;

			const uint32_t publishedMask =
				renderedCascadeMask & cascadeMask(identity.cascadeCount);
			for (uint32_t cascadeIndex = 0; cascadeIndex < identity.cascadeCount; ++cascadeIndex)
			{
				const uint32_t cascadeBit = 1u << cascadeIndex;
				if ((publishedMask & cascadeBit) == 0u)
				{
					continue;
				}
				m_cascades[cascadeIndex] = CascadeSnapshot{
					.viewProjection = frameData.cascades[cascadeIndex].viewProjection,
					.cullingViewProjection = frameData.cascades[cascadeIndex].cullingViewProjection,
				};
			}
			m_validMask = (m_validMask | publishedMask) & cascadeMask(identity.cascadeCount);
		}

		void invalidate() noexcept
		{
			m_identity = {};
			m_validMask = 0u;
		}

	private:
		struct CascadeSnapshot
		{
			glm::mat4 viewProjection{1.0f};
			glm::mat4 cullingViewProjection{1.0f};
		};

		[[nodiscard]] static constexpr uint32_t cascadeMask(uint32_t cascadeCount) noexcept
		{
			const uint32_t clampedCount =
				std::min(cascadeCount, static_cast<uint32_t>(shaderio::LCascadeCount));
			return clampedCount == 0u ? 0u : (1u << clampedCount) - 1u;
		}

		[[nodiscard]] static bool sameVector(const glm::vec3& lhs, const glm::vec3& rhs) noexcept
		{
			return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
		}

		[[nodiscard]] static bool sameMatrix(const glm::mat4& lhs, const glm::mat4& rhs) noexcept
		{
			for (uint32_t column = 0; column < 4u; ++column)
			{
				for (uint32_t row = 0; row < 4u; ++row)
				{
					if (lhs[column][row] != rhs[column][row])
					{
						return false;
					}
				}
			}
			return true;
		}

		CSMShadowCacheIdentity m_identity{};
		std::array<CascadeSnapshot, shaderio::LCascadeCount> m_cascades{};
		glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
		float m_normalBiasWorld{0.0f};
		uint32_t m_validMask{0u};
	};
} // namespace demo
