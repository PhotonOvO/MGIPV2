#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace demo
{
	inline constexpr float kFlaxGIDistanceHorizonToSpacing = 2.598076211f;
	inline constexpr float kFlaxGIMinVisibility = 0.05f;

	[[nodiscard]] constexpr float computeFlaxGIDistanceLimit(
		float probeSpacing, float rayMaxDistance) noexcept
	{
		const float spacing = std::max(probeSpacing, 0.0001f);
		const float maxDistance = std::max(rayMaxDistance, 0.0001f);
		return std::min(maxDistance, spacing * kFlaxGIDistanceHorizonToSpacing);
	}

	[[nodiscard]] constexpr float computeFlaxGIRayStartOffset(
		float probeSpacing, float sdfVoxelSize) noexcept
	{
		const float spacing = std::max(probeSpacing, 0.0001f);
		const float voxelSize = std::max(sdfVoxelSize, 0.0001f);
		return std::min(voxelSize * 0.25f, spacing * 0.05f);
	}

	struct FlaxGIDistanceMoments
	{
		float first{0.0f};
		float second{0.0f};
	};

	[[nodiscard]] constexpr FlaxGIDistanceMoments blendFlaxGIDistanceMoments(
		FlaxGIDistanceMoments current,
		FlaxGIDistanceMoments history,
		float historyWeight) noexcept
	{
		const float weight = std::clamp(historyWeight, 0.0f, 1.0f);
		return {
			current.first + (history.first - current.first) * weight,
			current.second + (history.second - current.second) * weight,
		};
	}

	[[nodiscard]] constexpr float computeFlaxGIDistanceVariance(
		FlaxGIDistanceMoments moments) noexcept
	{
		return std::max(moments.second - moments.first * moments.first, 0.0f);
	}

	enum class FlaxGIDebugStage : uint8_t
	{
		meshSDFInput = 0,
		globalSDF,
		surfaceAtlas,
		cascadeLayout,
		classify,
		initArgs,
		updateInactive,
		traceRays,
		updateDistance,
		updateIrradiance,
		lightingSample,
		finalComposite,
		count,
	};

	enum class FlaxGIDebugStageState : uint8_t
	{
		disabled = 0,
		blocked,
		pending,
		executed,
		valid,
		invalid,
		stale,
		notImplemented,
	};

	enum class FlaxGIDebugView : uint8_t
	{
		globalSDFDistance = 0,
		globalSDFAlbedo,
		probeState,
		traceRadiance,
		traceHitDistance,
		probeDistance,
		probeIrradiance,
		surfaceAtlasLighting,
		count,
	};

	struct FlaxGIDispatchDimensions
	{
		uint32_t x{0};
		uint32_t y{1};
		uint32_t z{1};

		[[nodiscard]] constexpr bool operator==(const FlaxGIDispatchDimensions&) const noexcept = default;
	};

	struct FlaxGIIndirectDispatch
	{
		uint32_t activeProbeCount{0};
		FlaxGIDispatchDimensions trace{};
		FlaxGIDispatchDimensions distance{};
		FlaxGIDispatchDimensions irradiance{};

		[[nodiscard]] constexpr bool operator==(const FlaxGIIndirectDispatch&) const noexcept = default;
	};

	struct FlaxGIOutputSelection
	{
		uint32_t parity{0};
		bool valid{false};

		[[nodiscard]] constexpr bool operator==(const FlaxGIOutputSelection&) const noexcept = default;
	};

	// Tracks the atlas pair independently from the renderer-wide temporal counter.
	// A parity becomes visible to consumers only after a complete irradiance update.
	struct FlaxGIOutputState
	{
		uint32_t nextWriteParity{0};
		uint32_t publishedParity{0};
		bool publishedValid{false};

		[[nodiscard]] constexpr FlaxGIOutputSelection selectForFrame(
			bool willPublish, bool willInvalidate) const noexcept
		{
			if(willPublish)
			{
				return {nextWriteParity & 1u, true};
			}
			if(willInvalidate)
			{
				return {publishedParity & 1u, false};
			}
			return {publishedParity & 1u, publishedValid};
		}

		[[nodiscard]] constexpr FlaxGIOutputSelection published() const noexcept
		{
			return {publishedParity & 1u, publishedValid};
		}

		constexpr void publishPending() noexcept
		{
			publishedParity = nextWriteParity & 1u;
			publishedValid = true;
			nextWriteParity = publishedParity ^ 1u;
		}

		constexpr void invalidate() noexcept
		{
			publishedValid = false;
		}

		constexpr void reset() noexcept
		{
			nextWriteParity = 0u;
			publishedParity = 0u;
			publishedValid = false;
		}
	};

	struct FlaxGIDebugTelemetry
	{
		uint64_t sourceFrame{0};
		bool gpuReadbackValid{false};
		bool activeProbeCountValid{false};
		bool indirectArgsValid{false};
		uint32_t classifiedActiveProbeCount{0};
		FlaxGIIndirectDispatch actualDispatch{};
		FlaxGIIndirectDispatch expectedDispatch{};
	};

	struct FlaxGIDebugStageStatus
	{
		FlaxGIDebugStageState state{FlaxGIDebugStageState::disabled};
		uint64_t lastExecutedFrame{0};
		uint64_t inputVersion{0};
		uint64_t outputVersion{0};
		std::string_view reason{};
	};

	struct FlaxGIDebugSnapshot
	{
		std::array<FlaxGIDebugStageStatus, static_cast<size_t>(FlaxGIDebugStage::count)> stages{};
		FlaxGIDebugTelemetry telemetry{};
		uint32_t cascadeCount{0};
		uint32_t implementedCascadeCount{0};
		uint32_t totalProbes{0};
		uint32_t raysPerProbe{0};
		bool surfaceAtlasSampledByTrace{false};
		float probeSpacing{0.0f};
		float distanceMomentHorizon{0.0f};
		float sdfVoxelSize{0.0f};
		float traceRayStartOffset{0.0f};
		float selfHitThreshold{0.0f};
		float minimumVisibility{kFlaxGIMinVisibility};
		uint32_t distanceMomentSchemaVersion{2u};
	};

	struct FlaxGIDebugViewSet
	{
		uint64_t textureId{0};
		uint32_t atlasWidth{0};
		uint32_t atlasHeight{0};
		uint32_t columns{4};
		uint32_t rows{2};
		uint64_t sourceFrame{0};

		[[nodiscard]] constexpr bool isValid() const noexcept
		{
			return textureId != 0 && atlasWidth != 0 && atlasHeight != 0;
		}
	};

	[[nodiscard]] constexpr uint32_t computeFlaxGICascadeUpdateBudget(
		uint32_t globalBudget,
		uint32_t maximumProbeCount,
		uint32_t cascadeIndex,
		uint32_t cascadeCount) noexcept
	{
		if(cascadeCount == 0u) return 0u;
		if(globalBudget == 0u) return maximumProbeCount;
		const uint32_t baseBudget = globalBudget / cascadeCount;
		const uint32_t remainder = globalBudget % cascadeCount;
		return std::min(
			maximumProbeCount,
			baseBudget + (cascadeIndex < remainder ? 1u : 0u));
	}

	[[nodiscard]] constexpr FlaxGIIndirectDispatch computeFlaxGIIndirectDispatch(
		uint32_t classifiedActiveProbeCount,
		uint32_t maximumProbeCount,
		uint32_t cascadeUpdateBudget) noexcept
	{
		const uint32_t activeCount =
			std::min(classifiedActiveProbeCount, maximumProbeCount);
		const uint32_t updateCount = activeCount == 0u
			? 0u : std::min(cascadeUpdateBudget, activeCount);

		return FlaxGIIndirectDispatch{
			.activeProbeCount = activeCount,
			.trace = {(updateCount + 255u) / 256u, 1u, 1u},
			.distance = {updateCount * 4u, 1u, 1u},
			.irradiance = {updateCount, 1u, 1u},
		};
	}

	[[nodiscard]] constexpr bool isFlaxGIActiveProbeCountValid(
		uint32_t activeProbeCount, uint32_t maximumProbeCount) noexcept
	{
		return maximumProbeCount == 0u ? activeProbeCount == 0u
		                               : activeProbeCount > 0u && activeProbeCount <= maximumProbeCount;
	}

	[[nodiscard]] constexpr bool shouldExecuteFlaxGIStage(
		FlaxGIDebugStage stage,
		FlaxGIDebugStage stopAfterStage) noexcept
	{
		return static_cast<uint8_t>(stage) <= static_cast<uint8_t>(stopAfterStage);
	}

	[[nodiscard]] constexpr bool isNewFlaxGIDebugRequest(uint64_t requestId, uint64_t consumedRequestId) noexcept
	{
		return requestId != 0 && requestId != consumedRequestId;
	}

	[[nodiscard]] constexpr std::string_view flaxGIDebugStageName(FlaxGIDebugStage stage) noexcept
	{
		constexpr std::array names{
			std::string_view{"Mesh SDF Input"}, std::string_view{"Global SDF"},
			std::string_view{"Surface Atlas"}, std::string_view{"Cascade Layout"},
			std::string_view{"Classify / Relocate"}, std::string_view{"Active List / Init Args"},
			std::string_view{"Update Inactive"}, std::string_view{"Trace Rays"},
			std::string_view{"Update Distance"}, std::string_view{"Update Irradiance"},
			std::string_view{"Lighting Sample"}, std::string_view{"Final Composite"},
		};
		const size_t index = static_cast<size_t>(stage);
		return index < names.size() ? names[index] : std::string_view{"Unknown"};
	}

	[[nodiscard]] constexpr std::string_view flaxGIDebugStageStateName(FlaxGIDebugStageState state) noexcept
	{
		constexpr std::array names{
			std::string_view{"Disabled"}, std::string_view{"Blocked"},
			std::string_view{"Pending"}, std::string_view{"Executed"},
			std::string_view{"Valid"}, std::string_view{"Invalid"},
			std::string_view{"Stale"}, std::string_view{"Not Implemented"},
		};
		const size_t index = static_cast<size_t>(state);
		return index < names.size() ? names[index] : std::string_view{"Unknown"};
	}

	[[nodiscard]] constexpr std::string_view flaxGIDebugViewName(FlaxGIDebugView view) noexcept
	{
		constexpr std::array names{
			std::string_view{"Global SDF Distance"}, std::string_view{"Global SDF Albedo"},
			std::string_view{"Probe State / Relocation"}, std::string_view{"Trace Radiance"},
			std::string_view{"Trace Hit Distance"}, std::string_view{"Probe Distance Moments"},
			std::string_view{"Probe Irradiance"}, std::string_view{"Surface Atlas Lighting"},
		};
		const size_t index = static_cast<size_t>(view);
		return index < names.size() ? names[index] : std::string_view{"Unknown"};
	}
} // namespace demo
