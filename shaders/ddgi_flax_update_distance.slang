// ddgi_flax_update_distance.slang - Flax-style probe distance/moment update.
// Dispatches only active probe tiles and uses the active-list slot directly as
// the trace row, avoiding a full-atlas dispatch and per-texel linear search.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesTrace         : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> probesData          : register(t1);
[[vk::binding(2, 0)]] RWTexture2D<float2> probesDistance    : register(u0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> activeProbes : register(u1); // counter + indices
[[vk::binding(4, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData : register(t2);
[[vk::binding(5, 0)]] Texture2D<float2> probesDistanceHist : register(t3);
static FlaxDDGIData ddgi = ddgiData[0];

[numthreads(8, 8, 1)]
void CS_UpdateDistance(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    static const uint kCascadeIndex = 0;
    static const uint kTileSize = 16;
    static const uint kGroupsPerTileAxis = 2;
    static const uint kGroupsPerProbe = kGroupsPerTileAxis * kGroupsPerTileAxis;

    uint activeSlot = groupId.x / kGroupsPerProbe;
    uint activeCount = activeProbes[0];
    if (activeSlot >= activeCount) return;

    uint tileGroup = groupId.x % kGroupsPerProbe;
    uint2 tileBlock = uint2(tileGroup % kGroupsPerTileAxis, tileGroup / kGroupsPerTileAxis);
    uint2 localTexel = tileBlock * 8 + groupThreadId.xy;

    uint probeIndex = activeProbes[1 + activeSlot];
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, kCascadeIndex, probeCoords);
    uint2 probeTile = getProbeTexelCoords(ddgi, kCascadeIndex, scrolledIdx);
    uint2 texel = probeTile * kTileSize + localTexel;

    float4 probeData = probesData.Load(int3(probeTile, 0));
    uint state = decodeProbeState(probeData);
    if (state == kDDGIProbeStateInactive)
    {
        float2 historyData = probesDistanceHist.Load(int3(texel, 0));
        probesDistance[texel] = historyData;
        return;
    }

    float sumDist = 0.0f;
    float sumDist2 = 0.0f;
    float accumulatedWeight = 0.0f;
    float2 octahedralCoord = ((float2(localTexel) + 0.5f) / float(kTileSize)) * 2.0f - 1.0f;
    float3 texelDirection = flaxOctahedralDecode(octahedralCoord);

    for (uint r = 0; r < ddgi.raysCount; r++)
    {
        float4 traceData = probesTrace.Load(int3(uint2(r, activeSlot), 0));
        float hitDist = traceData.w;
        if (hitDist > 0.0f)
        {
            float3 rayDirection = flaxSphericalFibonacci(float(r), float(ddgi.raysCount));
            float rayWeight = saturate(dot(texelDirection, rayDirection));
            sumDist += hitDist * rayWeight;
            sumDist2 += hitDist * hitDist * rayWeight;
            accumulatedWeight += rayWeight;
        }
    }

    float meanDist = accumulatedWeight > 0.0f
        ? sumDist / accumulatedWeight : ddgi.rayMaxDistance;
    float varDist = accumulatedWeight > 0.0f
        ? max(sumDist2 / accumulatedWeight - meanDist * meanDist, 0.0f) : 0.0f;

    float2 historyData = probesDistanceHist.Load(int3(texel, 0));
    float historyWeight = state == kDDGIProbeStateActivated ? 0.0f : ddgi.probeHistoryWeight;
    float historyMeanDistance = historyData.x * ddgi.rayMaxDistance;
    meanDist = lerp(meanDist, historyMeanDistance, historyWeight);
    varDist = lerp(varDist, historyData.y, historyWeight);

    probesDistance[texel] = float2(meanDist / ddgi.rayMaxDistance, varDist);
}
