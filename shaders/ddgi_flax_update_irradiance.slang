// ddgi_flax_update_irradiance.slang - Flax-style irradiance update.
// One 8x8 group updates one active probe tile; the active-list slot is also
// the trace texture row, so no per-texel active-list search is required.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesTrace                  : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> probesData                   : register(t1);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesIrradianceOut        : register(u0);
[[vk::binding(3, 0)]] Texture2D<float4> probesIrradianceHist         : register(t2);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> activeProbes             : register(u1);
[[vk::binding(5, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData            : register(t3);
static FlaxDDGIData ddgi = ddgiData[0];

[numthreads(8, 8, 1)]
void CS_UpdateIrradiance(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    static const uint kCascadeIndex = 0;
    static const uint kTileSize = 8;

    uint activeSlot = groupId.x;
    uint totalActiveCount = activeProbes[0];
    uint updateCount = ddgi.updateParams.y == 0u
        ? totalActiveCount : min(totalActiveCount, ddgi.updateParams.y);
    if (activeSlot >= updateCount || totalActiveCount == 0u) return;

    uint selectedActiveSlot = (activeSlot + ddgi.updateParams.x) % totalActiveCount;
    uint probeIndex = activeProbes[1 + selectedActiveSlot];
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, kCascadeIndex, probeCoords);
    uint2 probeTile = getProbeTexelCoords(ddgi, kCascadeIndex, scrolledIdx);
    uint2 texel = probeTile * kTileSize + groupThreadId.xy;

    float4 probeData = probesData.Load(int3(probeTile, 0));
    uint state = decodeProbeState(probeData);
    float attention = decodeProbeAttention(probeData);

    if (state == kDDGIProbeStateInactive)
    {
        float4 historyIrradiance = probesIrradianceHist.Load(int3(texel, 0));
        probesIrradianceOut[texel] = float4(historyIrradiance.rgb, 0);
        return;
    }

    float3 accumulated = float3(0, 0, 0);
    float accumulatedWeight = 0.0f;
    float2 octahedralCoord = ((float2(groupThreadId.xy) + 0.5f) / float(kTileSize)) * 2.0f - 1.0f;
    float3 texelDirection = flaxOctahedralDecode(octahedralCoord);
    for (uint r = 0; r < ddgi.raysCount; r++)
    {
        float4 traceData = probesTrace.Load(int3(uint2(r, activeSlot), 0));
        if (traceData.w >= 0.0f)
        {
            float3 rayDirection = flaxSphericalFibonacci(float(r), float(ddgi.raysCount));
            float rayWeight = saturate(dot(texelDirection, rayDirection));
            accumulated += traceData.rgb * rayWeight;
            accumulatedWeight += rayWeight;
        }
    }

    float3 irradiance = accumulatedWeight > 0.0f
        ? accumulated / accumulatedWeight : float3(0, 0, 0);
    float4 historyIrradiance = probesIrradianceHist.Load(int3(texel, 0));
    bool historyValid = historyIrradiance.a > 0.0f;
    float historyWeight = (state == kDDGIProbeStateActivated || !historyValid)
        ? 0.0f : ddgi.probeHistoryWeight;
    float3 historyLinear = pow(max(historyIrradiance.rgb, float3(0, 0, 0)),
                               ddgi.irradianceGamma);
    irradiance = lerp(irradiance, historyLinear, historyWeight);
    irradiance = pow(max(irradiance, float3(0, 0, 0)), 1.0f / ddgi.irradianceGamma);

    probesIrradianceOut[texel] = float4(irradiance, attention);
}
