#pragma once

// DDGI (Dynamic Diffuse Global Illumination) tunable configuration.
// Wave D0-1: plain POD config skeleton; consumed by later DDGI waves
// (probe volume, SDF ray trace, probe update, lighting sample).
// All defaults follow ddgi-implementation-plan.md section 4 (LuxGI-verified
// values with mobile-budget adjustments).

#include <glm/glm.hpp>

#include <cstdint>

namespace demo
{
	enum class DDGIRuntimeMode : uint32_t
	{
		current = 0,
		flaxStyle = 1,
	};

	struct DDGIConfig
	{
		// --- Current-simple settings (preserved) ---
		// Probe grid dimensions. Computed at runtime from the scene AABB
		// (probeCounts = sceneLength / probeSpacing + 2, clamped per axis).
		glm::uvec3 gridDims{0u, 0u, 0u};
		// World-space distance between neighboring probes.
		float probeSpacing{1.5f};
		// Octahedral irradiance texel resolution per probe (excluding 2px border).
		uint32_t irradianceTexelSize{8u};
		// Octahedral depth texel resolution per probe (excluding 2px border).
		uint32_t depthTexelSize{16u};
		// Rays traced per probe per update (mobile budget; LuxGI desktop default 256).
		uint32_t raysPerProbe{64u};
		// Temporal blend factor: mix(newResult, history, hysteresis).
		float hysteresis{0.98f};
		// Irradiance storage gamma encode/decode exponent.
		float ddgiGamma{5.0f};
		// Depth update weight exponent: pow(dot(rayDir, texelDir), depthSharpness).
		float depthSharpness{50.0f};
		// Surface offset along normal when sampling probes (leak reduction).
		float normalBias{0.3f};
		// Max ray-hit distance stored in the depth atlas. Computed at runtime as
		// probeSpacing * 1.5; default mirrors that for the default spacing.
		float maxDistance{2.25f};
		// Staggered update stride: each frame updates 1/updateStride of the probes.
		uint32_t updateStride{4u};
		// Blend weight between IBL and DDGI irradiance in the lighting pass.
		float ddgiWeight{0.5f};

		// --- Flax-style settings (FGI-040, gated by runtimeMode == flaxStyle) ---
		// Maximum world-space distance from camera to extend GI coverage.
		float giDistance{50.0f};
		// Number of cascaded probe volumes (1-4). Outer cascades cover larger
		// regions at lower resolution.
		uint32_t maxCascades{2u};
		// Temporal response weight for irradiance/distance history blend.
		// Lower = faster response, higher = more stable. Flax default ~0.98.
		float probeHistoryWeight{0.98f};
		// Indirect lighting multiplier applied when sampling DDGI in the light pass.
		float indirectLightingIntensity{1.0f};
		// Fallback irradiance color when no valid probe data is available.
		glm::vec4 fallbackIrradiance{0.02f, 0.02f, 0.02f, 1.0f};
		// Maximum probes updated per frame (budget cap). 0 = unlimited.
		uint32_t maxUpdatedProbesPerFrame{0u};
		// Inner cascade update frequency (1 = every frame, 2 = every other, etc.).
		uint32_t cascadeUpdateFrequency{1u};
		// Debug override: force a specific cascade for visualization (-1 = off).
		int32_t debugCascadeOverride{-1};

		// --- Surface Atlas (FGI-030) ---
		uint32_t surfaceAtlasResolution{2048u};
		uint32_t surfaceAtlasTileResolution{128u};
		float surfaceAtlasCoverageDistance{50.0f};

		// --- Feature gates ---
		// Runtime implementation selector. The Flax-style path is reserved until
		// the cascaded DDGI and Global Surface Atlas resources land.
		DDGIRuntimeMode runtimeMode{DDGIRuntimeMode::current};
		// Separate allocation gate for the future Global Surface Atlas path.
		bool enableGlobalSurfaceAtlas{false};
		// Master toggle. Defaults to off so default rendering behavior is
		// unchanged until later waves wire the DDGI passes in.
		bool enabled{false};
	};
} // namespace demo
