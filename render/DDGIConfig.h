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
	struct DDGIConfig
	{
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
		// Master toggle. Defaults to off so default rendering behavior is
		// unchanged until later waves wire the DDGI passes in.
		bool enabled{false};
	};
} // namespace demo
