// DDGI SampleProbe include-only module (Wave D3-1).
//
// Full 8-neighbor weighted irradiance sampling translated from LuxGI
// DDGICommon.glsl:163-233 sampleIrradiance (trilinear x smooth-backface x
// Chebyshev moment visibility x crush), plus textureCoordFromDirection
// (DDGICommon.glsl:141-158) for octahedral atlas addressing.
//
// Plan deviation note (user-sanctioned): LuxGI runs SampleProbe as a separate
// fullscreen compute pass writing an indirect-light texture; MGIF inlines the
// sampling directly into the lighting fragment shader (shader.light.slang),
// so this module carries no entry point and no bindings of its own.
//
// Probe positions are fetched through the probe-position buffer BDA (the same
// float4[totalProbes] buffer the ray trace pass consumes) instead of LuxGI's
// analytic gridToPosition; DDGIProbeVolume::uploadProbePositions writes
// origin + coord * spacing, so the values are identical while keeping a
// single source of truth across the DDGI passes.
//
// Include-only module: excluded from the compile_slang list in CMakeLists.txt
// (same pattern as ddgi_common.slang / sdf_common.slang).

#ifndef DDGI_SAMPLE_PROBE_SLANG
#define DDGI_SAMPLE_PROBE_SLANG

// Reuses octEncode/signNotZero and kDDGICommonPi. Include-chain note: any
// shader including this module must not define its own colliding
// signNotZero(float2) overload (shader.light.slang dropped its local copy).
#include "ddgi_common.slang"

// GPU-side volume constants for one sampleDDGIIrradiance call. Mirrors the
// LightParams ddgi* fields (shader_io.h); filled by the including shader.
struct DDGISampleVolume
{
    float3 gridOrigin;            // world-space position of probe (0,0,0)
    float probeSpacing;           // uniform probe spacing (all axes)
    int3 probeCounts;             // probe grid dims
    float normalBias;             // vBias = (N + 3*Wo) * normalBias
    float ddgiGamma;              // irradiance decode: pow(x, gamma*0.5) then square
    int irradianceSideLength;     // irradiance texels per probe (no border)
    int2 irradianceAtlasSize;     // full irradiance atlas extent in texels
    int depthSideLength;          // depth texels per probe (no border)
    int2 depthAtlasSize;          // full depth atlas extent in texels
    uint64_t probePositionAddress; // BDA of float4[totalProbes] world positions
};

float ddgiSquare(float v)
{
    return v * v;
}

// One-dimensional probe index of a grid coordinate (LuxGI
// gridCoordToProbeIndex, DDGICommon.glsl:136-139). Matches the
// DDGIProbeVolume position-buffer ordering (x + y*dims.x + z*dims.x*dims.y).
int ddgiGridCoordToProbeIndex(DDGISampleVolume volume, int3 probeCoords)
{
    return probeCoords.x + probeCoords.y * volume.probeCounts.x
        + probeCoords.z * volume.probeCounts.x * volume.probeCounts.y;
}

// Octahedral atlas UV of a direction within a probe tile (LuxGI
// textureCoordFromDirection, DDGICommon.glsl:141-158). Atlas layout contract
// shared with ddgi_common.slang: tile = side + 2 border texels, plus a
// 2-texel outer edge, probesPerRow recovered from the atlas width.
float2 ddgiTextureCoordFromDirection(float3 dir, int probeIndex, int2 atlasSize, int probeSideLength)
{
    const float2 normalizedOctCoordZeroOne = (octEncode(normalize(dir)) + float2(1.0, 1.0)) * 0.5;
    const float probeWithBorderSide = float(probeSideLength) + 2.0;
    const float2 octCoordNormalizedToTextureDimensions =
        (normalizedOctCoordZeroOne * float(probeSideLength)) / float2(atlasSize);
    const int probesPerRow = (atlasSize.x - 2) / int(probeWithBorderSide);
    const float2 probeTopLeftPosition =
        float2(float(probeIndex % probesPerRow) * probeWithBorderSide,
               float(probeIndex / probesPerRow) * probeWithBorderSide)
        + float2(2.0, 2.0);
    return probeTopLeftPosition / float2(atlasSize) + octCoordNormalizedToTextureDimensions;
}

// Full LuxGI sampleIrradiance weight chain (DDGICommon.glsl:163-233).
// P = shaded world position, N = world normal, Wo = surface-to-camera
// direction (LuxGI SampleProbe.comp:55: normalize(cameraPosition - P)).
// Returns world-space irradiance (already includes the 2*pi factor); the
// caller applies its own albedo/BRDF energy convention.
float3 sampleDDGIIrradiance(DDGISampleVolume volume, float3 P, float3 N, float3 Wo,
                            Sampler2D irradianceAtlas, Sampler2D depthAtlas)
{
    // Probe origins through the position-buffer BDA (same pattern as
    // ddgi_gi_sdf_rays.slang rayTraceUniforms.probePositionAddress).
    float4* probePositions = (float4*)volume.probePositionAddress;

    const int3 maxGridCoord = volume.probeCounts - int3(1, 1, 1);
    const int3 baseGridCoord = clamp(
        int3((P - volume.gridOrigin) / volume.probeSpacing), int3(0, 0, 0), maxGridCoord);
    const float3 baseProbePos =
        probePositions[ddgiGridCoordToProbeIndex(volume, baseGridCoord)].xyz;

    float3 sumIrradiance = float3(0.0, 0.0, 0.0);
    float sumWeight = 0.0;

    const float3 alpha =
        clamp((P - baseProbePos) / volume.probeSpacing, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));

    for (int i = 0; i < 8; ++i)
    {
        const int3 offset = int3(i, i >> 1, i >> 2) & int3(1, 1, 1);
        const int3 probeGridCoord = clamp(baseGridCoord + offset, int3(0, 0, 0), maxGridCoord);
        const int probeIdx = ddgiGridCoordToProbeIndex(volume, probeGridCoord);
        const float3 probePos = probePositions[probeIdx].xyz;

        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        float weight = 1.0;

        // Smooth backface test (DDGICommon.glsl:184-185).
        const float3 dirToProbe = normalize(probePos - P);
        weight *= ddgiSquare(max(0.0001, (dot(dirToProbe, N) + 1.0) * 0.5)) + 0.2;

        // Moment visibility test / Chebyshev (DDGICommon.glsl:191-207).
        const float3 vBias = (N + 3.0 * Wo) * volume.normalBias;
        const float3 probeToPoint = P - probePos + vBias;
        const float3 dir = normalize(-probeToPoint);

        float2 texCoord = ddgiTextureCoordFromDirection(
            -dir, probeIdx, volume.depthAtlasSize, volume.depthSideLength);
        const float dist = length(probeToPoint);

        const float2 temp = depthAtlas.SampleLevel(texCoord, 0.0).rg;
        const float mean = temp.x;
        const float variance = abs(ddgiSquare(temp.x) - temp.y);

        float chebyshevWeight = variance / (variance + ddgiSquare(max(dist - mean, 0.0)));
        chebyshevWeight = max(chebyshevWeight * chebyshevWeight * chebyshevWeight, 0.0);
        weight *= (dist <= mean) ? 1.0 : chebyshevWeight;

        weight = max(0.000001, weight);

        const float3 irradianceDir = N;
        texCoord = ddgiTextureCoordFromDirection(
            normalize(irradianceDir), probeIdx, volume.irradianceAtlasSize, volume.irradianceSideLength);
        float3 probeIrradiance = irradianceAtlas.SampleLevel(texCoord, 0.0).rgb;

        // Gamma decode half 1: pow(x, gamma * 0.5); the net result is squared
        // below (DDGICommon.glsl:217,230).
        probeIrradiance = pow(probeIrradiance,
                              float3(volume.ddgiGamma * 0.5, volume.ddgiGamma * 0.5, volume.ddgiGamma * 0.5));

        // Crush low weights cubically (DDGICommon.glsl:219-221).
        const float crushThreshold = 0.2;
        if (weight < crushThreshold)
        {
            weight *= weight * weight * (1.0 / ddgiSquare(crushThreshold));
        }

        weight *= trilinear.x * trilinear.y * trilinear.z;
        sumIrradiance += weight * probeIrradiance;
        sumWeight += weight;
    }

    float3 netIrradiance = sumIrradiance / max(sumWeight, 0.000001);
    netIrradiance *= netIrradiance;

    return 2.0 * kDDGICommonPi * netIrradiance;
}

#endif // DDGI_SAMPLE_PROBE_SLANG
