// FlaxGI diagnostic atlas. Each 256x256 tile visualizes one pipeline contract.
// The atlas is sampled by ImGui through the backend-neutral DebugInteropBridge.

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
    uint viewMask;
    uint surfaceAtlasAvailable;
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

uint2 sourceCoord(float2 uv, uint2 dimensions)
{
    return min(uint2(uv * float2(dimensions)), dimensions - 1u);
}

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
            color = toneMapDebug(trace.rgb);
        }
        else
        {
            const float normalizedDistance = trace.w >= 0.0f
                ? saturate(trace.w / max(ddgiData[0].rayMaxDistance, 0.001f)) : 1.0f;
            color = trace.w < 0.0f ? float3(0.02f, 0.04f, 0.3f)
                                  : float3(normalizedDistance, 1.0f - normalizedDistance, 0.05f);
        }
    }
    else if (tileIndex == 5u)
    {
        uint width, height;
        probesDistance.GetDimensions(width, height);
        const float2 distanceMoments = probesDistance.Load(int3(sourceCoord(uv, uint2(width, height)), 0));
        color = float3(saturate(distanceMoments.x), saturate(distanceMoments.y * 4.0f), 0.05f);
    }
    else if (tileIndex == 6u)
    {
        uint width, height;
        probesIrradiance.GetDimensions(width, height);
        const float4 irradiance = probesIrradiance.Load(int3(sourceCoord(uv, uint2(width, height)), 0));
        const float3 linearIrradiance = pow(max(irradiance.rgb, float3(0, 0, 0)), ddgiData[0].irradianceGamma);
        color = toneMapDebug(linearIrradiance);
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
