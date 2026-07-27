// FlaxGI diagnostic atlas. Each 256x256 tile visualizes one pipeline contract.
// The atlas is sampled by ImGui through the backend-neutral ImGuiRhiRenderer.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> debugAtlas         : register(u0);
[[vk::binding(1, 0)]] Texture3D<float> globalSDF             : register(t0);
[[vk::binding(2, 0)]] Texture3D<float4> globalAlbedo         : register(t1);
[[vk::binding(3, 0)]] Texture2D<float4> probesData           : register(t2);
[[vk::binding(4, 0)]] Texture2D<float4> probesTrace          : register(t3);
[[vk::binding(5, 0)]] Texture2D<float2> probesDistance       : register(t4);
[[vk::binding(6, 0)]] Texture2D<float4> probesIrradiance     : register(t5);
[[vk::binding(7, 0)]] Texture2D<float4> surfaceAtlasLighting : register(t6);
[[vk::binding(8, 0)]] SamplerState linearSampler             : register(s0);
[[vk::binding(9, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData : register(t7);

struct FlaxGIDebugViewPush
{
    float sdfSlice;
    float exposure;
    float chromaBoost;
    uint viewMask;
    uint surfaceAtlasAvailable;
    uint probeOverviewPage;
    uint padding0;
    uint padding1;
};

[[vk::push_constant]] ConstantBuffer<FlaxGIDebugViewPush> debugPush;

static const uint kTileSize = 256;
static const uint kAtlasColumns = 4;
static const uint kAtlasRows = 2;

float3 toneMapDebug(float3 value)
{
    value = max(value * max(debugPush.exposure, 0.001f), float3(0, 0, 0));
    return value / (value + 1.0f);
}

float3 boostDebugChroma(float3 toneMappedValue)
{
    static const float3 kRec709Luminance = float3(0.2126f, 0.7152f, 0.0722f);
    const float3 value = saturate(toneMappedValue);
    const float luminance = dot(value, kRec709Luminance);
    const float3 neutral = float3(luminance, luminance, luminance);
    const float3 chroma = value - neutral;
    float safeBoost = clamp(debugPush.chromaBoost, 1.0f, 2.0f);
    [unroll]
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        if (chroma[channel] > 1e-5f)
            safeBoost = min(safeBoost, (1.0f - luminance) / chroma[channel]);
        else if (chroma[channel] < -1e-5f)
            safeBoost = min(safeBoost, luminance / -chroma[channel]);
    }
    return saturate(neutral + chroma * max(safeBoost, 0.0f));
}

uint2 sourceCoord(float2 uv, uint2 dimensions)
{
    return min(uint2(uv * float2(dimensions)), dimensions - 1u);
}

float4 sampleProbeIrradianceDirection(
    FlaxDDGIData ddgi, uint cascadeIndex, uint physicalProbeIndex, float3 direction)
{
    const float2 probeUV = getProbeUV(
        ddgi, cascadeIndex, physicalProbeIndex,
        flaxOctahedralEncode(normalize(direction)),
        kDDGIProbeIrradianceResolution);
    const float4 encoded =
        probesIrradiance.SampleLevel(linearSampler, probeUV, 0);
    return float4(
        pow(max(encoded.rgb, float3(0, 0, 0)), ddgi.irradianceGamma), encoded.a);
}

float4 sampleProbeIrradianceOverview(
    FlaxDDGIData ddgi, uint cascadeIndex, uint physicalProbeIndex)
{
    float4 irradiance = float4(0, 0, 0, 0);
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(1, 0, 0));
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(-1, 0, 0));
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(0, 1, 0));
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(0, -1, 0));
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(0, 0, 1));
    irradiance += sampleProbeIrradianceDirection(
        ddgi, cascadeIndex, physicalProbeIndex, float3(0, 0, -1));
    return irradiance * (1.0f / 6.0f);
}

static const float3 kProbeOverviewInactiveColor = float3(0.0f, 0.0f, 0.0f);
static const float3 kProbeOverviewActivatedColor = float3(0.14f, 0.14f, 0.14f);
static const float3 kProbeOverviewInvalidIrradianceColor = float3(0.035f, 0.035f, 0.035f);

[numthreads(8, 8, 1)]
void CS_FlaxGIDebugViews(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const uint2 atlasExtent = uint2(kAtlasColumns * kTileSize, kAtlasRows * kTileSize);
    if (any(pixel >= atlasExtent)) return;

    const uint2 tileCoord = pixel / kTileSize;
    const uint tileIndex = tileCoord.y * kAtlasColumns + tileCoord.x;
    const float2 uv = (float2(pixel % kTileSize) + 0.5f) / float(kTileSize);

    if ((debugPush.viewMask & (1u << tileIndex)) == 0u)
    {
        const float checker = ((pixel.x / 16u + pixel.y / 16u) & 1u) != 0u ? 0.025f : 0.01f;
        debugAtlas[pixel] = float4(checker, checker, checker, 1.0f);
        return;
    }

    float3 color = float3(0, 0, 0);
    if (tileIndex == 0u)
    {
        const float encoded = globalSDF.SampleLevel(linearSampler, float3(uv, saturate(debugPush.sdfSlice)), 0);
        const float signedDistance = encoded * 2.0f - 1.0f;
        color = signedDistance < 0.0f
            ? float3(saturate(-signedDistance), 0.05f, 0.05f)
            : float3(0.05f, saturate(1.0f - signedDistance), saturate(signedDistance));
    }
    else if (tileIndex == 1u)
    {
        color = globalAlbedo.SampleLevel(linearSampler, float3(uv, saturate(debugPush.sdfSlice)), 0).rgb;
    }
    else if (tileIndex == 2u)
    {
        uint width, height;
        probesData.GetDimensions(width, height);
        const float4 probe = probesData.Load(int3(sourceCoord(uv, uint2(width, height)), 0));
        const uint state = decodeProbeState(probe);
        const float relocation = saturate(length(probe.xyz) * 2.0f);
        color = state == kDDGIProbeStateInactive ? float3(0.5f, 0.02f, 0.02f)
              : state == kDDGIProbeStateActivated ? float3(1.0f, 0.75f, 0.02f)
              : float3(0.02f, 0.8f, 0.1f);
        color = lerp(color, float3(0.1f, 0.35f, 1.0f), relocation * 0.65f);
    }
    else if (tileIndex == 3u || tileIndex == 4u)
    {
        uint width, height;
        probesTrace.GetDimensions(width, height);
        const float4 trace = probesTrace.Load(int3(sourceCoord(uv, uint2(width, height)), 0));
        if (tileIndex == 3u)
        {
            color = boostDebugChroma(toneMapDebug(trace.rgb));
        }
        else
        {
            const float distanceLimit = flaxProbeDistanceLimit(ddgiData[0], 0u);
            const float normalizedDistance = saturate(trace.w / distanceLimit);
            const bool openRay = normalizedDistance >= 0.999f;
            color = openRay ? float3(0.02f, 0.15f, 0.8f)
                            : float3(normalizedDistance, 1.0f - normalizedDistance, 0.05f);
        }
    }
    else if (tileIndex == 5u)
    {
        uint width, height;
        probesDistance.GetDimensions(width, height);
        const float2 distanceMoments = probesDistance.Load(int3(sourceCoord(uv, uint2(width, height)), 0));
        const float normalizedMean = saturate(distanceMoments.x);
        const float normalizedSecondMoment = saturate(distanceMoments.y);
        const float normalizedVariance = max(
            normalizedSecondMoment - normalizedMean * normalizedMean, 0.0f);
        color = float3(normalizedMean, sqrt(normalizedVariance),
                       normalizedSecondMoment);
    }
    else if (tileIndex == 6u)
    {
        const FlaxDDGIData ddgi = ddgiData[0];
        const uint cascadeCount = max(ddgi.cascadesCount, 1u);
        const uint probesPerCascade =
            ddgi.probesCounts.x * ddgi.probesCounts.y * ddgi.probesCounts.z;
        const uint totalProbeCount = probesPerCascade * cascadeCount;
        const uint probesPerPage = kTileSize * kTileSize;
        const uint pageCount = max(
            (totalProbeCount + probesPerPage - 1u) / probesPerPage, 1u);
        const uint pageIndex = min(debugPush.probeOverviewPage, pageCount - 1u);
        const uint2 tilePixel = pixel % kTileSize;
        const uint pageProbeIndex = tilePixel.y * kTileSize + tilePixel.x;
        const uint logicalProbeSlot = pageIndex * probesPerPage + pageProbeIndex;

        if (logicalProbeSlot >= totalProbeCount)
        {
            color = float3(0.005f, 0.005f, 0.005f);
        }
        else
        {
            const uint cascadeIndex = logicalProbeSlot / probesPerCascade;
            const uint cascadeProbeIndex = logicalProbeSlot % probesPerCascade;
            const uint3 logicalProbeCoords =
                getProbeCoords(ddgi, cascadeProbeIndex);
            const uint physicalProbeIndex =
                getScrollingProbeIndex(ddgi, cascadeIndex, logicalProbeCoords);
            const uint2 probeDataTexel =
                getProbeTexelCoords(ddgi, cascadeIndex, physicalProbeIndex);
            const float4 probeData =
                probesData.Load(int3(probeDataTexel, 0));
            const uint probeState = decodeProbeState(probeData);

            if (probeState == kDDGIProbeStateInactive)
            {
                color = kProbeOverviewInactiveColor;
            }
            else if (probeState == kDDGIProbeStateActivated)
            {
                color = kProbeOverviewActivatedColor;
            }
            else
            {
                const float4 probeIrradiance = sampleProbeIrradianceOverview(
                    ddgi, cascadeIndex, physicalProbeIndex);
                color = probeState == kDDGIProbeStateActive && probeIrradiance.a > 0.0f
                    ? boostDebugChroma(toneMapDebug(probeIrradiance.rgb))
                    : kProbeOverviewInvalidIrradianceColor;
            }
        }
    }
    else
    {
        if (debugPush.surfaceAtlasAvailable == 0u)
        {
            const bool stripe = ((pixel.x + pixel.y) / 12u & 1u) != 0u;
            color = stripe ? float3(0.45f, 0.0f, 0.45f) : float3(0.08f, 0.0f, 0.08f);
        }
        else
        {
            uint width, height;
            surfaceAtlasLighting.GetDimensions(width, height);
            const float3 lighting = surfaceAtlasLighting.Load(int3(sourceCoord(uv, uint2(width, height)), 0)).rgb;
            color = toneMapDebug(lighting);
        }
    }

    debugAtlas[pixel] = float4(color, 1.0f);
}
