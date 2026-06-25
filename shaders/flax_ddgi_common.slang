// flax_ddgi_common.slang — Flax-style DDGI helpers (FGI-013, FGI-043, FGI-060)
// Include-only module: probe index math, state encode/decode, cascade sampling.
// Ported from Flax DDGI.hlsl behavior. Uses VKDemo Slang conventions.

#ifndef FLAX_DDGI_COMMON_SLANG
#define FLAX_DDGI_COMMON_SLANG

#include "shader_io.h"

static const uint kDDGIProbeStateInactive   = 0;
static const uint kDDGIProbeStateActivated  = 1;
static const uint kDDGIProbeStateActive     = 2;
static const float kDDGIProbeAttentionMin   = 0.02f;
static const float kDDGIProbeAttentionMax   = 0.98f;
static const float kDDGICascadeBlendSize    = 2.0f;

// --- Probe index / coordinate math ---

uint getProbeIndex(FlaxDDGIData ddgi, uint3 probeCoords)
{
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    return probeCoords.y * probesPerPlane + probeCoords.x + ddgi.probesCounts.x * probeCoords.z;
}

uint3 getProbeCoords(FlaxDDGIData ddgi, uint probeIndex)
{
    uint3 coords;
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    coords.x = probeIndex % ddgi.probesCounts.x;
    coords.y = probeIndex / probesPerPlane;
    coords.z = (probeIndex / ddgi.probesCounts.x) % ddgi.probesCounts.z;
    return coords;
}

uint2 getProbeTexelCoords(FlaxDDGIData ddgi, uint cascadeIndex, uint probeIndex)
{
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    uint planeIndex = probeIndex / probesPerPlane;
    uint gridSpaceX = probeIndex % ddgi.probesCounts.x;
    uint gridSpaceY = probeIndex / ddgi.probesCounts.x;
    return uint2(
        gridSpaceX + planeIndex * ddgi.probesCounts.x,
        gridSpaceY % ddgi.probesCounts.z + uint(cascadeIndex) * ddgi.probesCounts.z);
}

uint getScrollingProbeIndex(FlaxDDGIData ddgi, uint cascadeIndex, uint3 probeCoords)
{
    int3 offset = int3(ddgi.probesCounts) + ddgi.probesScrollOffsets[cascadeIndex].xyz;
    uint3 scrolled = (probeCoords + uint3(offset)) % ddgi.probesCounts;
    return getProbeIndex(ddgi, scrolled);
}

float3 getProbeWorldPosition(FlaxDDGIData ddgi, uint cascadeIndex, uint3 probeCoords)
{
    float3 origin = ddgi.probesOriginAndSpacing[cascadeIndex].xyz;
    float spacing = ddgi.probesOriginAndSpacing[cascadeIndex].w;
    float3 gridOffset = spacing * float3(ddgi.probesCounts - 1) * 0.5f;
    float3 scrollOffset = float3(ddgi.probesScrollOffsets[cascadeIndex].xyz) * spacing;
    return origin + float3(probeCoords) * spacing - gridOffset + scrollOffset;
}

// --- Probe state encode / decode (matches Flax) ---

float4 encodeProbeData(float3 offset, uint state, float attention)
{
    float a = saturate(attention) * 2.0f - 1.0f;
    if (state == kDDGIProbeStateInactive)  a = -1.0f;
    if (state == kDDGIProbeStateActivated) a =  1.0f;
    return float4(offset, a);
}

float decodeProbeAttention(float4 probeData)
{
    if (probeData.w <= -1.0f) return 0.0f;
    if (probeData.w >=  1.0f) return 1.0f;
    return probeData.w * 0.5f + 0.5f;
}

uint decodeProbeState(float4 probeData)
{
    if (probeData.w <= -1.0f) return kDDGIProbeStateInactive;
    if (probeData.w >=  1.0f) return kDDGIProbeStateActivated;
    return kDDGIProbeStateActive;
}

float3 decodeProbePosition(FlaxDDGIData ddgi, float4 probeData, uint cascadeIndex, uint probeIndex, uint3 probeCoords)
{
    float3 pos = probeData.xyz * ddgi.probesOriginAndSpacing[cascadeIndex].w;
    return pos + getProbeWorldPosition(ddgi, cascadeIndex, probeCoords);
}

// Fallback coords encoding (maps integer coords into [0,~1] float range)
float3 encodeFallbackCoords(uint3 coords)
{
    return (float3(coords) + 1.0f) / 128.0f;
}

uint3 decodeFallbackCoords(float3 data)
{
    return uint3(data * 128.0f - 1.0f);
}

bool fallbackCoordsValid(float3 data)
{
    return length(data) > 0.0f;
}

// --- Texture UV helpers ---

float2 getProbeUV(FlaxDDGIData ddgi, uint cascadeIndex, uint probeIndex,
                   float2 octahedralCoords, uint resolution)
{
    uint2 coords = getProbeTexelCoords(ddgi, cascadeIndex, probeIndex);
    float probeTexelSize = float(resolution + 2);
    float2 textureSize = float2(
        ddgi.probesCounts.x * ddgi.probesCounts.y * probeTexelSize,
        ddgi.probesCounts.z * ddgi.cascadesCount * probeTexelSize);
    float2 uv = float2(coords.x, coords.y) * probeTexelSize + probeTexelSize * 0.5f;
    uv += octahedralCoords * float2(resolution * 0.5f);
    return uv / textureSize;
}

// --- Cascade sampling (FGI-060) ---

// Sample DDGI irradiance for a single cascade with trilinear blending.
// worldPosition: position to shade
// worldNormal: surface normal for bias
// biasedWorldPosition = worldPosition + worldNormal * normalBias
float3 sampleFlaxDDGIIrradianceCascade(
    FlaxDDGIData ddgi,
    Texture2D<float4> probesData,
    Texture2D<float4> probesDistance,
    Texture2D<float4> probesIrradiance,
    SamplerState linearSampler,
    float3 worldPosition,
    float3 worldNormal,
    uint cascadeIndex,
    float probesSpacing,
    float3 biasedWorldPosition)
{
    // Clamp cascade index
    if (cascadeIndex >= ddgi.cascadesCount)
        cascadeIndex = ddgi.cascadesCount - 1;

    uint3 probeCountsEnd = ddgi.probesCounts - 1;

    // Get base probe coordinates from biased position
    float3 probesOrigin = ddgi.probesOriginAndSpacing[cascadeIndex].xyz;
    float3 probesExtent = probesSpacing * float3(ddgi.probesCounts - 1) * 0.5f;

    uint3 baseProbeCoords = uint3(clamp(
        (biasedWorldPosition - probesOrigin + probesExtent) / probesSpacing,
        float3(0,0,0), float3(probeCountsEnd)));

    float3 baseProbeWorldPos = getProbeWorldPosition(ddgi, cascadeIndex, baseProbeCoords);
    float3 biasAlpha = saturate((biasedWorldPosition - baseProbeWorldPos) / probesSpacing);

    // Trilinear interpolation over 8 nearest probes
    float3 irradiance = float3(0,0,0);
    float totalWeight = 0.0f;

    for (uint i = 0; i < 8; i++)
    {
        uint3 offset = uint3(i, i >> 1, i >> 2) & 1;
        uint3 probeCoords = clamp(baseProbeCoords + offset, uint3(0,0,0), probeCountsEnd);
        uint probeIndex = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);

        // Compute trilinear weight
        float3 w = float3(1,1,1) - abs(float3(offset) - biasAlpha);
        float weight = w.x * w.y * w.z;

        // Load probe data
        uint2 texelCoord = getProbeTexelCoords(ddgi, cascadeIndex, probeIndex);
        float4 probeData = probesData.Load(int3(texelCoord, 0));
        uint state = decodeProbeState(probeData);

        // Inactive probe -> use fallback
        if (state == kDDGIProbeStateInactive)
        {
            uint3 fallbackCoords = decodeFallbackCoords(probeData.xyz);
            probeIndex = getScrollingProbeIndex(ddgi, cascadeIndex, fallbackCoords);
            texelCoord = getProbeTexelCoords(ddgi, cascadeIndex, probeIndex);
            probeData = probesData.Load(int3(texelCoord, 0));
        }

        // Decode real probe position and compute visibility
        float3 probeWorldPos = decodeProbePosition(ddgi, probeData, cascadeIndex, probeIndex, probeCoords);
        float3 toProbe = probeWorldPos - worldPosition;
        float distToProbe = length(toProbe);
        float3 probeDir = toProbe / max(distToProbe, 0.0001f);

        // Back-face test
        float backFaceWeight = saturate(dot(probeDir, worldNormal) + 0.2f);

        // Distance visibility (simplified — full version uses moments)
        float distVis = 1.0f;
        float2 probeUV = getProbeUV(ddgi, cascadeIndex, probeIndex,
                                     float2(0,0), 6); // irradiance res
        float4 distData = probesDistance.SampleLevel(
            linearSampler, probeUV, 0);
        float meanDist = distData.x * ddgi.rayMaxDistance;
        if (distToProbe > meanDist * 1.5f)
            distVis = 0.2f;

        // Sample irradiance
        float3 probeDirLocal = probeDir;
        // Octahedral encode probeDir -> UV
        float l1 = abs(probeDirLocal.x) + abs(probeDirLocal.y) + abs(probeDirLocal.z);
        float2 oct = probeDirLocal.xy / l1;
        if (probeDirLocal.z < 0.0f)
        {
            float sx = probeDirLocal.x >= 0.0f ? 1.0f : -1.0f;
            float sy = probeDirLocal.y >= 0.0f ? 1.0f : -1.0f;
            oct = (float2(1,1) - abs(oct.yx)) * float2(sx, sy);
        }

        float2 irrUV = getProbeUV(ddgi, cascadeIndex, probeIndex, oct, 6);
        float3 irrSample = probesIrradiance.SampleLevel(linearSampler, irrUV, 0).rgb;
        irrSample = pow(irrSample, ddgi.irradianceGamma); // gamma decode

        float contribWeight = weight * backFaceWeight * distVis;
        irradiance += irrSample * contribWeight;
        totalWeight += contribWeight;
    }

    if (totalWeight > 0.0001f)
        irradiance /= totalWeight;
    else
        irradiance = ddgi.fallbackIrradiance.rgb;

    return irradiance;
}

// Main entry: sample cascaded DDGI irradiance with cascade blending
float3 sampleFlaxDDGIIrradiance(
    FlaxDDGIData ddgi,
    Texture2D<float4> probesData,
    Texture2D<float4> probesDistance,
    Texture2D<float4> probesIrradiance,
    SamplerState linearSampler,
    float3 worldPosition,
    float3 worldNormal,
    float normalBias,
    float cascadeBlendDistance)
{
    float3 result = float3(0,0,0);
    float totalWeight = 0.0f;

    float3 biasedPos = worldPosition + worldNormal * normalBias;

    for (uint c = 0; c < ddgi.cascadesCount; c++)
    {
        float spacing = ddgi.probesOriginAndSpacing[c].w;

        float3 cascadeIrradiance = sampleFlaxDDGIIrradianceCascade(
            ddgi, probesData, probesDistance, probesIrradiance,
            linearSampler,
            worldPosition, worldNormal, c, spacing, biasedPos);

        // Cascade blending weight (fade at cascade boundary)
        float blendWeight = 1.0f;
        if (c > 0)
        {
            float distFromView = length(worldPosition - ddgi.viewPos);
            float cascadeDist = spacing * float(ddgi.probesCounts.x) * 0.5f;
            blendWeight = 1.0f - saturate((distFromView - cascadeDist + cascadeBlendDistance) / cascadeBlendDistance);
        }

        result += cascadeIrradiance * blendWeight;
        totalWeight += blendWeight;
    }

    if (totalWeight > 0.0001f)
        result /= totalWeight;
    else
        result = ddgi.fallbackIrradiance.rgb;

    return result * ddgi.indirectLightingIntensity;
}

#endif // FLAX_DDGI_COMMON_SLANG
