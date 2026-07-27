#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace demo
{
	enum class MeshSDFQualityWarningLevel : uint8_t
	{
		none,
		warning,
		severe,
	};

	struct MeshSDFPhysicalResolutionQuality
	{
		glm::vec3 sourceSpacing{0.0f};
		float globalVoxelSize{0.0f};
		glm::vec3 ratio{0.0f};
		MeshSDFQualityWarningLevel warningLevel{MeshSDFQualityWarningLevel::none};
		bool valid{false};
	};

	[[nodiscard]] inline MeshSDFPhysicalResolutionQuality evaluateMeshSDFPhysicalResolution(
		const glm::vec3& sourceBoundsMin,
		const glm::vec3& sourceBoundsMax,
		const glm::uvec3& sourceResolution,
		const glm::vec3& globalBoundsMin,
		const glm::vec3& globalBoundsMax,
		uint32_t globalResolution)
	{
		constexpr float kWarningRatio = 1.25f;
		constexpr float kSevereRatio = 2.0f;

		MeshSDFPhysicalResolutionQuality quality{};
		if (sourceResolution.x <= 1u || sourceResolution.y <= 1u || sourceResolution.z <= 1u
		    || globalResolution == 0u)
		{
			return quality;
		}

		const glm::vec3 sourceExtent = sourceBoundsMax - sourceBoundsMin;
		const glm::vec3 globalExtent = globalBoundsMax - globalBoundsMin;
		const auto isFinitePositive = [](const glm::vec3& value)
		{
			return std::isfinite(value.x) && value.x > 0.0f
			    && std::isfinite(value.y) && value.y > 0.0f
			    && std::isfinite(value.z) && value.z > 0.0f;
		};
		if (!isFinitePositive(sourceExtent) || !isFinitePositive(globalExtent))
		{
			return quality;
		}

		quality.sourceSpacing =
			sourceExtent / glm::vec3(sourceResolution - glm::uvec3(1u));
		quality.globalVoxelSize =
			std::max({globalExtent.x, globalExtent.y, globalExtent.z})
			/ static_cast<float>(globalResolution);
		if (!std::isfinite(quality.globalVoxelSize) || quality.globalVoxelSize <= 0.0f)
		{
			return quality;
		}

		quality.ratio = quality.sourceSpacing / quality.globalVoxelSize;
		const float worstRatio = std::max({quality.ratio.x, quality.ratio.y, quality.ratio.z});
		quality.warningLevel = worstRatio > kSevereRatio
			? MeshSDFQualityWarningLevel::severe
			: (worstRatio > kWarningRatio
				? MeshSDFQualityWarningLevel::warning
				: MeshSDFQualityWarningLevel::none);
		quality.valid = true;
		return quality;
	}
} // namespace demo
