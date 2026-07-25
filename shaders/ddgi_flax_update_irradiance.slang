// ddgi_flax_update_irradiance.slang - Flax-style irradiance update.
// One 8x8 group updates one active probe tile; the active-list slot is also
// the trace texture row, so no per-texel active-list search is required.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesTrace                  : register(t0);
[[vk::binding(1, 0)]] RWTexture2D<float4> probesData                 : register(u3);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesIrradianceOut        : register(u0);
[[vk::binding(3, 0)]] RWTexture2D<float4> probesIrradianceHist       : register(u2);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> activeProbes             : register(u1);
[[vk::binding(5, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData            : register(t3);
static FlaxDDGIData ddgi = ddgiData[0];
struct FlaxDDGICascadePush
{
    uint cascadeIndex;
};
[[vk::push_constant]] ConstantBuffer<FlaxDDGICascadePush> cascadePush;

[numthreads(8, 8, 1)]
void CS_UpdateIrradiance(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    static const uint kTileSize = 8;
    uint cascadeIndex = min(cascadePush.cascadeIndex, ddgi.cascadesCount - 1u);

    uint activeSlot = groupId.x;
    uint probeIndex;
    uint requiredState;
    if (!getProbeUpdateSelection(
            ddgi, activeProbes, cascadeIndex, activeSlot, probeIndex, requiredState)) return;
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 probeTile = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);
    uint2 texel = probeTile * kTileSize + groupThreadId.xy;

    float4 probeData = probesData.Load(int2(probeTile));
    uint state = decodeProbeState(probeData);
    if (!probeMatchesUpdateState(probeData, requiredState))
        return;
    float attention = decodeProbeAttention(probeData);
    float activeAttention = clamp(attention, kDDGIProbeAttentionMin, kDDGIProbeAttentionMax);

    float3 accumulated = float3(0, 0, 0);
    float accumulatedWeight = 0.0f;
    uint2 interiorTexel = flaxProbeTileInteriorTexel(
        groupThreadId.xy, kDDGIProbeIrradianceResolution);
    float2 octahedralCoord =
        ((float2(interiorTexel - uint2(1u, 1u)) + 0.5f)
         / float(kDDGIProbeIrradianceResolution)) * 2.0f - 1.0f;
    float3 texelDirection = flaxOctahedralDecode(octahedralCoord);
    for (uint r = 0; r < ddgi.raysCount; r++)
    {
        float4 traceData = probesTrace.Load(int3(uint2(r, activeSlot), 0));
        // Miss rays carry sky radiance and must participate in the directional
        // irradiance integral just like surface hits.
        float3 rayDirection = flaxSphericalFibonacci(float(r), float(ddgi.raysCount));
        float rayWeight = saturate(dot(texelDirection, rayDirection));
        accumulated += traceData.rgb * rayWeight;
        accumulatedWeight += rayWeight;
    }

    float3 irradiance = accumulatedWeight > 0.0f
        ? accumulated / accumulatedWeight : float3(0, 0, 0);
    float4 historyIrradiance = probesIrradianceHist.Load(int2(texel));
    bool historyValid = historyIrradiance.a > 0.0f;
    float3 historyLinear = pow(max(historyIrradiance.rgb, float3(0, 0, 0)),
                               ddgi.irradianceGamma);
    float historyWeight = flaxIrradianceHistoryWeight(
        ddgi, state, historyValid, irradiance, historyLinear);
    irradiance = lerp(irradiance, historyLinear, historyWeight);
    irradiance = pow(max(irradiance, float3(0, 0, 0)), 1.0f / ddgi.irradianceGamma);

    // Keep both parity atlases coherent under the partial per-frame probe
    // budget. The history read happens before these stores in this invocation.
    float4 encodedIrradiance = float4(irradiance, activeAttention);
    probesIrradianceOut[texel] = encodedIrradiance;
    probesIrradianceHist[texel] = encodedIrradiance;
    if (all(groupThreadId.xy == uint2(0u, 0u)))
        probesData[probeTile] = encodeProbeData(
            probeData.xyz, kDDGIProbeStateActive, activeAttention);
}
