// ddgi_flax_update_inactive.slang — Inactive probe fallback (FGI-052)
// Jump-flood-style search: propagates nearest active probe coordinates
// to inactive probes across multiple iterations. Ported from Flax behavior.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData         : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes    : register(u1);
[[vk::binding(2, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData  : register(t1);
static FlaxDDGIData ddgi = ddgiData[0];

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

    // Search the 3D logical grid. The ProbeData texture is a scrolled 2D packing,
    // so texture-neighbor coordinates are not valid fallback probe coordinates.
    uint3 targetProbeCoords = getLogicalProbeCoordsFromDataTexel(ddgi, cascadeIndex, texel);
    int3 probeCounts = int3(ddgi.probesCounts);
    int step = 1;
    float bestDist = 9999.0f;
    float3 bestFallback = float3(0,0,0);

    for (int dz = -step; dz <= step; dz += step)
    {
        for (int dy = -step; dy <= step; dy += step)
        {
            for (int dx = -step; dx <= step; dx += step)
            {
                if (dx == 0 && dy == 0 && dz == 0) continue;

                int3 neighborCoords = int3(targetProbeCoords) + int3(dx, dy, dz);
                if (any(neighborCoords < int3(0,0,0)) || any(neighborCoords >= probeCounts))
                    continue;

                uint3 neighborProbeCoords = uint3(neighborCoords);
                uint neighborProbeIndex = getScrollingProbeIndex(ddgi, cascadeIndex, neighborProbeCoords);
                uint2 neighborTexel = getProbeTexelCoords(ddgi, cascadeIndex, neighborProbeIndex);
                float4 neighborData = probesData[neighborTexel];
                uint neighborState = decodeProbeState(neighborData);

                if (neighborState != kDDGIProbeStateInactive)
                {
                    float dist = length(float3(neighborProbeCoords) - float3(targetProbeCoords));
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestFallback = encodeFallbackCoords(neighborProbeCoords);
                    }
                }
                else if (fallbackCoordsValid(ddgi, neighborData.xyz))
                {
                    uint3 fallbackCoords = decodeFallbackCoords(neighborData.xyz);
                    float dist = length(float3(fallbackCoords) - float3(targetProbeCoords));
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestFallback = neighborData.xyz;
                    }
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
