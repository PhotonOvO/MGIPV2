// flax_ddgi_common.slang — Flax-style DDGI helpers (FGI-013, FGI-043, FGI-060)
// Include-only module: probe index math, state encode/decode, cascade sampling.
// Ported from Flax DDGI.hlsl behavior. Uses VKDemo Slang conventions.

#ifndef FLAX_DDGI_COMMON_SLANG
#define FLAX_DDGI_COMMON_SLANG

#include "shader_io.h"

static const uint kDDGIProbeStateInactive   = 0;
static const uint kDDGIProbeStateActivated  = 1;
static const uint kDDGIProbeStateActive     = 2;
static const uint kDDGIProbeIrradianceResolution = 6;
static const uint kDDGIProbeDistanceResolution   = 14;
static const float kDDGIProbeAttentionMin   = 0.02f;
static const float kDDGIProbeAttentionMax   = 0.98f;
static const float kDDGIProbeHighPriorityAttention = 0.75f;
static const float kDDGICascadeBlendSize    = 2.0f;
static const float kFlaxDDGIPi              = 3.14159265359f;

// RTXGI bounds the directional distance distribution to 1.5 probe-cell
// diagonals. Keeping the horizon larger than a cell diagonal is important:
// an all-miss probe must remain visible everywhere inside its interpolation
// cell.
static const float kFlaxDDGIDistanceHorizonScale = 1.5f;
static const float kFlaxDDGIMinVisibility = 0.05f;
static const float kFlaxDDGIFrontFacingMinVisibility = 0.12f;
static const uint kFlaxDDGIHighPriorityBudgetNumerator = 3u;
static const uint kFlaxDDGIHighPriorityBudgetDenominator = 4u;
static const uint kFlaxEmissiveSampleCount = 4u;
static const float kFlaxEmissiveTransportScale = 0.8f;

float flaxWindowedInverseSquare(float distanceToLight, float sourceRange)
{
    float distanceRatio = distanceToLight / max(sourceRange, 0.0001f);
    float window = pow(saturate(1.0f - pow(distanceRatio, 4.0f)), 2.0f);
    return window / max(distanceToLight * distanceToLight, 0.01f);
}

float2 flaxEmissiveSampleOffset(uint sampleIndex)
{
    return float2((sampleIndex & 1u) != 0u ? 0.5f : -0.5f,
                  (sampleIndex & 2u) != 0u ? 0.5f : -0.5f);
}

float3 flaxEmissiveSamplePosition(FlaxDDGIRadianceSource source, uint sampleIndex)
{
    float2 offset = flaxEmissiveSampleOffset(sampleIndex);
    return source.positionAndRange.xyz
         + source.shapeAxisXAndHalfWidth.xyz * (offset.x * source.shapeAxisXAndHalfWidth.w)
         + source.shapeAxisYAndHalfHeight.xyz * (offset.y * source.shapeAxisYAndHalfHeight.w);
}

float3 flaxEmissiveSampleRadiance(
    FlaxDDGIRadianceSource source, float3 worldPosition, uint sampleIndex,
    out float3 lightDirection, out float distanceToLight)
{
    float3 toLight = flaxEmissiveSamplePosition(source, sampleIndex) - worldPosition;
    distanceToLight = length(toLight);
    lightDirection = toLight / max(distanceToLight, 0.0001f);
    float emitterCosine = saturate(dot(normalize(source.directionAndCone.xyz), -lightDirection));
    float area = 4.0f * max(source.shapeAxisXAndHalfWidth.w, 0.001f)
                      * max(source.shapeAxisYAndHalfHeight.w, 0.001f);
    return source.colorAndType.rgb * (emitterCosine * area / float(kFlaxEmissiveSampleCount))
         * flaxWindowedInverseSquare(distanceToLight, source.positionAndRange.w) * kFlaxEmissiveTransportScale;
}

bool flaxPointOnEmissiveSource(
    FlaxDDGIRadianceSource source, float3 worldPosition, float tolerance)
{
    if (uint(source.colorAndType.w + 0.5f) != LFlaxRadianceSourceEmissive)
        return false;
    float3 offset = worldPosition - source.positionAndRange.xyz;
    float planeDistance = abs(dot(offset, normalize(source.directionAndCone.xyz)));
    float axisXDistance = abs(dot(offset, normalize(source.shapeAxisXAndHalfWidth.xyz)));
    float axisYDistance = abs(dot(offset, normalize(source.shapeAxisYAndHalfHeight.xyz)));
    return planeDistance <= tolerance
        && axisXDistance <= source.shapeAxisXAndHalfWidth.w + tolerance
        && axisYDistance <= source.shapeAxisYAndHalfHeight.w + tolerance;
}

float flaxProbeDistanceLimit(FlaxDDGIData ddgi, uint cascadeIndex)
{
    float spacing = max(ddgi.probesOriginAndSpacing[cascadeIndex].w, 0.0001f);
    float cellDiagonal = length(float3(spacing, spacing, spacing));
    return max(min(ddgi.rayMaxDistance,
                   cellDiagonal * kFlaxDDGIDistanceHorizonScale), 0.0001f);
}

float flaxProbeRayStartOffset(FlaxDDGIData ddgi, uint cascadeIndex)
{
    float spacing = max(ddgi.probesOriginAndSpacing[cascadeIndex].w, 0.0001f);
    float voxelSize = max(ddgi.sdfBoundsMinAndVoxel.w, 0.0001f);
    // A sub-voxel offset avoids sampling the exact relocation boundary, while
    // the spacing cap prevents coarse SDFs from skipping a nearby thin wall.
    return min(voxelSize * 0.25f, spacing * 0.05f);
}

float flaxProbeSelfHitThreshold(FlaxDDGIData ddgi)
{
    return max(ddgi.sdfBoundsMinAndVoxel.w, 0.0001f);
}

uint2 flaxProbeTileInteriorTexel(uint2 tileTexel, uint interiorResolution)
{
    uint borderMax = interiorResolution + 1u;

    if (tileTexel.x == 0u)
    {
        if (tileTexel.y == 0u)
            return uint2(interiorResolution, interiorResolution);
        if (tileTexel.y == borderMax)
            return uint2(interiorResolution, 1u);
        return uint2(1u, borderMax - tileTexel.y);
    }
    if (tileTexel.x == borderMax)
    {
        if (tileTexel.y == 0u)
            return uint2(1u, interiorResolution);
        if (tileTexel.y == borderMax)
            return uint2(1u, 1u);
        return uint2(interiorResolution, borderMax - tileTexel.y);
    }
    if (tileTexel.y == 0u)
        return uint2(borderMax - tileTexel.x, 1u);
    if (tileTexel.y == borderMax)
        return uint2(borderMax - tileTexel.x, interiorResolution);
    return tileTexel;
}

float flaxDistanceHistoryWeight(
    FlaxDDGIData ddgi,
    uint cascadeIndex,
    uint probeState,
    bool historyValid,
    float currentMeanDistance,
    float historyMeanDistance)
{
    if (probeState == kDDGIProbeStateActivated || !historyValid)
        return 0.0f;

    float spacing = max(ddgi.probesOriginAndSpacing[cascadeIndex].w, 0.0001f);
    float voxelSize = max(ddgi.sdfBoundsMinAndVoxel.w, 0.0001f);
    float historyRejectionThreshold = max(spacing * 0.25f, voxelSize * 2.0f);
    float disagreement = abs(currentMeanDistance - historyMeanDistance);
    float historyConfidence = 1.0f - smoothstep(
        historyRejectionThreshold,
        historyRejectionThreshold * 2.0f,
        disagreement);
    return saturate(ddgi.probeHistoryWeight) * historyConfidence;
}

float flaxIrradianceHistoryWeight(
    FlaxDDGIData ddgi,
    uint probeState,
    bool historyValid,
    float3 currentIrradiance,
    float3 historyIrradiance)
{
    if (probeState == kDDGIProbeStateActivated || !historyValid)
        return 0.0f;

    float signal = max(max(max(currentIrradiance.x, currentIrradiance.y),
                           currentIrradiance.z),
                       max(max(historyIrradiance.x, historyIrradiance.y),
                           historyIrradiance.z));
    float historyRejectionThreshold = max(0.02f, signal * 0.35f);
    float disagreement = max(
        max(abs(currentIrradiance.x - historyIrradiance.x),
            abs(currentIrradiance.y - historyIrradiance.y)),
        abs(currentIrradiance.z - historyIrradiance.z));
    float historyConfidence = 1.0f - smoothstep(
        historyRejectionThreshold,
        historyRejectionThreshold * 2.0f,
        disagreement);
    return saturate(ddgi.probeHistoryWeight) * historyConfidence;
}
float3 flaxSphericalFibonacci(float rayIndex, float rayCount)
{
    float phiGolden = sqrt(5.0f) * 0.5f + 0.5f;
    float fraction = frac(rayIndex * (phiGolden - 1.0f));
    float phi = 2.0f * kFlaxDDGIPi * fraction;
    float cosTheta = 1.0f - (2.0f * rayIndex + 1.0f) / rayCount;
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 flaxOctahedralDecode(float2 encoded)
{
    float3 direction = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    if (direction.z < 0.0f)
    {
        float2 signs = float2(direction.x >= 0.0f ? 1.0f : -1.0f,
                              direction.y >= 0.0f ? 1.0f : -1.0f);
        direction.xy = (1.0f - abs(direction.yx)) * signs;
    }
    return normalize(direction);
}

float2 flaxOctahedralEncode(float3 direction)
{
    direction = normalize(direction);
    float l1 = abs(direction.x) + abs(direction.y) + abs(direction.z);
    float2 encoded = direction.xy / max(l1, 0.0001f);
    if (direction.z < 0.0f)
    {
        float2 signs = float2(direction.x >= 0.0f ? 1.0f : -1.0f,
                              direction.y >= 0.0f ? 1.0f : -1.0f);
        encoded = (1.0f - abs(encoded.yx)) * signs;
    }
    return encoded;
}

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
static const uint kDDGIProbeStateAnyValid = 3u;

bool getProbeUpdateSelection(
    FlaxDDGIData ddgi, RWStructuredBuffer<uint> activeProbes,
    uint cascadeIndex, uint updateSlot, out uint probeIndex, out uint requiredState)
{
    uint totalProbes = ddgi.probesCounts.x * ddgi.probesCounts.y * ddgi.probesCounts.z;
    uint activeCount = min(activeProbes[0], totalProbes);
    uint priorityCount = min(activeProbes[1], activeCount);
    uint regularCount = min(activeProbes[2], activeCount - priorityCount);
    uint priorityListBase = 4u + 2u * totalProbes;
    uint regularListBase = priorityListBase + totalProbes;
    if (activeCount == 0u || updateSlot >= totalProbes)
    {
        probeIndex = 0u;
        requiredState = kDDGIProbeStateInactive;
        return false;
    }

    // InitArgs redistributes the global budget after all cascade active counts
    // are known. Every trace/update shader consumes the same GPU allocation.
    uint updateCount = min(activeProbes[3], activeCount);
    if (updateSlot >= updateCount)
    {
        probeIndex = 0u;
        requiredState = kDDGIProbeStateInactive;
        return false;
    }

    // When the budget covers the active set, update every active probe exactly
    // once. This is the common smoke-test case and must not lose probes merely
    // because the priority list is shorter than the requested priority quota.
    if (updateCount >= activeCount)
    {
        if (updateSlot < priorityCount)
            probeIndex = activeProbes[priorityListBase + updateSlot];
        else
            probeIndex = activeProbes[regularListBase + (updateSlot - priorityCount)];
        requiredState = kDDGIProbeStateAnyValid;
        return probeIndex < totalProbes;
    }

    // Under a constrained budget, reserve most work for geometry-priority
    // probes, then fill any unused priority slots with regular probes. Both
    // lists are compact, so inactive logical-grid cells cannot waste dispatch
    // slots and priority/regular probes cannot race on the same atlas tile.
    uint prioritySlots = min(updateCount,
        (updateCount * kFlaxDDGIHighPriorityBudgetNumerator
         + kFlaxDDGIHighPriorityBudgetDenominator - 1u)
        / kFlaxDDGIHighPriorityBudgetDenominator);
    prioritySlots = min(prioritySlots, priorityCount);
    uint regularSlots = min(regularCount, updateCount - prioritySlots);
    prioritySlots += min(priorityCount - prioritySlots,
                         updateCount - prioritySlots - regularSlots);
    if (updateSlot < prioritySlots)
    {
        uint priorityOffset = ddgi.updateParams[cascadeIndex].z;
        uint prioritySlot = priorityCount == 0u ? 0u
            : (priorityOffset + updateSlot) % priorityCount;
        probeIndex = activeProbes[priorityListBase + prioritySlot];
        requiredState = kDDGIProbeStateAnyValid;
        return probeIndex < totalProbes;
    }

    if (regularSlots == 0u || regularCount == 0u)
    {
        probeIndex = 0u;
        requiredState = kDDGIProbeStateInactive;
        return false;
    }
    uint regularSlot = updateSlot - prioritySlots;
    if (regularSlot >= regularSlots)
    {
        probeIndex = 0u;
        requiredState = kDDGIProbeStateInactive;
        return false;
    }
    uint regularOffset = ddgi.updateParams[cascadeIndex].x;
    probeIndex = activeProbes[regularListBase + ((regularOffset + regularSlot) % regularCount)];
    requiredState = kDDGIProbeStateAnyValid;
    return probeIndex < totalProbes;
}

bool probeMatchesUpdateState(float4 probeData, uint requiredState)
{
    uint state = decodeProbeState(probeData);
    return requiredState == kDDGIProbeStateAnyValid
        ? state != kDDGIProbeStateInactive : state == requiredState;
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

// ProbeData packs physical (scrolled) coordinates as
// X = probeX + probeY * countX, Y = cascade * countZ + probeZ.
// Fallback coordinates are stored in logical grid space, so undo scrolling.
uint3 getLogicalProbeCoordsFromDataTexel(
    FlaxDDGIData ddgi, uint cascadeIndex, uint2 texelCoord)
{
    int3 counts = int3(ddgi.probesCounts);
    int3 scrolledCoords = int3(
        texelCoord.x % ddgi.probesCounts.x,
        texelCoord.x / ddgi.probesCounts.x,
        texelCoord.y % ddgi.probesCounts.z);
    int3 logicalCoords = (scrolledCoords - ddgi.probesScrollOffsets[cascadeIndex].xyz) % counts;
    logicalCoords = (logicalCoords + counts) % counts;
    return uint3(logicalCoords);
}

float3 getProbeWorldPosition(FlaxDDGIData ddgi, uint cascadeIndex, uint3 probeCoords)
{
    float3 origin = ddgi.probesOriginAndSpacing[cascadeIndex].xyz;
    float spacing = ddgi.probesOriginAndSpacing[cascadeIndex].w;
    float3 gridOffset = spacing * float3(ddgi.probesCounts - 1) * 0.5f;
    return origin + float3(probeCoords) * spacing - gridOffset;
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

bool fallbackCoordsValid(FlaxDDGIData ddgi, float3 data)
{
    if (any(data <= float3(0,0,0)))
        return false;
    uint3 coords = decodeFallbackCoords(data);
    return all(coords < ddgi.probesCounts);
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
    Sampler2D<float4> probesData,
    Sampler2D<float4> probesDistance,
    Sampler2D<float4> probesIrradiance,
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
    float validTrilinearWeight = 0.0f;
    float inactiveTrilinearWeight = 0.0f;
    float2 surfaceNormalOct = flaxOctahedralEncode(worldNormal);

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

        float2 irrUV = getProbeUV(ddgi, cascadeIndex, probeIndex, surfaceNormalOct,
                                  kDDGIProbeIrradianceResolution);
        float4 encodedIrradiance = probesIrradiance.SampleLevel(irrUV, 0);

        // Activated/inactive probes have no directional payload. Defer fallback
        // handling until the valid cage has been renormalized; otherwise an
        // inactive probe contributes weight=1 while a visible probe contributes
        // weight*visibility and the fallback can incorrectly dominate corners.
        if (state != kDDGIProbeStateActive || encodedIrradiance.a <= 0.0f)
        {
            inactiveTrilinearWeight += weight;
            continue;
        }
        validTrilinearWeight += weight;
        // The update pass stores a cosine-weighted radiance average. Its
        // hemispherical irradiance integral is pi times that average; using
        // 2*pi doubles all valid probe energy.
        float3 irrSample = pow(max(encodedIrradiance.rgb, float3(0,0,0)),
                               ddgi.irradianceGamma) * kFlaxDDGIPi;

        // Decode real probe position and compute visibility
        float3 probeWorldPos = decodeProbePosition(ddgi, probeData, cascadeIndex, probeIndex, probeCoords);
        float3 pointToProbe = probeWorldPos - biasedWorldPosition;
        float distToProbe = length(pointToProbe);
        float3 pointToProbeDir = pointToProbe / max(distToProbe, 0.0001f);
        float3 probeToPointDir = -pointToProbeDir;
        float2 probeToPointOct = flaxOctahedralEncode(probeToPointDir);

        // Smooth wrap shading with a non-zero floor. A hard surface-side gate
        // can reject all probes on one side of a cell while moment visibility
        // rejects the other side, producing a cell-aligned fallback band.
        float normalAlignment = dot(pointToProbeDir, worldNormal);
        float wrapShading = saturate((normalAlignment + 1.0f) * 0.5f);
        float backFaceWeight = wrapShading * wrapShading + 0.2f;

        // The distance atlas stores normalized raw moments (E[d], E[d^2]),
        // not variance. Raw moments can be blended temporally without losing
        // the between-frame mean-shift term.
        float2 probeUV = getProbeUV(ddgi, cascadeIndex, probeIndex,
                                     probeToPointOct, kDDGIProbeDistanceResolution);
        float2 distanceMoments = probesDistance.SampleLevel(probeUV, 0).xy;
        float distanceLimit = flaxProbeDistanceLimit(ddgi, cascadeIndex);
        float meanDistance = saturate(distanceMoments.x) * distanceLimit;
        float secondMoment = max(distanceMoments.y, 0.0f)
                           * distanceLimit * distanceLimit;
        float varianceFloor = max(distanceLimit * distanceLimit * 1.0e-6f,
                                  1.0e-6f);
        float variance = max(secondMoment - meanDistance * meanDistance,
                             varianceFloor);
        float visibilityDelta = max(distToProbe - meanDistance, 0.0f);
        float rawChebyshevWeight = variance /
            (variance + visibilityDelta * visibilityDelta);
        float conservativeVisibility = rawChebyshevWeight * rawChebyshevWeight
                                     * rawChebyshevWeight;
        // A probe in front of the surface cannot be hidden by that same
        // surface. Directional moment filtering near a concave edge can still
        // mix in an adjacent wall and report a false blocker, so relax only the
        // front-facing side while keeping the conservative cubic response for
        // back-facing probes where leak prevention matters.
        float frontFacing = saturate(normalAlignment);
        float chebyshevWeight = lerp(
            conservativeVisibility, rawChebyshevWeight, frontFacing);
        float minimumVisibility = lerp(
            kFlaxDDGIMinVisibility,
            kFlaxDDGIFrontFacingMinVisibility,
            frontFacing);
        float distVis = distToProbe <= meanDistance
            ? 1.0f : max(chebyshevWeight, minimumVisibility);

        // irrSample was decoded above after validating the probe payload.

        // Keep the interpolation continuous. Cubing low weights makes one
        // probe abruptly become the winner at cell boundaries.
        float contribWeight = weight * backFaceWeight * distVis;
        if (contribWeight <= 1.0e-8f)
            continue;
        irradiance += irrSample * contribWeight;
        totalWeight += contribWeight;
    }

    if (totalWeight > 1.0e-8f)
    {
        // Missing probes use the average modulation of the valid cage so their
        // fallback share is measured on the same scale as backface/visibility
        // weighted probe contributions.
        float averageContributionScale =
            totalWeight / max(validTrilinearWeight, 1.0e-8f);
        float fallbackWeight = inactiveTrilinearWeight * averageContributionScale;
        irradiance += ddgi.fallbackIrradiance.rgb * fallbackWeight;
        totalWeight += fallbackWeight;
        irradiance /= totalWeight;
    }
    else
        irradiance = ddgi.fallbackIrradiance.rgb;

    return irradiance;
}

// Main entry: sample cascaded DDGI irradiance with cascade blending
float3 sampleFlaxDDGIIrradiance(
    FlaxDDGIData ddgi,
    Sampler2D<float4> probesData,
    Sampler2D<float4> probesDistance,
    Sampler2D<float4> probesIrradiance,
    float3 worldPosition,
    float3 worldNormal,
    float3 worldToCameraDirection,
    float normalBias,
    float viewBias,
    float cascadeBlendDistance)
{
    float3 result = float3(0,0,0);
    float remainingWeight = 1.0f;

    float3 safeNormal = length(worldNormal) > 0.0001f
        ? normalize(worldNormal) : float3(0, 1, 0);
    float3 safeViewDirection = length(worldToCameraDirection) > 0.0001f
        ? normalize(worldToCameraDirection) : safeNormal;
    float3 biasedPos = worldPosition
        + safeNormal * normalBias
        + safeViewDirection * viewBias;

    for (uint c = 0; c < ddgi.cascadesCount; c++)
    {
        float spacing = ddgi.probesOriginAndSpacing[c].w;
        // Coverage and cross-cascade blending follow the continuous camera
        // origin. Probe lookup below remains anchored to the snapped grid.
        float3 blendOrigin = ddgi.blendOrigin[c].xyz;
        float3 extent = spacing * float3(ddgi.probesCounts - 1) * 0.5f;
        float3 edgeDistances = extent - abs(biasedPos - blendOrigin);
        if (any(edgeDistances < float3(0,0,0)))
            continue;

        float edgeDistance = min(min(edgeDistances.x, edgeDistances.y), edgeDistances.z);
        bool isLastCascade = c + 1u == ddgi.cascadesCount;
        float blendWidth = max(cascadeBlendDistance * spacing, spacing);
        float cascadeWeight = isLastCascade
            ? remainingWeight : remainingWeight * saturate(edgeDistance / blendWidth);
        if (cascadeWeight <= 1.0e-6f)
            continue;

        float3 cascadeIrradiance = sampleFlaxDDGIIrradianceCascade(
            ddgi, probesData, probesDistance, probesIrradiance,
            worldPosition, safeNormal, c, spacing, biasedPos);
        result += cascadeIrradiance * cascadeWeight;
        remainingWeight = max(remainingWeight - cascadeWeight, 0.0f);
        if (remainingWeight <= 1.0e-6f)
            break;
    }

    if (remainingWeight > 1.0e-6f)
        result += ddgi.fallbackIrradiance.rgb * remainingWeight;

    return result * ddgi.indirectLightingIntensity;
}

#endif // FLAX_DDGI_COMMON_SLANG
