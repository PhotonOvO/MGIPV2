// Flax DDGI compute shader stubs (R4-R7).
// Entry points mirror Flax DDGI.shader compute passes.
// Until fully implemented, each kernel is a no-op that writes zero output.
//
// Shared includes and constants come from ddgi_common.slang and flax_ddgi_common.slang.
// SDF tracing reuses sdf_common.slang.

#include "shader_io.h"
#include "ddgi_common.slang"
#include "flax_ddgi_common.slang"
#include "sdf_common.slang"

// --- Bindings mirror FlaxDDGIResources layout ---

// ProbesData: 2D texture (per-probe offset.xyz + state+attention)
[[vk::binding(0, 0)]] RWTexture2D<float4> probesData;

// ActiveProbes: counter at [0], compacted indices follow
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes;

// Global SDF volume (read-only, pre-composed by GlobalSDFPass)
[[vk::binding(2, 0)]] Texture3D<float> globalSDF;
[[vk::binding(3, 0)]] SamplerState sdfSampler;

// Uniforms: cascade descriptors, camera position, config
[[vk::binding(4, 0)]] ConstantBuffer<FlaxDDGIData> uniforms;

// ============================================================
// CS_Classify (FGI-050): probe classification and relocation
// ============================================================
[numthreads(64, 1, 1)]
void kernelFlaxClassify(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R4: Port Flax CS_Classify
    // - Read globalSDF distance at probe world position
    // - Determine probe state (inactive/activated/active)
    // - Compute relocation offset from SDF gradient
    // - Disable probes inside higher-quality cascades
    // - Write probesData (offset, state, attention)
    // - Atomically append active probes to activeProbes
}

// ============================================================
// CS_InitArgs (FGI-051): indirect dispatch argument builder
// ============================================================
// UpdateProbesInitArgs: cascade x batch x 3 passes (trace/distance/irradiance)
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> initArgs;

[numthreads(64, 1, 1)]
void kernelFlaxInitArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R4: Port Flax CS_UpdateProbesInitArgs
    // - Read activeProbes counter for each cascade
    // - Compute dispatch groups: ceil(activeCount / threadsPerGroup)
    // - Write initArgs[cascade][batch][pass] = (groupsX, groupsY, groupsZ)
}

// ============================================================
// CS_UpdateInactive (FGI-052): jump-flood inactive probe fallback
// ============================================================
[numthreads(8, 8, 1)]
void kernelFlaxUpdateInactive(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R4: Port Flax CS_UpdateInactiveProbes
    // - Jump-flood algorithm to find nearest active probe
    // - Store fallback coordinates in inactive probes' probesData
}

// ============================================================
// CS_TraceRays (FGI-053): SDF ray trace into ProbesTrace
// ============================================================
// ProbesTrace: 2D (raysPerProbe x activeProbes), HDR radiance output
[[vk::binding(0, 2)]] RWTexture2D<float4> probesTrace;

[numthreads(16, 16, 1)]
void kernelFlaxTraceRays(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R5: Port Flax CS_TraceRays
    // - Read activeProbe index, reconstruct world position
    // - Generate spherical Fibonacci ray direction (per-frame rotation)
    // - Trace GlobalSDF with coarse-mip-first traversal
    // - On hit: sample Surface Atlas lighting (R6) or SDF albedo + direct light (R5 fallback)
    // - On miss: sample sky color
    // - Write radiance to probesTrace[rayIndex][probeIndex]
}

// ============================================================
// CS_UpdateDistance (FGI-054): octahedral distance moment update
// ============================================================
// ProbesDistance: 2D atlas (octahedral tiles across cascades)
[[vk::binding(0, 3)]] RWTexture2D<float2> probesDistanceOut;
[[vk::binding(1, 3)]] Texture2D<float2> probesDistanceHistory;

[numthreads(8, 8, 1)]
void kernelFlaxUpdateDistance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R7: Port Flax distance probe update
    // - Map dispatchThreadId to probe texel
    // - Convert octahedral texel to world-space ray direction
    // - Read probesTrace radiance and hit distance
    // - Accumulate first/second moments
    // - Blend with history (hysteresis)
    // - Write probesDistanceOut
}

// ============================================================
// CS_UpdateIrradiance (FGI-055): octahedral irradiance convolution
// ============================================================
// ProbesIrradiance: 2D atlas (octahedral tiles across cascades)
[[vk::binding(0, 4)]] RWTexture2D<float4> probesIrradianceOut;
[[vk::binding(1, 4)]] Texture2D<float4> probesIrradianceHistory;

[numthreads(8, 8, 1)]
void kernelFlaxUpdateIrradiance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R7: Port Flax irradiance probe update
    // - Map dispatchThreadId to probe texel
    // - Convert octahedral texel to world-space ray direction
    // - Read probesTrace radiance for matching directions
    // - Convolve radiance into irradiance using octahedral direction weights
    // - Apply gamma encode
    // - Blend with history (hysteresis)
    // - Write probesIrradianceOut
}

// ============================================================
// CS_BorderUpdate (shared): octahedral border copy
// ============================================================
// Used for both distance and irradiance atlases.
// Binding set varies: distance uses set 5, irradiance uses set 6.

[numthreads(8, 8, 1)]
void kernelFlaxBorderUpdateIrradiance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R7: Copy edge texels to border ring for seamless octahedral sampling
}

[numthreads(8, 8, 1)]
void kernelFlaxBorderUpdateDistance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO R7: Copy edge texels to border ring for seamless octahedral sampling
}
