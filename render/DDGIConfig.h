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
		// Probe grid dimensions. Computed at runtime from the scene AABB with
		// probe centers kept inside the bounds (ceil(length / spacing)).
		glm::uvec3 gridDims{0u, 0u, 0u};
		// World-space distance between neighboring probes.
		float probeSpacing{1.5f};
		// Octahedral irradiance texel resolution per probe (excluding 2px border).
		uint32_t irradianceTexelSize{8u};
		// Octahedral depth texel resolution per probe (excluding 2px border).
		uint32_t depthTexelSize{16u};
		// Rays traced per probe per update (mobile budget; LuxGI desktop default 256).
		uint32_t raysPerProbe{256u};
		// Temporal blend factor: mix(newResult, history, hysteresis).
		float hysteresis{0.98f};
		// Irradiance storage gamma encode/decode exponent.
		float ddgiGamma{5.0f};
		// Depth update weight exponent: pow(dot(rayDir, texelDir), depthSharpness).
		float depthSharpness{50.0f};
		// Surface offset along normal when sampling probes (leak reduction).
		float normalBias{0.3f};
		// Max ray-hit distance. Must cover full cascade extent so empty-area probes
		// can trace into the SDF. 96 = 1.5 * 32 * 2 (probeSpacing * probesPerAxis * 2).
		float maxDistance{96.0f};
		// Staggered update stride: each frame updates 1/updateStride of the probes.
		uint32_t updateStride{4u};
		// Blend weight between IBL and DDGI irradiance in the lighting pass.
		float ddgiWeight{0.65f};

		// --- Flax-style settings (FGI-040, gated by runtimeMode == flaxStyle) ---
		// Maximum world-space distance from camera to extend GI coverage.
		float giDistance{50.0f};
		// Number of cascaded probe volumes (1-4). Outer cascades cover larger
		// regions at lower resolution.
		uint32_t maxCascades{2u};
		// Temporal response weight for irradiance/distance history blend.
		// Lower = faster response, higher = more stable. Flax default ~0.98.
		float probeHistoryWeight{0.98f};
		// Surface-to-camera bias used with normalBias during FlaxGI sampling.
		// A separate view term reduces visibility discontinuities at silhouettes.
		float viewBias{0.1f};
		// Indirect lighting multiplier applied when sampling DDGI in the light pass.
		float indirectLightingIntensity{1.0f};
		// Fallback irradiance color when no valid probe data is available.
		glm::vec4 fallbackIrradiance{0.02f, 0.02f, 0.02f, 1.0f};
		// Maximum probes updated per frame (budget cap). 0 = unlimited.
		uint32_t maxUpdatedProbesPerFrame{2048u};
		// Inner cascade update frequency (1 = every frame, 2 = every other, etc.).
		uint32_t cascadeUpdateFrequency{1u};
		// Debug override: force a specific cascade for visualization (-1 = off).
		int32_t debugCascadeOverride{-1};

		// --- Surface Atlas (FGI-030) ---
		uint32_t surfaceAtlasResolution{2048u};
		uint32_t surfaceAtlasTileResolution{128u};
		float surfaceAtlasCoverageDistance{50.0f};

		// --- Feature gates ---
		// Default FlaxGI enabled for GPU smoke testing.
		DDGIRuntimeMode runtimeMode{DDGIRuntimeMode::flaxStyle};
		bool enableGlobalSurfaceAtlas{true};
		bool enabled{true};

		// --- Flax GI runtime controls (R10: freeze/reset/single-step) ---
		bool flaxGIFreeze{false};
		bool flaxGISingleStep{false};
		bool flaxGIDisableIBL{false};
	};

	struct FlaxGIDebugStatus
	{
		bool ddgiEnabled{false};
		bool flaxRequested{false};
		bool meshSDFLoaded{false};
		bool globalSDFPassReady{false};
		bool globalSDFVolumeReady{false};
		uint32_t globalSDFMeshCount{0};

		bool sharedProbeVolumeReady{false};
		bool probePositionReady{false};
		bool rayTracePassReady{false};
		bool probeUpdatePassReady{false};
		bool lightingAtlasViewsReady{false};
		glm::uvec3 probeGridDims{0u, 0u, 0u};
		uint32_t totalProbes{0};
		uint32_t raysPerProbe{0};

		bool flaxResourcesReady{false};
		uint32_t flaxCascadeCount{0};
		bool flaxOutputReady{false};
		uint32_t flaxOutputParity{0};
		glm::uvec3 flaxProbesPerCascade{0u, 0u, 0u};

		bool surfaceAtlasRequested{false};
		bool surfaceAtlasReady{false};
		bool surfaceAtlasRasterReady{false};
		uint32_t surfaceAtlasObjects{0};
		uint32_t surfaceAtlasDirtyObjects{0};
		uint32_t surfaceAtlasTiles{0};

		uint64_t temporalFrameCounter{0};
		uint32_t updateOffset{0};
		glm::vec3 globalSDFBoundsMin{0.0f};
		glm::vec3 globalSDFBoundsMax{0.0f};
		uint32_t globalSDFResolution{0};
	};
} // namespace demo
