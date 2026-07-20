// ddgi_flax_update_inactive.slang - Deterministic inactive-probe sanitation.
//
// Inactive probes do not own valid directional moments. Previous code tried to
// propagate a nearest active probe through one in-place step. That operation
// had cross-workgroup races and produced discontinuous Voronoi regions. The
// lighting sampler now blends the configured fallback irradiance with the
// original trilinear cage weight, so this stage only removes stale relocation
// or fallback payload from inactive texels.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData           : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes   : register(u1);
[[vk::binding(2, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData : register(t1);
static FlaxDDGIData ddgi = ddgiData[0];

[numthreads(8, 8, 1)]
void CS_UpdateInactive(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 texel = dispatchThreadId.xy;
    uint probesPerRow = ddgi.probesCounts.x * ddgi.probesCounts.y;
    uint atlasRows = ddgi.probesCounts.z * ddgi.cascadesCount;
    if (texel.x >= probesPerRow || texel.y >= atlasRows)
        return;

    float4 probeData = probesData[texel];
    if (decodeProbeState(probeData) != kDDGIProbeStateInactive)
        return;

    // Keep the binding live for a stable descriptor contract while ensuring
    // that the sanitation result itself is independent of dispatch ordering.
    uint activeCount = activeProbes[0];
    if (activeCount <= ddgi.probesCounts.x * ddgi.probesCounts.y
                     * ddgi.probesCounts.z)
        probesData[texel] = float4(0.0f, 0.0f, 0.0f, -1.0f);
}
