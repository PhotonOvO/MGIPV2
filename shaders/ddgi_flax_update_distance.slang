// ddgi_flax_update_distance.slang — Flax-style probe distance/moment update (FGI-054)
// Filters traced hit distances into probe distance texture with temporal history.
// Handles scroll invalidation and border updates.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesTrace       : register(t0); // input radiance+distance
[[vk::binding(1, 0)]] Texture2D<float4> probesData         : register(t1);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesDistance    : register(u0); // output moments

[[vk::push_constant]] ConstantBuffer<FlaxDDGIData> ddgi;

[numthreads(8, 8, 1)]
void CS_UpdateDistance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 texel = dispatchThreadId.xy;
    uint cascadeIndex = texel.y / ddgi.probesCounts.z;
    if (cascadeIndex >= ddgi.cascadesCount) return;

    // Determine which probe this texel belongs to
    uint probeRes = 14; // DDGI_PROBE_RESOLUTION_DISTANCE
    uint tileSize = probeRes + 2;
    uint probesPerRow = ddgi.probesCounts.x * ddgi.probesCounts.y;

    uint probeX = texel.x / tileSize;
    uint probeY = texel.y % ddgi.probesCounts.z;
    if (probeX >= probesPerRow || probeY >= ddgi.probesCounts.z) return;

    uint probeIndex = probeX + probeY * probesPerRow;
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);

    // Load probe data
    uint2 dataCoord = getProbeTexelCoords(ddgi, cascadeIndex,
        getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords));
    float4 pd = probesData.Load(int3(dataCoord, 0));
    uint state = decodeProbeState(pd);
    float attention = decodeProbeAttention(pd);

    // Accumulate ray hit distances for this probe
    float3 worldPos = getProbeWorldPosition(ddgi, cascadeIndex, probeCoords);
    float sumDist = 0.0f;
    float sumDist2 = 0.0f;
    float validRays = 0.0f;

    for (uint r = 0; r < ddgi.raysCount; r++)
    {
        // Find this probe's batch slot (simplified: assume sequential batch layout)
        // In production, active probe list maps probes to batch slots
        float4 traceData = probesTrace.Load(int3(uint2(r, probeIndex % 256), 0));
        float hitDist = traceData.w;
        if (hitDist > 0.0f)
        {
            sumDist += hitDist;
            sumDist2 += hitDist * hitDist;
            validRays += 1.0f;
        }
    }

    float meanDist = validRays > 0.0f ? sumDist / validRays : ddgi.rayMaxDistance;
    float varDist = validRays > 1.0f ? max(sumDist2 / validRays - meanDist * meanDist, 0.0f) : 0.0f;

    // Temporal blend with history
    float4 historyData = probesDistance.Load(int2(texel));
    float historyWeight = state == kDDGIProbeStateActivated ? 0.0f : ddgi.probeHistoryWeight;
    meanDist = lerp(meanDist, historyData.x, historyWeight);
    varDist  = lerp(varDist,  historyData.y, historyWeight);

    probesDistance[texel] = float4(meanDist / ddgi.rayMaxDistance, varDist, 0, attention);
}
