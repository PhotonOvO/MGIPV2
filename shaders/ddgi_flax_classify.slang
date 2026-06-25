// ddgi_flax_classify.slang — Flax-style DDGI probe classification (FGI-050)
// Uses flax_ddgi_common helpers for probe math, state encode/decode.
// SDF distance check, relocation, empty-area fallback probes, active list append.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData        : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes   : register(u1);
[[vk::binding(2, 0)]] Texture3D<float> globalSDFTex           : register(t0);
[[vk::binding(3, 0)]] SamplerState linearSampler              : register(s0);

[[vk::push_constant]] ConstantBuffer<FlaxDDGIData> ddgi;

// Cascade index is set via specialization constant or push constant
#ifndef DDGI_CLASSIFY_CASCADE
static const uint kCascadeIndex = 0;
#else
static const uint kCascadeIndex = DDGI_CLASSIFY_CASCADE;
#endif

[numthreads(32, 1, 1)]
void CS_Classify(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIndex = dispatchThreadId.x;
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    uint totalProbes = probesPerPlane * ddgi.probesCounts.y;
    if (probeIndex >= totalProbes) return;

    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, kCascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, kCascadeIndex, scrolledIdx);
    float probesSpacing = ddgi.probesOriginAndSpacing[kCascadeIndex].w;
    float3 probeBasePos = getProbeWorldPosition(ddgi, kCascadeIndex, probeCoords);

    // Check if higher-quality cascade covers this probe
    if (kCascadeIndex > 0)
    {
        uint prevC = kCascadeIndex - 1;
        float prevSpacing = ddgi.probesOriginAndSpacing[prevC].w;
        float3 prevOrigin = ddgi.probesOriginAndSpacing[prevC].xyz;
        float3 prevExtent = float3(ddgi.probesCounts - 1) * (prevSpacing * 0.5f);
        prevExtent -= prevSpacing * kDDGICascadeBlendSize * 2.0f;
        float3 prevCascadeWeight = prevExtent - abs(probeBasePos - prevOrigin);
        if (min(min(prevCascadeWeight.x, prevCascadeWeight.y), prevCascadeWeight.z) > 0.1f)
        {
            probesData[dataCoord] = encodeProbeData(float3(0,0,0), kDDGIProbeStateInactive, 0.0f);
            return;
        }
    }

    // Read scroll offsets to determine if probe was scrolled
    int3 scrollClears = ddgi.probesScrollOffsets[kCascadeIndex].xyz;
    bool wasScrolled = false;
    for (uint p = 0; p < 3; p++)
    {
        int newCoord = int(probeCoords[p]) + scrollClears[p];
        if (newCoord < 0 || newCoord >= int(ddgi.probesCounts[p])) wasScrolled = true;
        newCoord = int(probeCoords[p]) - scrollClears[p];
        if (newCoord < 0 || newCoord >= int(ddgi.probesCounts[p])) wasScrolled = true;
    }

    // Load existing probe data
    float4 probeData = probesData[dataCoord];
    float attention = decodeProbeAttention(probeData);
    uint state = decodeProbeState(probeData);
    float3 probeOffset = probeData.xyz * probesSpacing;

    if (wasScrolled || state == kDDGIProbeStateInactive)
    {
        probeOffset = float3(0,0,0);
        attention = 1.0f; // wake-up
    }

    float3 probePosition = probeBasePos + probeOffset;

    // Sample SDF distance
    float3 sdfUvw = (probePosition - float3(-16,-16,-16)) / float3(32,32,32);
    float sdf = globalSDFTex.SampleLevel(linearSampler, sdfUvw, 0) * 32.0f;
    float sdfDist = abs(sdf);
    float voxelLimit = 0.25f * 0.8f;
    float distanceLimit = probesSpacing * 2.3f;
    uint stateOld = state;

    // Empty area fallback (sparse probes for dynamic objects)
    if (sdf > probesSpacing * ddgi.probesCounts.x * 0.3f)
    {
        probeOffset = float3(0,0,0);
        state = wasScrolled ? kDDGIProbeStateActivated : kDDGIProbeStateActive;
        attention = kDDGIProbeAttentionMin;
    }
    else if (sdfDist > distanceLimit + length(probeOffset))
    {
        // Too far from geometry
        probeOffset = float3(0,0,0);
        state = kDDGIProbeStateInactive;
        attention = 0.0f;
    }
    else
    {
        // Active probe
        state = kDDGIProbeStateActive;
        float3 viewToProbe = probePosition - ddgi.viewPos;
        float distToProbe = length(viewToProbe);
        viewToProbe /= max(distToProbe, 0.0001f);
        attention *= lerp(0.1f, 1.0f, saturate(dot(viewToProbe, float3(0,0,-1))));
        attention *= lerp(1.0f, 0.5f, saturate(sdfDist / voxelLimit));
        attention = clamp(attention, kDDGIProbeAttentionMin, kDDGIProbeAttentionMax);

        // Relocate away from surfaces
        if (sdf <= voxelLimit)
        {
            float eps = 1.0f / 128.0f;
            float xp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(eps,0,0), 0);
            float xn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(eps,0,0), 0);
            float yp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,eps,0), 0);
            float yn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,eps,0), 0);
            float zp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,0,eps), 0);
            float zn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,0,eps), 0);
            float3 gradient = float3(xp - xn, yp - yn, zp - zn);
            float3 sdfNormal = length(gradient) > 1e-6f ? normalize(gradient) : float3(0,1,0);

            float speed = stateOld != kDDGIProbeStateActive ? 1.0f : 0.3f;
            float3 offsetToSet = probeOffset + sdfNormal * ((sdf + voxelLimit) * speed);
            float relocateLimit = probesSpacing * 0.5f;
            if (length(offsetToSet) < relocateLimit)
                probeOffset = offsetToSet;
            else
                probeOffset = float3(0,0,0); // reset if too far
        }
    }

    // Write probe data
    probesData[dataCoord] = encodeProbeData(probeOffset / probesSpacing, state, attention);

    // Append active probe
    if (state != kDDGIProbeStateInactive)
    {
        uint slot;
        InterlockedAdd(activeProbes[0], 1, slot);
        if (slot < 4095)
            activeProbes[1 + slot] = probeIndex;
    }
}
