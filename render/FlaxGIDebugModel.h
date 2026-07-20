#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace demo
{
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

	[[nodiscard]] constexpr FlaxGIIndirectDispatch computeFlaxGIIndirectDispatch(
		uint32_t classifiedActiveProbeCount,
		uint32_t maximumProbeCount,
		uint32_t maximumUpdatedProbesPerFrame) noexcept
	{
		uint32_t activeCount = std::min(classifiedActiveProbeCount, maximumProbeCount);
		if(maximumUpdatedProbesPerFrame != 0)
		{
			activeCount = std::min(activeCount, maximumUpdatedProbesPerFrame);
		}

		return FlaxGIIndirectDispatch{
			.activeProbeCount = activeCount,
			.trace = {(activeCount + 255u) / 256u, 1u, 1u},
			.distance = {activeCount * 4u, 1u, 1u},
			.irradiance = {activeCount, 1u, 1u},
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
			std::string_view{"Trace Hit Distance"}, std::string_view{"Probe Distance"},
			std::string_view{"Probe Irradiance"}, std::string_view{"Surface Atlas Lighting"},
		};
		const size_t index = static_cast<size_t>(view);
		return index < names.size() ? names[index] : std::string_view{"Unknown"};
	}
} // namespace demo
