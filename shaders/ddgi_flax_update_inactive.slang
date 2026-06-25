// ddgi_flax_update_inactive.slang — Inactive probe fallback (FGI-052)
// Jump-flood-style search: propagates nearest active probe coordinates
// to inactive probes across multiple iterations. Ported from Flax behavior.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData      : register(u0); // read/write probe data
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes : register(u1);

[[vk::push_constant]] ConstantBuffer<FlaxDDGIData> ddgi;

[numthreads(8, 8, 1)]
void CS_UpdateInactive(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 texel = dispatchThreadId.xy;
    uint cascadeIndex = texel.y / ddgi.probesCounts.z;
    if (cascadeIndex >= ddgi.cascadesCount) return;

    // Check bounds
    uint probesPerRow = ddgi.probesCounts.x * ddgi.probesCounts.y;
    if (texel.x >= probesPerRow || (texel.y % ddgi.probesCounts.z) >= ddgi.probesCounts.z) return;

    // Load current probe data
    float4 probeData = probesData[texel];
    uint state = decodeProbeState(probeData);

    // Only process inactive probes
    if (state != kDDGIProbeStateInactive) return;

    // Jump-flood: check neighbors at increasing distances
    // Iteration 0: check immediate neighbors (1px)
    // Iteration 1: check distance 2 neighbors
    // (Full version runs multiple passes with growing step sizes)

    int step = 1; // step size grows each iteration (1, 2, 4, 8...)
    float bestDist = 9999.0f;
    float3 bestFallback = float3(0,0,0);

    for (int dy = -step; dy <= step; dy += step)
    {
        for (int dx = -step; dx <= step; dx += step)
        {
            if (dx == 0 && dy == 0) continue;
            int2 neighborCoord = int2(texel) + int2(dx, dy);
            if (neighborCoord.x < 0 || neighborCoord.x >= int(probesPerRow) ||
                neighborCoord.y < 0 || neighborCoord.y >= int(ddgi.probesCounts.z * ddgi.cascadesCount))
                continue;

            float4 neighborData = probesData[neighborCoord];
            uint neighborState = decodeProbeState(neighborData);

            if (neighborState != kDDGIProbeStateInactive)
            {
                float dist = length(float2(dx, dy));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestFallback = float3(neighborCoord.x, neighborCoord.y % ddgi.probesCounts.z,
                                          neighborCoord.y / ddgi.probesCounts.z);
                }
            }
            else if (fallbackCoordsValid(neighborData.xyz))
            {
                uint3 fb = decodeFallbackCoords(neighborData.xyz);
                float dist = length(float3(fb) - float3(texel.x, texel.y % ddgi.probesCounts.z,
                                                         texel.y / ddgi.probesCounts.z));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestFallback = neighborData.xyz;
                }
            }
        }
    }

    if (bestDist < 9999.0f && bestDist <= 2.0f)
    {
        // Store fallback coordinates (remain inactive, but with valid fallback)
        probesData[texel] = float4(bestFallback, -1.0f); // -1 = inactive with fallback
    }
}
