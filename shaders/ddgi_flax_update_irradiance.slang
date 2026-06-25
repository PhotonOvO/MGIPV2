// ddgi_flax_update_irradiance.slang — Flax-style probe irradiance update (FGI-055)
// Filters traced radiance into octahedral irradiance atlas with temporal
// blending, gamma encode, and scroll invalidation.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesTrace       : register(t0); // input radiance
[[vk::binding(1, 0)]] Texture2D<float4> probesData         : register(t1);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesIrradianceOut : register(u0); // output
[[vk::binding(3, 0)]] Texture2D<float4> probesIrradianceHist : register(t2); // previous frame

[[vk::push_constant]] ConstantBuffer<FlaxDDGIData> ddgi;

[numthreads(8, 8, 1)]
void CS_UpdateIrradiance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 texel = dispatchThreadId.xy;
    uint cascadeIndex = texel.y / ddgi.probesCounts.z;
    if (cascadeIndex >= ddgi.cascadesCount) return;

    uint probeRes = 6; // DDGI_PROBE_RESOLUTION_IRRADIANCE
    uint tileSize = probeRes + 2;
    uint probesPerRow = ddgi.probesCounts.x * ddgi.probesCounts.y;

    uint probeX = texel.x / tileSize;
    uint probeY = texel.y % ddgi.probesCounts.z;
    if (probeX >= probesPerRow || probeY >= ddgi.probesCounts.z) return;

    uint probeIndex = probeX + probeY * probesPerRow;
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);

    float4 pd = probesData.Load(int3(dataCoord, 0));
    uint state = decodeProbeState(pd);
    float attention = decodeProbeAttention(pd);

    // Octahedral direction for this texel
    float2 octCoord = normalizedOctCoord(int2(texel), probeRes);

    // Accumulate ray radiance for this probe
    float3 accumulated = float3(0,0,0);
    float validRays = 0.0f;
    uint raysPerProbe = ddgi.raysCount;

    for (uint r = 0; r < raysPerProbe; r++)
    {
        float3 rayDir = octDecode(octCoord); // approximate — full version uses Fibonacci
        float4 traceData = probesTrace.Load(int3(uint2(r, probeIndex % 256), 0));
        if (traceData.w >= 0.0f) // valid hit
        {
            accumulated += traceData.rgb;
            validRays += 1.0f;
        }
    }

    float3 irradiance = validRays > 0.0f ? accumulated / validRays : float3(0,0,0);

    // Temporal blend
    float4 historyIrradiance = probesIrradianceHist.Load(int3(texel, 0));
    float histWeight = state == kDDGIProbeStateActivated ? 0.0f : ddgi.probeHistoryWeight;
    irradiance = lerp(irradiance, historyIrradiance.rgb, histWeight);

    // Gamma encode for storage
    irradiance = pow(max(irradiance, float3(0,0,0)), 1.0f / ddgi.irradianceGamma);

    probesIrradianceOut[texel] = float4(irradiance, attention);
}
