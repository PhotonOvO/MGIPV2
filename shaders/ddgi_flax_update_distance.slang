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
[[vk::binding(5, 0)]] RWTexture2D<float2> probesDistanceHist : register(u2);
static FlaxDDGIData ddgi = ddgiData[0];
struct FlaxDDGICascadePush
{
    uint cascadeIndex;
};
[[vk::push_constant]] ConstantBuffer<FlaxDDGICascadePush> cascadePush;

[numthreads(8, 8, 1)]
void CS_UpdateDistance(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    static const uint kTileSize = 16;
    static const uint kGroupsPerTileAxis = 2;
    static const uint kGroupsPerProbe = kGroupsPerTileAxis * kGroupsPerTileAxis;
    uint cascadeIndex = min(cascadePush.cascadeIndex, ddgi.cascadesCount - 1u);

    uint activeSlot = groupId.x / kGroupsPerProbe;
    uint tileGroup = groupId.x % kGroupsPerProbe;
    uint2 tileBlock = uint2(tileGroup % kGroupsPerTileAxis, tileGroup / kGroupsPerTileAxis);
    uint2 localTexel = tileBlock * 8 + groupThreadId.xy;

    uint probeIndex;
    uint requiredState;
    if (!getProbeUpdateSelection(
            ddgi, activeProbes, cascadeIndex, activeSlot, probeIndex, requiredState)) return;
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 probeTile = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);
    uint2 texel = probeTile * kTileSize + localTexel;

    float4 probeData = probesData.Load(int3(probeTile, 0));
    uint state = decodeProbeState(probeData);
    if (!probeMatchesUpdateState(probeData, requiredState))
        return;

    float sumDist = 0.0f;
    float distanceLimit = flaxProbeDistanceLimit(ddgi, cascadeIndex);
    float sumDist2 = 0.0f;
    float accumulatedWeight = 0.0f;
    uint2 interiorTexel = flaxProbeTileInteriorTexel(
        localTexel, kDDGIProbeDistanceResolution);
    float2 octahedralCoord =
        ((float2(interiorTexel - uint2(1u, 1u)) + 0.5f)
         / float(kDDGIProbeDistanceResolution)) * 2.0f - 1.0f;
    float3 texelDirection = flaxOctahedralDecode(octahedralCoord);

    for (uint r = 0; r < ddgi.raysCount; r++)
    {
        float4 traceData = probesTrace.Load(int3(uint2(r, activeSlot), 0));
        // Trace writes the finite distance horizon for misses. Clamp here as
        // well so captures made by older trace code cannot reintroduce a
        // conditional-on-hit distance estimator.
        float rayDistance = traceData.w < 0.0f
            ? distanceLimit : min(traceData.w, distanceLimit);
        float3 rayDirection = flaxSphericalFibonacci(float(r), float(ddgi.raysCount));
        float rayWeight = saturate(dot(texelDirection, rayDirection));
        // Flax sharpens the directional lobe to preserve depth discontinuities.
        rayWeight = pow(rayWeight, 10.0f);
        sumDist += rayDistance * rayWeight;
        sumDist2 += rayDistance * rayDistance * rayWeight;
        accumulatedWeight += rayWeight;
    }

    float meanDist = accumulatedWeight > 0.0f
        ? sumDist / accumulatedWeight : distanceLimit;
    float secondMoment = accumulatedWeight > 0.0f
        ? sumDist2 / accumulatedWeight : distanceLimit * distanceLimit;

    float2 historyData = probesDistanceHist.Load(int2(texel));
    bool historyValid = any(historyData > float2(0.0f, 0.0f));
    float historyMeanDistance = historyData.x * distanceLimit;
    float historySecondMoment = historyData.y * distanceLimit * distanceLimit;
    // A high fixed hysteresis can preserve an obsolete near-zero moment for
    // hundreds of frames. Reject history when current rays disagree at probe
    // or SDF-voxel scale.
    float historyWeight = flaxDistanceHistoryWeight(
        ddgi, cascadeIndex, state, historyValid,
        meanDist, historyMeanDistance);
    meanDist = lerp(meanDist, historyMeanDistance, historyWeight);
    secondMoment = lerp(secondMoment, historySecondMoment, historyWeight);

    float2 encodedMoments = float2(
        saturate(meanDist / distanceLimit),
        saturate(secondMoment / (distanceLimit * distanceLimit)));
    // Mirror each budgeted update into both atlases. Unselected probes are
    // already identical, so frame-to-frame parity changes cannot reveal stale
    // or zero tiles while still retaining the previous value as history.
    probesDistance[texel] = encodedMoments;
    probesDistanceHist[texel] = encodedMoments;
}
